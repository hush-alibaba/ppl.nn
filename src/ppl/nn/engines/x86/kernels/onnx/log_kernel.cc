#include "ppl/nn/engines/x86/kernels/onnx/log_kernel.h"
#include "ppl/kernel/x86/fp32/log.h"

namespace ppl { namespace nn { namespace x86 {

ppl::common::RetCode LogKernel::DoExecute(KernelExecContext* ctx) {
    auto input = ctx->GetInput<TensorImpl>(0);
    auto output = ctx->GetOutput<TensorImpl>(0);

    PPLNN_X86_DEBUG_TRACE("Op: %s\n", GetName().c_str());
    PPLNN_X86_DEBUG_TRACE("Input [input]:\n");
    PPL_X86_TENSOR_PRINT_DEBUG_MSG(input);
    PPLNN_X86_DEBUG_TRACE("Output [output]:\n");
    PPL_X86_TENSOR_PRINT_DEBUG_MSG(output);
    PPLNN_X86_DEBUG_TRACE("isa: %u\n", GetISA());

    const auto data_type = input->GetShape().GetDataType();

    if (data_type == ppl::common::DATATYPE_FLOAT32) {
        return ppl::kernel::x86::log_fp32(&input->GetShape(), input->GetBufferPtr<float>(),
                                          output->GetBufferPtr<float>());
    } else {
        LOG(ERROR) << "unsupported datatype: " << ppl::common::GetDataTypeStr(data_type) << ".";
    }

    return ppl::common::RC_UNSUPPORTED;
}

}}} // namespace ppl::nn::x86