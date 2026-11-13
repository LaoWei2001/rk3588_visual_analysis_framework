#include "zip_depth.h"
#include "vision/inference/npu_execution.h"

#include <algorithm>
#include <arm_neon.h>
#include <chrono>
#include <climits>
#include <rga/im2d.h>
#include <stdexcept>

namespace
{
using Clock = std::chrono::steady_clock;
float ms(Clock::time_point start)
{
    return std::chrono::duration<float, std::milli>(Clock::now() - start).count();
}
void check(int code, const char *operation)
{
    if (code < 0)
        throw std::runtime_error(std::string("ZipDepth ") + operation + ": " + std::to_string(code));
}
} // namespace

ZipDepth::ZipDepth(const std::string &path, int core_mask)
{
    try
    {
        check(rknn_init(&context_, const_cast<char *>(path.c_str()), 0, 0, nullptr), "rknn_init");
        check(rknn_set_core_mask(context_, static_cast<rknn_core_mask>(core_mask)), "set_core_mask");
        npu_core_mask_ = core_mask;
        query_model();
        output_pool_.reserve(4);
        init_input_memory();
        init_native_input();
        printf("[ZipDepth] input=%dx%d output=%dx%d RGB8, core=0x%x, DMA=%s, native FP16=%s\n", width_, height_,
               output_width_, output_height_, core_mask, input_mem_ ? "enabled" : "CPU fallback",
               native_input_mem_ ? "validated" : "disabled");
    }
    catch (...)
    {
        release();
        throw;
    }
}

ZipDepth::~ZipDepth()
{
    release();
}

void ZipDepth::release()
{
    if (input_rga_handle_)
        releasebuffer_handle(static_cast<rga_buffer_handle_t>(input_rga_handle_));
    if (input_mem_ && context_)
        rknn_destroy_mem(context_, input_mem_);
    if (native_input_mem_ && context_)
        rknn_destroy_mem(context_, native_input_mem_);
    if (context_)
        rknn_destroy(context_);
    input_rga_handle_ = 0;
    input_mem_ = nullptr;
    native_input_mem_ = nullptr;
    context_ = 0;
}

void ZipDepth::query_model()
{
    rknn_input_output_num io{};
    check(rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "query IO");
    if (io.n_input != 1 || io.n_output != 1)
        throw std::runtime_error("ZipDepth requires one RGB input and one relative inverse-depth output");
    rknn_tensor_attr out{};
    check(rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_, sizeof(input_attr_)), "query input");
    check(rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &out, sizeof(out)), "query output");
    if (input_attr_.n_dims != 4 || input_attr_.dims[0] != 1)
        throw std::runtime_error("ZipDepth requires static batch-1 image input");
    uint32_t channels = 0;
    if (input_attr_.fmt == RKNN_TENSOR_NHWC)
    {
        height_ = input_attr_.dims[1];
        width_ = input_attr_.dims[2];
        channels = input_attr_.dims[3];
    }
    else if (input_attr_.fmt == RKNN_TENSOR_NCHW)
    {
        channels = input_attr_.dims[1];
        height_ = input_attr_.dims[2];
        width_ = input_attr_.dims[3];
    }
    const uint64_t input_elements = uint64_t(width_) * height_ * 3;
    if (width_ <= 0 || height_ <= 0 || channels != 3 || input_elements != input_attr_.n_elems ||
        input_elements > UINT32_MAX)
        throw std::runtime_error("ZipDepth invalid RGB image shape");
    if (out.n_dims == 4 && out.dims[0] == 1 && out.fmt == RKNN_TENSOR_NCHW && out.dims[1] == 1)
    {
        output_height_ = out.dims[2];
        output_width_ = out.dims[3];
    }
    else if (out.n_dims == 4 && out.dims[0] == 1 && out.fmt == RKNN_TENSOR_NHWC && out.dims[3] == 1)
    {
        output_height_ = out.dims[1];
        output_width_ = out.dims[2];
    }
    else if (out.n_dims == 3 && out.dims[0] == 1)
    {
        output_height_ = out.dims[1];
        output_width_ = out.dims[2];
    }
    else if (out.n_dims == 2)
    {
        output_height_ = out.dims[0];
        output_width_ = out.dims[1];
    }
    const uint64_t output_elements = uint64_t(output_width_) * output_height_;
    if (output_width_ <= 0 || output_height_ <= 0 || output_elements != out.n_elems ||
        output_elements > UINT32_MAX / sizeof(float))
        throw std::runtime_error("ZipDepth expects a single-channel depth plane");
    if (output_width_ > width_ || output_height_ > height_ ||
        uint64_t(output_width_) * height_ != uint64_t(output_height_) * width_)
        throw std::runtime_error("ZipDepth output must match the input canvas aspect ratio (possibly downsampled)");
    if (out.type != RKNN_TENSOR_FLOAT32 && out.type != RKNN_TENSOR_FLOAT16 && out.type != RKNN_TENSOR_INT8 &&
        out.type != RKNN_TENSOR_UINT8)
        throw std::runtime_error("ZipDepth unsupported output dtype");
}

