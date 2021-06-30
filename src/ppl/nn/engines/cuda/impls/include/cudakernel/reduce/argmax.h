#ifndef PPLCUDA_REDUCE_ARGMAX_H_
#define PPLCUDA_REDUCE_ARGMAX_H_
#include "cudakernel/reduce/reduce_helper.h"
#include "ppl/common/retcode.h"
#include "ppl/nn/common/tensor_shape.h"

ReduceMode GetReduceMode(PPLReduceDimDes des);

ppl::common::RetCode PPLCUDAArgMaxForwardImp(
    cudaStream_t stream,
    PPLReduceDimDes des,
    const ppl::nn::TensorShape* input_shape,
    const void* input,
    const ppl::nn::TensorShape* output_shape,
    void* output);
#endif