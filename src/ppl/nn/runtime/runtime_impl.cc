// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "ppl/nn/common/logger.h"
#include "ppl/nn/engines/engine.h"
#include "ppl/nn/runtime/runtime_impl.h"
#include "ppl/nn/runtime/sequential_scheduler.h"
#include "ppl/nn/runtime/runtime_internal_conf.h"
#include "ppl/nn/utils/utils.h"
#include <stdarg.h>
using namespace std;
using namespace ppl::common;

namespace ppl { namespace nn {

RuntimeImpl::~RuntimeImpl() {
    // NOTE: released before SharedResource
    sched_.reset();
    graph_.Clear();
    graph_info_.reset();
    engctx_.clear();
}

static EngineContext* FindOrCreateEngineContext(const string& graph_name, const EngineContextOptions& options,
                                                EngineImpl* engine, map<EngineImpl*, EngineContext*>* eng2ctx,
                                                vector<unique_ptr<EngineContext>>* engctx) {
    auto ref = eng2ctx->find(engine);
    if (ref != eng2ctx->end()) {
        return ref->second;
    }

    auto ctx = engine->CreateEngineContext(graph_name, options);
    if (ctx) {
        engctx->emplace_back(unique_ptr<EngineContext>(ctx));
        eng2ctx->insert(make_pair(engine, ctx));
    }

    return ctx;
}

static void InitEngineContextOptions(const RuntimeOptions& rt_opt, EngineContextOptions* opt) {
    opt->mm_policy = rt_opt.mm_policy;
}

static RetCode InitRuntimeGraphKernels(const ir::GraphTopo* topo, const RuntimeGraphInfo& info,
                                       const RuntimeOptions& options, vector<unique_ptr<EngineContext>>* engctx,
                                       RuntimeGraph* graph) {
    graph->nodeid2kernel.resize(topo->GetMaxNodeId());

    EngineContextOptions engctx_options;
    InitEngineContextOptions(options, &engctx_options);

    map<EngineImpl*, EngineContext*> eng2ctx;
    for (auto it = info.kernels.begin(); it != info.kernels.end(); ++it) {
        auto ctx = FindOrCreateEngineContext(topo->GetName(), engctx_options, it->engine, &eng2ctx, engctx);
        if (!ctx) {
            LOG(ERROR) << "create context of engine[" << it->engine->GetName() << "] failed.";
            return RC_OTHER_ERROR;
        }

        auto impl = it->op->CreateKernelImpl();
        if (!impl) {
            LOG(ERROR) << "create kernel[" << it->op->GetNode()->GetName() << "] failed.";
            return RC_OTHER_ERROR;
        }

        impl->SetDevice(ctx->GetDevice());
        graph->nodeid2kernel[it->op->GetNode()->GetId()].reset(impl);
    }

    return RC_SUCCESS;
}

static KernelImpl* FindKernelByName(const vector<unique_ptr<KernelImpl>>& kernels, const string& name) {
    for (auto it = kernels.begin(); it != kernels.end(); ++it) {
        auto kernel = it->get();
        if (kernel && kernel->GetName() == name) {
            return kernel;
        }
    }
    return nullptr;
}

static RetCode InitRuntimeGraphInputs(const ir::GraphTopo* topo, const RuntimeGraphInfo& info,
                                      utils::GenericCpuDevice* cpu_device, RuntimeGraph* graph) {
    graph->inputs.reserve(topo->GetInputCount());

    for (uint32_t i = 0; i < topo->GetInputCount(); ++i) {
        auto eid = topo->GetInput(i);
        auto edge = topo->GetEdgeById(eid);

        auto ret_pair = graph->tensors.insert(make_pair(eid, TensorImpl(edge, TENSORTYPE_RESERVED)));
        auto tensor = &ret_pair.first->second;

        if (ret_pair.second) {
            auto consumer_iter = edge->CreateConsumerIter();
            if (!consumer_iter.IsValid()) {
                // some edges may be used only by graph itself, e.g. `cond` of Loop
                tensor->SetDevice(cpu_device);
            } else {
                for (; consumer_iter.IsValid(); consumer_iter.Forward()) {
                    auto consumer = topo->GetNodeById(consumer_iter.Get());
                    if (utils::IsPplConverterNode(consumer)) {
                        continue;
                    }

                    auto kernel = FindKernelByName(graph->nodeid2kernel, consumer->GetName());
                    if (!kernel) {
                        LOG(ERROR) << "cannot find consumer[" << consumer->GetName() << "] of [" << edge->GetName()
                                   << "]";
                        return RC_NOT_FOUND;
                    }
                    tensor->SetDevice(kernel->GetDevice());
                }
            }

            // ONNX supports reshaping inputs in runtime stage
            auto shape_ref = info.shapes.find(edge->GetId());
            if (shape_ref != info.shapes.end()) {
                tensor->GetShape() = shape_ref->second;
            }
        }

        graph->inputs.push_back(tensor);
    }

    return RC_SUCCESS;
}

static RetCode InitRuntimeGraphExtraInputs(const ir::GraphTopo* topo, const RuntimeGraphInfo& info,
                                           RuntimeGraph* graph) {
    graph->extra_inputs.reserve(topo->GetExtraInputCount());

    for (uint32_t i = 0; i < topo->GetExtraInputCount(); ++i) {
        auto eid = topo->GetExtraInput(i);
        auto edge = topo->GetEdgeById(eid);

        auto ret_pair = graph->tensors.insert(make_pair(eid, TensorImpl(edge, TENSORTYPE_RESERVED)));
        auto tensor = &ret_pair.first->second;

        if (ret_pair.second) {
            for (auto it = edge->CreateConsumerIter(); it.IsValid(); it.Forward()) {
                auto consumer = topo->GetNodeById(it.Get());
                if (utils::IsPplConverterNode(consumer)) {
                    continue;
                }

                auto kernel = FindKernelByName(graph->nodeid2kernel, consumer->GetName());
                if (!kernel) {
                    LOG(ERROR) << "cannot find consumer[" << consumer->GetName() << "] of [" << edge->GetName() << "]";
                    return RC_NOT_FOUND;
                }
                tensor->SetDevice(kernel->GetDevice());
            }

            auto shape_ref = info.shapes.find(edge->GetId());
            if (shape_ref != info.shapes.end()) {
                tensor->GetShape() = shape_ref->second;
            }
        }

        graph->extra_inputs.push_back(tensor);
    }

    return RC_SUCCESS;
}

RetCode InitRuntimeGraphOutputs(const ir::GraphTopo* topo, const RuntimeGraphInfo& info, RuntimeGraph* graph) {
    graph->outputs.reserve(topo->GetOutputCount());

    for (uint32_t i = 0; i < topo->GetOutputCount(); ++i) {
        auto eid = topo->GetOutput(i);
        auto edge = topo->GetEdgeById(eid);

        auto ret_pair = graph->tensors.insert(make_pair(eid, TensorImpl(edge, TENSORTYPE_NORMAL)));
        auto tensor = &ret_pair.first->second;

        if (ret_pair.second) {
            auto producer_id = edge->GetProducer();
            if (producer_id != INVALID_NODEID) {
                auto producer = topo->GetNodeById(producer_id);
                auto kernel = FindKernelByName(graph->nodeid2kernel, producer->GetName());
                if (!kernel) {
                    LOG(ERROR) << "cannot find producer[" << producer->GetName() << "] of [" << edge->GetName() << "]";
                    return RC_NOT_FOUND;
                }
                tensor->SetDevice(kernel->GetDevice());
            }

            auto shape_ref = info.shapes.find(edge->GetId());
            if (shape_ref != info.shapes.end()) {
                tensor->GetShape() = shape_ref->second;
            }
        }

        graph->outputs.push_back(tensor);
    }

    return RC_SUCCESS;
}

static RetCode InitRuntimeGraphConstants(const ir::GraphTopo* topo, const RuntimeGraphInfo& info, RuntimeGraph* graph) {
    auto constants = &graph->constants;
    auto tensors = &graph->tensors;

    constants->reserve(topo->GetConstantCount());

    for (auto x = info.constants.begin(); x != info.constants.end(); ++x) {
        auto eid = x->first;
        auto edge = topo->GetEdgeById(eid);
        if (!edge) {
            LOG(ERROR) << "cannot find edge info of constant[" << eid << "]";
            return RC_NOT_FOUND;
        }
        auto ret_pair = tensors->insert(make_pair(eid, TensorImpl(edge, TENSORTYPE_RESERVED)));
        auto tensor = &ret_pair.first->second;

        if (ret_pair.second) {
            tensor->GetShape() = x->second.GetShape();
            tensor->SetBuffer(x->second.GetBufferDesc(), x->second.GetDevice());
        }

        constants->push_back(tensor);
    }

    return RC_SUCCESS;
}

static bool HasExtraInputFrom(const ir::Node* successor, const ir::Node* parent) {
    for (uint32_t j = 0; j < successor->GetExtraInputCount(); ++j) {
        auto extra_input = successor->GetExtraInput(j);
        for (uint32_t i = 0; i < parent->GetOutputCount(); ++i) {
            if (parent->GetOutput(i) == extra_input) {
                return true;
            }
        }
    }
    return false;
}

static RetCode InitRuntimeGraphKernelBarrierFlags(const ir::GraphTopo* topo, RuntimeGraph* graph) {
    graph->kernel_barrier_flag.resize(topo->GetMaxNodeId(), false);

    for (uint32_t nid = 0; nid < graph->nodeid2kernel.size(); ++nid) {
        auto kernel = graph->nodeid2kernel[nid].get();
        if (!kernel) {
            continue;
        }

        auto device = kernel->GetDevice();
        auto successors = topo->FindSuccessors(nid);
        for (auto s = successors.begin(); s != successors.end(); ++s) {
            auto successor = graph->nodeid2kernel[*s].get();
            /*
              kernel needs a barrier when one of successors:
              - runs on another devices, or
              - has at least one extra input, which means this successor has subgraph(s),
                from its parent.
            */
            if ((successor->GetDevice() != device) || HasExtraInputFrom(successor->GetNode(), kernel->GetNode())) {
                graph->kernel_barrier_flag[nid] = true;
                break;
            }
        }
    }

    /* leaf nodes of graph need to be synchronized */
    for (uint32_t i = 0; i < graph->outputs.size(); ++i) {
        auto output = graph->outputs[i];
        auto producer_id = output->GetEdge()->GetProducer();
        if (producer_id != INVALID_NODEID) {
            graph->kernel_barrier_flag[producer_id] = true;
            break;
        }
    }

    return RC_SUCCESS;
}

static void InitRuntimeGraphBarriers(uint64_t max_edge_id, RuntimeGraph* graph) {
    graph->edgeid2barrier.resize(max_edge_id);
    for (uint32_t i = 0; i < graph->kernel_barrier_flag.size(); ++i) {
        if (graph->kernel_barrier_flag[i]) {
            auto kernel = graph->nodeid2kernel[i].get();
            auto barrier = kernel->GetDevice()->CreateBarrier();
            auto node = kernel->GetNode();

            // all outputs share the same barrier from their parent
            for (uint32_t j = 0; j < node->GetOutputCount(); ++j) {
                graph->edgeid2barrier[node->GetOutput(j)] = barrier;
            }
        }
    }
}

RetCode RuntimeImpl::InitRuntimeGraph(const ir::GraphTopo* topo, const RuntimeGraphInfo& info,
                                      const RuntimeOptions& options, RuntimeGraph* graph) {
    auto status = InitRuntimeGraphKernels(topo, info, options, &engctx_, graph);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitRuntimeGraphKernels failed: " << GetRetCodeStr(status);
        return status;
    }