void ZipDepth::init_input_memory()
{
    // The pipeline writes packed RGB with model_width stride. Exotic shapes
    // safely use inputs_set instead of assuming a padded DMA layout.
    if (width_ % 16 != 0 || (input_attr_.w_stride && input_attr_.w_stride != uint32_t(width_)))
        return;
    input_mem_ = rknn_create_mem(context_, std::max<uint32_t>(width_ * height_ * 3, input_attr_.size_with_stride));
    if (!input_mem_)
        return;
    if (!input_mem_->virt_addr)
    {
        rknn_destroy_mem(context_, input_mem_);
        input_mem_ = nullptr;
        return;
    }
    try
    {
        bind_rgb_input();
    }
    catch (...)
    {
        rknn_destroy_mem(context_, input_mem_);
        input_mem_ = nullptr;
        return;
    }
    im_handle_param_t destination{};
    destination.width = width_;
    destination.height = height_;
    destination.format = RK_FORMAT_RGB_888;
    input_rga_handle_ = static_cast<int>(importbuffer_fd(input_mem_->fd, &destination));
}

void ZipDepth::bind_rgb_input()
{
    rknn_tensor_attr attr = input_attr_;
    attr.type = RKNN_TENSOR_UINT8;
    attr.fmt = RKNN_TENSOR_NHWC;
    attr.pass_through = 0;
    attr.w_stride = width_;
    attr.h_stride = height_;
    check(rknn_set_io_mem(context_, input_mem_, &attr), "bind RGB input");
}

void ZipDepth::prepare_native_input()
{
    // Standard ZipDepth exports normalize RGB8 by 255 in RKNN's InputOperator;
    // ImageNet normalization remains in the network. Convert eight samples at
    // once using hardware FP16 conversion, rather than scalar SDK conversion.
    const auto *source = static_cast<const uint8_t *>(input_mem_->virt_addr);
    auto *destination = static_cast<float16_t *>(native_input_mem_->virt_addr);
    const size_t count = size_t(width_) * height_ * 3;
    const float32x4_t scale = vdupq_n_f32(1.0f / 255.0f);
    size_t i = 0;
    for (; i + 8 <= count; i += 8)
    {
        const uint16x8_t values = vmovl_u8(vld1_u8(source + i));
        const float32x4_t lo = vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(values))), scale);
        const float32x4_t hi = vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(values))), scale);
        vst1q_f16(destination + i, vcombine_f16(vcvt_f16_f32(lo), vcvt_f16_f32(hi)));
    }
    for (; i < count; ++i)
        destination[i] = static_cast<float16_t>(source[i] / 255.0f);
}

