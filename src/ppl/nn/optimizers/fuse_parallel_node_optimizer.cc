#include <vector>
#include <map>

#include "ppl/nn/optimizers/fuse_parallel_node_optimizer.h"
#include "ppl/nn/models/op_info_manager.h"
#include "ppl/nn/common/logger.h"
using namespace std;
using namespace ppl::common;

namespace ppl { namespace nn {

inline bool IsGraphOutput(const ir::Graph* graph, edgeid_t edge_id) {
    for (uint32_t i = 0; i < graph->topo->GetOutputCount(); i++) {
        if (graph->topo->GetOutput(i) == edge_id) {
            return true;
        }
    }
    return false;
}

static bool CanFuseAsOneNode(const ir::Graph* graph, const ir::Node* node_0, const ir::Node* node_1) {
    if (node_0 == node_1 || node_0->GetId() == node_1->GetId()) {
        return false;
    }
    if (!(node_0->GetType() == node_1->GetType())) {
        return false;
    }

    if (node_0->GetInputCount() != node_1->GetInputCount()) {
        return false;
    }
    if (node_0->GetExtraInputCount() != node_1->GetExtraInputCount()) {
        return false;
    }
    if (node_0->GetOutputCount() != node_1->GetOutputCount()) {
        return false;
    }

    for (uint32_t i = 0; i < node_0->GetInputCount(); i++) {
        if (node_0->GetInput(i) != node_1->GetInput(i)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < node_0->GetExtraInputCount(); i++) {
        if (node_0->GetExtraInput(i) != node_1->GetExtraInput(i)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < node_0->GetOutputCount(); i++) {
        if (IsGraphOutput(graph, node_0->GetOutput(i))) {
            return false;
        }
    }
    for (uint32_t i = 0; i < node_1->GetOutputCount(); i++) {
        if (IsGraphOutput(graph, node_1->GetOutput(i))) {
            return false;
        }
    }

    auto op_info = OpInfoManager::Instance()->Find(node_0->GetType().domain, node_0->GetType().name);
    if (!op_info) {
        return true;
    }

    auto param_it_0 = graph->data->attrs.find(node_0->GetId());
    auto param_it_1 = graph->data->attrs.find(node_1->GetId());
    if (param_it_0 == graph->data->attrs.end() || param_it_1 == graph->data->attrs.end()) {
        return false;
    }

    return op_info->param_equal(param_it_0->second.get(), param_it_1->second.get());
}

// fuse parallel nodes which have same inputs & same params
ppl::common::RetCode FuseParallelNodeOptimizer::Optimize(ir::Graph* graph) const {
    static const std::vector<ir::Node::Type> skipped_node_types{
        ir::Node::Type{"", "Conv"}, // cannot compare param correctly
        ir::Node::Type{"", "Reshape"}, // cannot compare param correctly
        ir::Node::Type{"", "If"}, // has subgraph
        ir::Node::Type{"", "Loop"} // has subgraph
    };

    for (auto edge_it = graph->topo->CreateEdgeIter(); edge_it->IsValid(); edge_it->Forward()) {
        auto edge = edge_it->Get();

        std::map<ir::Node*, std::vector<ir::Node*>> fuse_op_groups;
        for (auto node_it = edge->CreateConsumerIter(); node_it.IsValid(); node_it.Forward()) {
            auto node = graph->topo->GetNodeById(node_it.Get());
            auto node_type = node->GetType();

            bool skipped = false;
            for (uint32_t i = 0; i < skipped_node_types.size(); i++) {
                if (node_type == skipped_node_types[i]) {
                    skipped = true;
                    break;
                }
            }
            if (skipped) {
                continue;
            }

            bool find_group = false;
            for (auto it = fuse_op_groups.begin(); it != fuse_op_groups.end(); ++it) {
                if (CanFuseAsOneNode(graph, node, it->first)) {
                    it->second.push_back(node);
                    find_group = true;
                    break;
                }
            }
            if (!find_group) {
                fuse_op_groups.emplace(node, std::vector<ir::Node*>());
            }
        }

        for (auto it = fuse_op_groups.begin(); it != fuse_op_groups.end(); ++it) {
            auto merge_node = it->first;
            auto to_merge_nodes = it->second;
            for (uint32_t i = 0; i < to_merge_nodes.size(); i++) {
                auto to_merge_node = to_merge_nodes[i];
                for (uint32_t j = 0; j < to_merge_node->GetInputCount(); j++) {
                    auto input_edge = graph->topo->GetEdgeById(to_merge_node->GetInput(j));
                    if (input_edge) {
                        input_edge->DelConsumer(to_merge_node->GetId());
                    }
                }
                for (uint32_t j = 0; j < to_merge_node->GetOutputCount(); j++) {
                    auto output_edge = graph->topo->GetEdgeById(to_merge_node->GetOutput(j));
                    for (auto successor_it = output_edge->CreateConsumerIter(); successor_it.IsValid();
                         successor_it.Forward()) {
                        auto successor_node = graph->topo->GetNodeById(successor_it.Get());
                        successor_node->ReplaceInput(output_edge->GetId(), merge_node->GetOutput(j));
                        successor_node->ReplaceExtraInput(output_edge->GetId(),
                                                          merge_node->GetOutput(j)); // TODO: check if has error
                        graph->topo->GetEdgeById(merge_node->GetOutput(j))->AddConsumer(successor_node->GetId());
                    }
                }

                for (uint32_t j = 0; j < to_merge_node->GetOutputCount(); j++) {
                    graph->topo->DelEdgeById(to_merge_node->GetOutput(j));
                }
                graph->topo->DelNodeById(to_merge_node->GetId());
            }
        }
    }

    return RC_SUCCESS;
}

}} // namespace ppl::nn