// Full tensor parity against RKNN's normal readback, including values that a
// detector might discard below its threshold. Run on an otherwise idle device.
#include "vision/inference/npu_device_owner.h"
#include "vision/models/rknn_output_buffers.h"
#include "vision/models/yolo/rknn_query_utils.h"
#include <cassert>
#include <iostream>
#include <opencv2/opencv.hpp>

struct Context
{
    rknn_context ctx = 0;
    rknn_tensor_mem *input = nullptr;
    model_io::OutputBuffers output;
    std::vector<rknn_tensor_attr> attrs;
    int width = 0, height = 0;
    bool floating = false;
    Context(const char *path, bool native, bool allow_native)
    {
        assert(rknn_init(&ctx, const_cast<char *>(path), 0, 0, nullptr) == 0);
        assert(rknn_set_core_mask(ctx, RKNN_NPU_CORE_0) == 0);
        rknn_input_output_num io{};
        model_io::query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
        assert(io.n_input == 1);
        rknn_tensor_attr in{};
        model_io::query(ctx, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in));
        model_io::validate_image(in);
        width = in.dims[in.fmt == RKNN_TENSOR_NHWC ? 2 : 3];
        height = in.dims[in.fmt == RKNN_TENSOR_NHWC ? 1 : 2];
        input = rknn_create_mem(ctx, std::max<uint32_t>(width * height * 3, in.size_with_stride));
        assert(input && input->virt_addr);
        in.type = RKNN_TENSOR_UINT8;
        in.fmt = RKNN_TENSOR_NHWC;
        in.pass_through = 0;
        in.w_stride = width;
        in.h_stride = height;
        assert(rknn_set_io_mem(ctx, input, &in) == 0);
        attrs.resize(io.n_output);
        for (unsigned i = 0; i < io.n_output; ++i)
        {
            attrs[i].index = i;
            model_io::query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attrs[i], sizeof(attrs[i]));
        }
        floating = !model_io::all_int8(attrs);
        if (native)
            output.initialize(ctx, attrs, floating, allow_native);
    }
    ~Context()
    {
        output.reset();
        if (input)
            rknn_destroy_mem(ctx, input);
        if (ctx)
            rknn_destroy(ctx);
    }
};

int main(int argc, char **argv)
{
    assert(argc == 4); // model, image, adapter type
    npu::DeviceOwner owner;
    cv::setNumThreads(1);
    const bool allow_native = std::string(argv[3]) != "yolo26_depth";
    Context reference(argv[1], false, false), candidate(argv[1], true, allow_native);
    if (allow_native)
        assert(candidate.output.native());
    cv::Mat camera = cv::imread(argv[2]), resized, rgb;
    assert(!camera.empty());
    cv::resize(camera, resized, {reference.width, reference.height});
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    cv::Mat noise(rgb.size(), CV_8UC3);
    cv::RNG random(3588);
    random.fill(noise, cv::RNG::UNIFORM, 0, 256);
    std::vector<cv::Mat> inputs{rgb, noise, cv::Mat(rgb.size(), CV_8UC3, cv::Scalar(0, 0, 0)),
                                cv::Mat(rgb.size(), CV_8UC3, cv::Scalar(255, 255, 255))};
    std::vector<std::vector<float>> buffers(reference.attrs.size());
    std::vector<rknn_output> outputs(reference.attrs.size());
    size_t compared = 0;
    bool equal = true;
    for (const auto &frame : inputs)
    {
        std::memcpy(reference.input->virt_addr, frame.data, frame.total() * frame.elemSize());
        std::memcpy(candidate.input->virt_addr, frame.data, frame.total() * frame.elemSize());
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            const size_t bytes = reference.attrs[i].n_elems * (reference.floating ? 4 : 1);
            buffers[i].resize((bytes + 3) / 4);
            outputs[i] = {};
            outputs[i].index = i;
            outputs[i].is_prealloc = 1;
            outputs[i].want_float = reference.floating;
            outputs[i].buf = buffers[i].data();
            outputs[i].size = bytes;
        }
        {
            npu::Execution execution(1);
            assert(execution.run(reference.ctx) == 0);
            npu::Outputs lease(execution, reference.ctx, outputs.size(), outputs.data());
            assert(lease.get() == 0 && lease.release() == 0);
        }
        std::chrono::steady_clock::time_point ran;
        assert(candidate.output.execute(1, nullptr, 0, ran) == 0);
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            const auto &actual = candidate.output.data()[i];
            assert(actual.size == outputs[i].size);
            if (std::memcmp(actual.buf, outputs[i].buf, actual.size) != 0)
            {
                std::cerr << "Tensor mismatch output=" << i << " bytes=" << actual.size << std::endl;
                if (reference.floating)
                {
                    const auto *x = static_cast<const float *>(actual.buf);
                    const auto *y = static_cast<const float *>(outputs[i].buf);
                    size_t different = 0;
                    float max_error = 0.f;
                    for (size_t k = 0; k < actual.size / 4; ++k)
                        if (x[k] != y[k])
                        {
                            if (different < 5)
                                std::cerr << "index=" << k << " candidate=" << std::hexfloat << x[k]
                                          << " expected=" << y[k] << std::defaultfloat << std::endl;
                            ++different;
                            max_error = std::max(max_error, std::abs(x[k] - y[k]));
                        }
                    std::cerr << "different=" << different << " max_abs=" << max_error << std::endl;
                }
                equal = false;
            }
            compared += actual.size;
        }
    }
    std::cout << argv[3] << " full tensor parity=" << equal << " bytes=" << compared << std::endl;
    return equal ? 0 : 1;
}
