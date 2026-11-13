#include "vision/models/yolo/detection_order.h"
#include "vision/models/yolo/rknn_query_utils.h"
#include <cassert>
#include <cmath>
#include <limits>

static int query_result = 0;
extern "C" int rknn_query(rknn_context, rknn_query_cmd, void *, uint32_t)
{
    return query_result;
}

template <typename F> void rejects(F operation)
{
    bool rejected = false;
    try
    {
        operation();
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    assert(rejected);
}

int main()
{
    const std::vector<float> scores{.4f, .9f, .6f};
    std::vector<int> order{0, 1, 2};
    sort_detection_indices(scores, order);
    assert(order == std::vector<int>({1, 2, 0}));
    assert(scores[order[0]] == .9f && scores[0] == .4f);
    for (auto size : {640, 768, 960, 1088})
    {
        rknn_tensor_attr proto{};
        proto.n_dims = 4;
        proto.fmt = RKNN_TENSOR_NCHW;
        proto.dims[0] = 1;
        proto.dims[1] = 32;
        proto.dims[2] = size / 4;
        proto.dims[3] = size / 8;
        model_io::validate_proto(proto, size / 2, size);
        proto.dims[1] = 16;
        rejects([&] { model_io::validate_proto(proto, size / 2, size); });
        rknn_tensor_attr head{};
        head.n_dims = 4;
        head.fmt = RKNN_TENSOR_NCHW;
        head.dims[0] = 1;
        head.dims[1] = 64;
        head.dims[2] = size / 8;
        head.dims[3] = size / 16;
        model_io::validate_head(head, size / 2, size, 64, 8);
        head.dims[3]++;
        rejects([&] { model_io::validate_head(head, size / 2, size, 64, 8); });
    }
    volatile float nan = std::numeric_limits<float>::quiet_NaN();
    volatile float infinity = std::numeric_limits<float>::infinity();
    assert(!std::isfinite(nan));
    assert(!std::isfinite(infinity));
    assert(std::isfinite(0.5f));
    rknn_input_output_num io{};
    rejects([&] { model_io::validate_io(io); });
    io.n_input = 1;
    io.n_output = 3;
    model_io::validate_io(io);
    query_result = -1;
    rejects([&] { model_io::query(0, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)); });
    query_result = 0;
    model_io::query(0, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    rknn_tensor_attr input{};
    rejects([&] { model_io::validate_image(input); });
    input.n_dims = 4;
    input.fmt = RKNN_TENSOR_NHWC;
    input.dims[0] = 1;
    input.dims[1] = 640;
    input.dims[2] = 640;
    input.dims[3] = 3;
    model_io::validate_image(input);
    input.dims[3] = 1;
    rejects([&] { model_io::validate_image(input); });
}