    status = InitRuntimeGraphConstants(topo, info, graph);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitRuntimeGraphConstants failed: " << GetRetCodeStr(status);
        return status;
    }

    status = InitRuntimeGraphInputs(topo, info, &cpu_device_, graph);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitRuntimeGraphInputs failed: " << GetRetCodeStr(status);
        return status;
    }

    status = InitRuntimeGraphExtraInputs(topo, info, graph);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitRuntimeGraphExtraInputs failed: " << GetRetCodeStr(status);
        return status;
    }

    status = InitRuntimeGraphOutputs(topo, info, graph);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitRuntimeGraphOutputs failed: " << GetRetCodeStr(status);
        return status;
    }

    status = InitRuntimeGraphKernelBarrierFlags(topo, graph);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitRuntimeGraphBarriers failed: " << GetRetCodeStr(status);
        return status;
    }

    InitRuntimeGraphBarriers(topo->GetMaxEdgeId(), graph);

    return RC_SUCCESS;
}

static bool CheckOptions(const RuntimeOptions& options) {
    if (options.mm_policy != MM_BETTER_PERFORMANCE && options.mm_policy != MM_LESS_MEMORY) {
        LOG(ERROR) << "invalid memory management policy [" << (uint32_t)options.mm_policy << "]";
        return false;
    }

    return true;
}

