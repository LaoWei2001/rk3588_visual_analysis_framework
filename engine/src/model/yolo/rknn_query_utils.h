#pragma once

#include <rknn_api.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

namespace model_io
{
inline bool all_int8(const std::vector<rknn_tensor_attr> &attrs) {
    return std::all_of(attrs.begin(), attrs.end(), [](const rknn_tensor_attr &a) {
        return a.type == RKNN_TENSOR_INT8 && a.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
    });
}
inline void validate_proto(const rknn_tensor_attr &a, int width, int height) {
    if (a.n_dims != 4 || a.fmt != RKNN_TENSOR_NCHW || a.dims[0] != 1 || a.dims[1] != 32 ||
        !a.dims[2] || !a.dims[3] || a.dims[2] > unsigned(height) || a.dims[3] > unsigned(width))
        throw std::runtime_error("Unsupported segmentation prototype shape");
}
inline void validate_head(const rknn_tensor_attr &a, int width, int height, unsigned channels, int stride) {
    if (width % stride || height % stride || a.n_dims != 4 || a.fmt != RKNN_TENSOR_NCHW ||
        a.dims[0] != 1 || !channels || a.dims[1] != channels ||
        a.dims[2] != unsigned(height / stride) || a.dims[3] != unsigned(width / stride))
        throw std::runtime_error("Detection head shape does not match model dimensions");
}

inline void query(rknn_context ctx, rknn_query_cmd command, void *value, unsigned size)
{
    const int result = rknn_query(ctx, command, value, size);
    if (result < 0)
        throw std::runtime_error("RKNN metadata query failed: " + std::to_string(result));
}

inline void validate_io(const rknn_input_output_num &io)
{
    if (io.n_input != 1 || io.n_output == 0 || io.n_output > 64)
        throw std::runtime_error("RKNN model requires one image input and 1..64 outputs");
}

inline void validate_image(const rknn_tensor_attr &attr)
{
    if (attr.n_dims != 4 || attr.dims[0] != 1 ||
        (attr.fmt != RKNN_TENSOR_NHWC && attr.fmt != RKNN_TENSOR_NCHW))
        throw std::runtime_error("RKNN model requires static batch-1 RGB image input");
    const bool nhwc = attr.fmt == RKNN_TENSOR_NHWC;
    if (attr.dims[nhwc ? 3 : 1] != 3 || !attr.dims[nhwc ? 1 : 2] || !attr.dims[nhwc ? 2 : 3])
        throw std::runtime_error("Invalid RKNN RGB input dimensions");
}
}
