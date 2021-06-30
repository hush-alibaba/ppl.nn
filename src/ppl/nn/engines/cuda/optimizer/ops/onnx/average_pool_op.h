#ifndef _ST_HPC_PPL_NN_ENGINES_CUDA_OPTIMIZER_OPS_ONNX_AVERAGE_POOL_OP_H_
#define _ST_HPC_PPL_NN_ENGINES_CUDA_OPTIMIZER_OPS_ONNX_AVERAGE_POOL_OP_H_

#include "ppl/nn/engines/cuda/optimizer/opt_kernel.h"

#include "ppl/nn/params/onnx/pooling_param.h"

namespace ppl { namespace nn { namespace cuda {

class AveragePoolOp final : public CudaOptKernel {
public:
    AveragePoolOp(const ir::Node* node) : CudaOptKernel(node) {}
    KernelImpl* CreateKernelImpl() const override;
    ppl::common::RetCode Init(const OptKernelOptions&) override;
    ppl::common::RetCode Finalize(const OptKernelOptions& options) override;
    void* GetParam() override {
        return (void*)&param_;
    };

private:
    ppl::nn::common::PoolingParam param_;
};

}}} // namespace ppl::nn::cuda

#endif