void ZipDepth::init_native_input()
{
    if (!input_mem_)
        return;
    rknn_tensor_attr attr{};
    if (rknn_query(context_, RKNN_QUERY_NATIVE_INPUT_ATTR, &attr, sizeof(attr)) < 0 ||
        attr.type != RKNN_TENSOR_FLOAT16 || attr.fmt != RKNN_TENSOR_NHWC || attr.n_dims != 4 || attr.dims[0] != 1 ||
        attr.dims[1] != uint32_t(height_) || attr.dims[2] != uint32_t(width_) || attr.dims[3] != 3 ||
        (attr.w_stride && attr.w_stride != uint32_t(width_)) ||
        attr.size_with_stride != uint64_t(width_) * height_ * 3 * sizeof(float16_t))
        return;
    native_input_mem_ = rknn_create_mem(context_, attr.size_with_stride);
    if (!native_input_mem_ || !native_input_mem_->virt_addr)
    {
        if (native_input_mem_)
            rknn_destroy_mem(context_, native_input_mem_);
        native_input_mem_ = nullptr;
        return;
    }

    // Do not guess a model's normalization from FP16 shape alone. Verify this
    // fast path against the runtime's own conversion before publishing frames.
    // The spatial/channel ramp covers every RGB8 value, including 0 and 255.
    auto *pixels = static_cast<uint8_t *>(input_mem_->virt_addr);
    for (size_t i = 0, count = size_t(width_) * height_ * 3; i < count; ++i)
        pixels[i] = static_cast<uint8_t>((i * 73 + (i / (width_ * 3)) * 19) & 255);
    prepare_native_input();
    cv::Mat expected(output_height_, output_width_, CV_32FC1), actual(expected.size(), expected.type());
    npu::Execution execution(npu_core_mask_);
    auto read = [&](cv::Mat &map) {
        check(execution.run(context_), "validate input run");
        rknn_output output{};
        output.want_float = 1;
        output.is_prealloc = 1;
        output.buf = map.data;
        output.size = static_cast<uint32_t>(map.total() * map.elemSize());
        npu::Outputs lease(execution, context_, 1, &output);
        check(lease.get(), "validate input output");
        check(lease.release(), "validate input release");
    };
    read(expected);
    attr.pass_through = 1;
    const int bound = rknn_set_io_mem(context_, native_input_mem_, &attr);
    if (bound >= 0)
    {
        read(actual);
        if (cv::checkRange(expected) && cv::checkRange(actual) && cv::norm(expected, actual, cv::NORM_INF) == 0)
            return;
    }
    bind_rgb_input();
    rknn_destroy_mem(context_, native_input_mem_);
    native_input_mem_ = nullptr;
    printf("[ZipDepth] native FP16 validation did not match; using runtime RGB conversion\n");
}

cv::Mat ZipDepth::acquire_output()
{
    for (auto &slot : output_pool_)
        if (slot.u && CV_XADD(&slot.u->refcount, 0) == 1)
            return slot;
    cv::Mat output(output_height_, output_width_, CV_32FC1);
    if (output_pool_.size() < 4)
        output_pool_.push_back(output);
    return output;
}

bool ZipDepth::run(std::vector<AlgoResult> &results, ModelPerfStat *perf, float preprocess_ms)
{
    results.clear();
    if (native_input_mem_)
    {
        const auto prepare_begin = Clock::now();
        prepare_native_input();
        preprocess_ms += ms(prepare_begin);
    }
    const auto begin = Clock::now();
    npu::Execution execution(npu_core_mask_);
    if (!input_mem_)
    {
        rknn_input input{};
        input.buf = rgb_.data;
        input.size = static_cast<uint32_t>(rgb_.total() * rgb_.elemSize());
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        check(execution.inputs_set(context_, 1, &input), "set RGB input");
    }
    check(execution.run(context_), "run");
    cv::Mat map = acquire_output();
    rknn_output output{};
    output.want_float = 1;
    output.is_prealloc = 1;
    output.buf = map.data;
    output.size = static_cast<uint32_t>(map.total() * map.elemSize());
    npu::Outputs output_lease(execution, context_, 1, &output);
    check(output_lease.get(), "get depth");
    check(output_lease.release(), "release output");
    const float infer_ms = ms(begin);
    const auto post_begin = Clock::now();
    AlgoResult result;
    result.label = "depth";
    result.score = 1.0f; // frame payload, not object confidence
    result.box = cv::Rect(0, 0, width_, height_);
    result.depth_kind = DepthKind::RelativeInverse;
    result.depth_map = std::move(map);
    results.push_back(std::move(result));
    if (perf)
    {
        perf->preprocess_ms = preprocess_ms;
        perf->infer_ms = infer_ms;
        perf->postprocess_ms = ms(post_begin);
    }
    return true;
}

bool ZipDepth::infer(cv::Mat &frame, std::vector<AlgoResult> &results, ModelPerfStat *perf)
{
    results.clear();
    if (perf)
        *perf = {};
    if (frame.empty() || frame.dims != 2 || frame.type() != CV_8UC3)
        return false;
    const auto begin = Clock::now();
    cv::Mat source = frame;
    if (frame.cols != width_ || frame.rows != height_)
    {
        cv::resize(frame, resized_, cv::Size(width_, height_), 0, 0, cv::INTER_LINEAR);
        source = resized_;
    }
    if (input_mem_)
        rgb_ = cv::Mat(height_, width_, CV_8UC3, input_mem_->virt_addr);
    cv::cvtColor(source, rgb_, cv::COLOR_BGR2RGB);

    return run(results, perf, ms(begin));
}

bool ZipDepth::infer_zero_copy(std::vector<AlgoResult> &results, ModelPerfStat *perf)
{
    if (!input_mem_)
    {
        results.clear();
        return false;
    }
    return run(results, perf, 0.0f);
}