RetCode RuntimeImpl::Init(const RuntimeOptions& options, const shared_ptr<ir::GraphTopo>& topo,
                          const shared_ptr<const RuntimeGraphInfo>& info,
                          const shared_ptr<const RuntimeAuxInfo>& aux_info,
                          const shared_ptr<utils::SharedResource>& resource) {
    if (!CheckOptions(options)) {
        return RC_INVALID_VALUE;
    }

    resource_ = resource;
    graph_info_ = info;
    aux_info_ = aux_info;
    topo_ = topo;

    profiler_.Init(&conf_, &graph_, aux_info.get());

    auto status = InitRuntimeGraph(topo.get(), *info, options, &graph_);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "InitRuntimeGraph failed: " << GetRetCodeStr(status);
        return status;
    }

    sched_.reset(new SequentialScheduler());
    return sched_->Init(topo.get(), aux_info.get(), &graph_);
}

RetCode RuntimeImpl::Run() {
    return sched_->Run(&profiler_);
}

RetCode RuntimeImpl::Sync() {
    for (uint32_t i = 0; i < GetOutputCount(); ++i) {
        auto output = GetOutputTensorImpl(i);
        auto barrier = graph_.edgeid2barrier[output->GetEdge()->GetId()];
        if (barrier) {
            auto status = barrier->Sync();
            if (status != RC_SUCCESS) {
                LOG(ERROR) << "sync tensor[" << output->GetName() << "] failed: " << GetRetCodeStr(status);
                return status;
            }
        }
    }
    return RC_SUCCESS;
}

