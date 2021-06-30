#include "ppl/nn/engines/cuda/optimizer/ops/onnx/leaky_relu_op.h"

#include "ppl/nn/common/logger.h"
#include "ppl/nn/engines/cuda/kernels/onnx/leaky_relu_kernel.h"
#include "ppl/nn/oputils/onnx/reshape_leaky_relu.h"

using namespace std;
using namespace ppl::common;
using namespace ppl::nn::common;

namespace ppl { namespace nn { namespace cuda {

void LeakyReluOp::CopyParam(void*& param) {
    if (param == nullptr) {
        param = new LeakyReLUParam();
    }
    *(LeakyReLUParam*)param = param_;
    return;
}

RetCode LeakyReluOp::Init(const OptKernelOptions& options) {
    auto status = GenericLoadParam<LeakyReLUParam>(options, &param_);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "load param failed: " << GetRetCodeStr(status);
        return status;
    }

    infer_type_func_ = [this](InputOutputInfo* info, datatype_t type) -> RetCode {
        return type != DATATYPE_UNKNOWN ? InferDefaultType(info, type) : InferInheritedType(info);
    };

    infer_dims_func_ = [this](InputOutputInfo* info) -> RetCode {
        return oputils::ReshapeLeakyReLU(info, nullptr);
    };

    return RC_SUCCESS;
}

RetCode LeakyReluOp::Finalize(const OptKernelOptions& options) {
    auto status = SetCommonParam(options);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "load common param failed: " << GetRetCodeStr(status);
        return status;
    }

    return RC_SUCCESS;
}

KernelImpl* LeakyReluOp::CreateKernelImpl() const {
    return CreateKernelImplWithParam<LeakyReluKernel>(&param_);
}

}}} // namespace ppl::nn::cuda