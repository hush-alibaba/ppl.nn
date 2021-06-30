#include "ppl/nn/models/onnx/model_parser.h"
#include "ppl/nn/models/onnx/graph_parser.h"
#include "ppl/nn/common/logger.h"
#include "ppl/common/file_mapping.h"

// large proto file support
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

using namespace std;
using namespace ppl::common;

namespace ppl { namespace nn { namespace onnx {

static bool ParseFromBinaryBuffer(const char* buf, size_t buf_len, google::protobuf::MessageLite* pb_model) {
    if (!buf) {
        LOG(ERROR) << "buf ptr is nullptr.";
        return false;
    }

    if (buf_len == 0) {
        LOG(ERROR) << "buf len is 0.";
        return false;
    }

    google::protobuf::io::CodedInputStream cis((uint8_t*)buf, buf_len);
    cis.SetTotalBytesLimit(INT_MAX, INT_MAX);
    bool ok = pb_model->ParseFromCodedStream(&cis);

    return ok;
}

RetCode ModelParser::Parse(const char* buf, size_t buf_len, ir::Graph* graph) {
    ::onnx::ModelProto pb_model;
    if (!ParseFromBinaryBuffer(buf, buf_len, &pb_model)) {
        LOG(ERROR) << "load onnx model from model buffer failed.";
        return RC_OTHER_ERROR;
    }

    for (int i = 0; i < pb_model.opset_import_size(); ++i) {
        const string& domain = pb_model.opset_import(i).domain();
        int64_t version = pb_model.opset_import(i).version();
        // only supports onnx opset version >= 11
        if (domain.empty() && version < 11) {
            LOG(ERROR) << "unsupported opset [" << domain << ":" << version << "]";
            return RC_UNSUPPORTED;
        }
    }

    if (pb_model.graph().quantization_annotation_size() > 0) {
        LOG(ERROR) << "quantization in ONNX model is not supported now.";
        return RC_UNSUPPORTED;
    }

    GraphParser graph_parser;
    auto status = graph_parser.Parse(pb_model.graph(), graph);
    if (status != RC_SUCCESS) {
        LOG(ERROR) << "parse graph failed: " << GetRetCodeStr(status);
        return status;
    }

    if (graph->topo->GetExtraInputCount() > 0) {
        auto topo = graph->topo.get();
        LOG(ERROR) << "unresolved extra input of graph[" << topo->GetName() << "]:";
        for (uint32_t i = 0; i < topo->GetExtraInputCount(); ++i) {
            LOG(ERROR) << "    -> " << topo->GetEdgeById(topo->GetExtraInput(i))->GetName();
        }
        return RC_NOT_FOUND;
    }

    return RC_SUCCESS;
}

RetCode ModelParser::Parse(const char* model_file, ir::Graph* graph) {
    ppl::common::FileMapping fm;
    if (fm.Init(model_file) != RC_SUCCESS) {
        LOG(ERROR) << "Init filemapping from file [" << model_file << "] error.";
        return RC_INVALID_VALUE;
    }
    const char* model_buf = fm.Data();
    uint64_t buf_len = fm.Size();
    return ModelParser::Parse(model_buf, buf_len, graph);
}

}}} // namespace ppl::nn::onnx