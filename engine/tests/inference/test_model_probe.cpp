#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <model_inspection.h>
#include <rknn_api.h>
#include <thread>

namespace
{
enum Failure
{
    None,
    Init,
    InitWithContext,
    Counts,
    Input,
    Output,
    Dimensions,
    MultiInput
};
Failure failure = None;
std::atomic<int> initialized{0}, destroyed{0}, live{0}, inference_calls{0};
} // namespace

extern "C" int rknn_init(rknn_context *context, void *, uint32_t, uint32_t, rknn_init_extend *)
{
    if (failure == Init)
        return -1;
    assert(++live == 1); // SDK serializes even concurrent callers.
    ++initialized;
    *context = 7;
    std::this_thread::yield();
    return failure == InitWithContext ? -1 : 0;
}
extern "C" int rknn_destroy(rknn_context context)
{
    assert(context == 7 && live == 1);
    --live;
    ++destroyed;
    return 0;
}
extern "C" int rknn_run(rknn_context, rknn_run_extend *)
{
    ++inference_calls;
    return -1;
}
extern "C" int rknn_query(rknn_context, rknn_query_cmd cmd, void *value, uint32_t)
{
    if (cmd == RKNN_QUERY_IN_OUT_NUM)
    {
        if (failure == Counts)
            return -1;
        auto counts = static_cast<rknn_input_output_num *>(value);
        counts->n_input = failure == MultiInput ? 2 : 1;
        counts->n_output = 1;
        return 0;
    }
    auto attr = static_cast<rknn_tensor_attr *>(value);
    if ((cmd == RKNN_QUERY_INPUT_ATTR && failure == Input) || (cmd == RKNN_QUERY_OUTPUT_ATTR && failure == Output))
        return -1;
    std::strcpy(attr->name, cmd == RKNN_QUERY_INPUT_ATTR ? "image" : "depth");
    attr->n_dims = failure == Dimensions ? RKNN_MAX_DIMS + 1 : 4;
    if (cmd == RKNN_QUERY_INPUT_ATTR)
    {
        attr->dims[0] = 1;
        attr->dims[1] = 640;
        attr->dims[2] = 640;
        attr->dims[3] = 3;
        attr->n_elems = 640 * 640 * 3;
        attr->fmt = RKNN_TENSOR_NHWC;
        attr->type = RKNN_TENSOR_UINT8;
    }
    else
    {
        attr->dims[0] = 1;
        attr->dims[1] = 1;
        attr->dims[2] = 160;
        attr->dims[3] = 160;
        attr->n_elems = 160 * 160;
        attr->fmt = RKNN_TENSOR_NCHW;
        attr->type = RKNN_TENSOR_INT8;
    }
    attr->qnt_type = RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
    attr->scale = .125f;
    attr->zp = 7;
    return 0;
}

int main()
{
    const auto checked = vision::check_model_file("test.rknn", "zip_depth");
    assert(checked.ok() && checked.error().empty() && live == 0 && initialized == destroyed);
    assert(checked.compatibility.input_width == 640);
    const auto &out = checked.inspection.metadata.outputs[0];
    assert(out.name == "depth" && out.shape == std::vector<std::int64_t>({1, 1, 160, 160}));
    assert(out.scale == .125f && out.zero_point == 7 && out.type == vision::TensorDataType::Int8);
    assert(out.quantization == vision::TensorQuantization::AffineAsymmetric);
    for (auto error : {Init, InitWithContext, Counts, Input, Output, Dimensions})
    {
        failure = error;
        const auto failed = vision::check_model_file("bad.rknn", "zip_depth");
        assert(!failed.ok() && !failed.error().empty());
        assert(live == 0 && initialized == destroyed);
    }
    failure = MultiInput;
    auto raw = vision::inspect_model("multi.rknn");
    assert(raw.ok && raw.metadata.inputs.size() == 2);
    assert(!vision::check_model_compatibility(raw.metadata, "zip_depth").ok);
    failure = None;
    const auto before = initialized.load();
    assert(!vision::inspect_model("").ok);
    assert(!vision::inspect_model(std::string("file\0.rknn", 10)).ok);
    assert(initialized == before);
    std::vector<std::thread> callers;
    for (int i = 0; i < 12; ++i)
        callers.emplace_back([] { assert(vision::inspect_model("parallel.rknn").ok); });
    for (auto &caller : callers)
        caller.join();
    assert(checked.inspection.metadata.outputs[0].name == "depth"); // owned copy survives later probes
    assert(live == 0 && initialized == destroyed && inference_calls == 0);
    std::cout << "Probe ownership, failures, concurrent calls and metadata lifetime passed\n";
}
