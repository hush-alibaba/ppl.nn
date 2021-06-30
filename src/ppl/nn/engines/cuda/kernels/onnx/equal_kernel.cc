#include "ppl/nn/engines/cuda/kernels/onnx/equal_kernel.h"

#include "cudakernel/arithmetic/relation.h"

namespace ppl { namespace nn { namespace cuda {

ppl::common::RetCode EqualKernel::DoExecute(KernelExecContext* ctx) {
    auto input0 = ctx->GetInput<TensorImpl>(0);
    auto input1 = ctx->GetInput<TensorImpl>(1);
    auto output = ctx->GetOutput<TensorImpl>(0);

    ppl::common::RetCode status =
        PPLCUDARelationEqualForwardImp(GetStream(), &input0->GetShape(), input0->GetBufferPtr(), &input1->GetShape(),
                                       input1->GetBufferPtr(), &output->GetShape(), output->GetBufferPtr<bool>());
    return status;
}

}}} // namespace ppl::nn::cuda