RetCode RuntimeImpl::GetProfilingStatistics(ProfilingStatistics* stat) const {
#ifdef PPLNN_ENABLE_KERNEL_PROFILING
    return profiler_.GetProfilingStatistics(stat);
#else
    LOG(ERROR) << "this version does not support profiling.";
    return RC_UNSUPPORTED;
#endif
}

/* -------------------------------------------------------------------------- */

RetCode RuntimeImpl::SetProfilingFlag(RuntimeImpl* rt, va_list args) {
#ifdef PPLNN_ENABLE_KERNEL_PROFILING
    auto flag = va_arg(args, uint32_t);
    bool profiling_flag = (flag > 0);
    rt->conf_.profiling_flag = profiling_flag;

    if (profiling_flag) {
        rt->profiler_.StartProfiling(rt->topo_->GetMaxNodeId());
    } else {
        rt->profiler_.StopProfiling();
    }

    return RC_SUCCESS;
#else
    LOG(ERROR) << "this version does not support profiling.";
    return RC_UNSUPPORTED;
#endif
}

RuntimeImpl::ConfHandlerFunc RuntimeImpl::conf_handlers_[] = {
    RuntimeImpl::SetProfilingFlag,
};

RetCode RuntimeImpl::Configure(uint32_t option, ...) {
    if (option >= RUNTIME_CONF_MAX) {
        LOG(ERROR) << "invalid option[" << option << "] >= [" << RUNTIME_CONF_MAX << "]";
        return RC_INVALID_VALUE;
    }

    va_list args;
    va_start(args, option);
    auto status = conf_handlers_[option](this, args);
    va_end(args);

    return status;
}

}} // namespace ppl::nn
