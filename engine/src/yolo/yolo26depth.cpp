#include "yolo26depth.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <rga/im2d.h>
#include <stdexcept>

namespace
{
using Clock = std::chrono::steady_clock;
float ms(Clock::time_point start) { return std::chrono::duration<float, std::milli>(Clock::now() - start).count(); }
void check(int code, const char *operation)
{
    if (code < 0) throw std::runtime_error(std::string("Yolo26Depth ") + operation + ": " + std::to_string(code));
}
}

Yolo26Depth::Yolo26Depth(const std::string &path, int core_mask)
{
    try
    {
        check(rknn_init(&context_, const_cast<char *>(path.c_str()), 0, 0, nullptr), "rknn_init");
        check(rknn_set_core_mask(context_, static_cast<rknn_core_mask>(core_mask)), "set_core_mask");
        query_model();
        output_pool_.reserve(4);
        init_input_memory();
        printf("[Yolo26Depth] input=%dx%d output=%dx%d RGB8, core=0x%x, DMA=%s\n",
               width_, height_, output_width_, output_height_, core_mask, input_mem_ ? "enabled" : "CPU fallback");
    }
    catch (...) { release(); throw; }
}

Yolo26Depth::~Yolo26Depth() { release(); }

void Yolo26Depth::release()
{
    if (input_rga_handle_) releasebuffer_handle(static_cast<rga_buffer_handle_t>(input_rga_handle_));
    if (input_mem_ && context_) rknn_destroy_mem(context_, input_mem_);
    if (context_) rknn_destroy(context_);
    input_rga_handle_ = 0;
    input_mem_ = nullptr;
    context_ = 0;
}

void Yolo26Depth::query_model()
{
    rknn_input_output_num io{};
    check(rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "query IO");
    if (io.n_input != 1 || io.n_output != 1)
        throw std::runtime_error("Yolo26Depth requires one RGB input and one metric-depth output");
    rknn_tensor_attr out{};
    check(rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_, sizeof(input_attr_)), "query input");
    check(rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &out, sizeof(out)), "query output");
    if (input_attr_.n_dims != 4 || input_attr_.dims[0] != 1)
        throw std::runtime_error("Yolo26Depth requires static batch-1 image input");
    uint32_t channels = 0;
    if (input_attr_.fmt == RKNN_TENSOR_NHWC)
    {
        height_ = input_attr_.dims[1]; width_ = input_attr_.dims[2]; channels = input_attr_.dims[3];
    }
    else if (input_attr_.fmt == RKNN_TENSOR_NCHW)
    {
        channels = input_attr_.dims[1]; height_ = input_attr_.dims[2]; width_ = input_attr_.dims[3];
    }
    const uint64_t input_elements = uint64_t(width_) * height_ * 3;
    if (width_ <= 0 || height_ <= 0 || channels != 3 || input_elements != input_attr_.n_elems || input_elements > UINT32_MAX)
        throw std::runtime_error("Yolo26Depth invalid RGB image shape");
    if (out.n_dims == 4 && out.dims[0] == 1 && out.fmt == RKNN_TENSOR_NCHW && out.dims[1] == 1)
    { output_height_ = out.dims[2]; output_width_ = out.dims[3]; }
    else if (out.n_dims == 4 && out.dims[0] == 1 && out.fmt == RKNN_TENSOR_NHWC && out.dims[3] == 1)
    { output_height_ = out.dims[1]; output_width_ = out.dims[2]; }
    else if (out.n_dims == 3 && out.dims[0] == 1)
    { output_height_ = out.dims[1]; output_width_ = out.dims[2]; }
    else if (out.n_dims == 2)
    { output_height_ = out.dims[0]; output_width_ = out.dims[1]; }
    const uint64_t output_elements = uint64_t(output_width_) * output_height_;
    if (output_width_ <= 0 || output_height_ <= 0 || output_elements != out.n_elems || output_elements > UINT32_MAX / sizeof(float))
        throw std::runtime_error("Yolo26Depth expects a single-channel depth plane");
    if (output_width_ > width_ || output_height_ > height_ ||
        uint64_t(output_width_) * height_ != uint64_t(output_height_) * width_)
        throw std::runtime_error("Yolo26Depth output must match the input canvas aspect ratio (possibly downsampled)");
    if (out.type != RKNN_TENSOR_FLOAT32 && out.type != RKNN_TENSOR_FLOAT16 &&
        out.type != RKNN_TENSOR_INT8 && out.type != RKNN_TENSOR_UINT8)
        throw std::runtime_error("Yolo26Depth unsupported output dtype");
}

void Yolo26Depth::init_input_memory()
{
    // The pipeline writes packed RGB with model_width stride. Exotic shapes
    // safely use inputs_set instead of assuming a padded DMA layout.
    if (width_ % 16 != 0) return;
    input_mem_ = rknn_create_mem(context_, std::max<uint32_t>(width_ * height_ * 3, input_attr_.size_with_stride));
    if (!input_mem_) return;
    if (!input_mem_->virt_addr)
    {
        rknn_destroy_mem(context_, input_mem_); input_mem_ = nullptr; return;
    }
    rknn_tensor_attr attr = input_attr_;
    attr.type = RKNN_TENSOR_UINT8;
    attr.fmt = RKNN_TENSOR_NHWC;
    attr.pass_through = 0;
    attr.w_stride = width_;
    attr.h_stride = height_;
    if (rknn_set_io_mem(context_, input_mem_, &attr) < 0)
    {
        rknn_destroy_mem(context_, input_mem_); input_mem_ = nullptr; return;
    }
    im_handle_param_t destination{};
    destination.width = width_;
    destination.height = height_;
    destination.format = RK_FORMAT_RGB_888;
    input_rga_handle_ = static_cast<int>(importbuffer_fd(input_mem_->fd, &destination));
}

cv::Mat Yolo26Depth::acquire_output()
{
    for (auto &slot : output_pool_)
        if (slot.u && CV_XADD(&slot.u->refcount, 0) == 1) return slot;
    cv::Mat output(output_height_, output_width_, CV_32FC1);
    if (output_pool_.size() < 4) output_pool_.push_back(output);
    return output;
}

bool Yolo26Depth::run(std::vector<AlgoResult> &results, YoloPerfStat *perf, float preprocess_ms)
{
    results.clear();
    const auto begin = Clock::now();
    check(rknn_run(context_, nullptr), "run");
    cv::Mat map = acquire_output();
    rknn_output output{};
    output.want_float = 1;
    output.is_prealloc = 1;
    output.buf = map.data;
    output.size = static_cast<uint32_t>(map.total() * map.elemSize());
    check(rknn_outputs_get(context_, 1, &output, nullptr), "get depth");
    check(rknn_outputs_release(context_, 1, &output), "release output");
    const float infer_ms = ms(begin);
    const auto post_begin = Clock::now();
    if (map.cols != width_ || map.rows != height_)
    {
        cv::Mat resized;
        cv::resize(map, resized, cv::Size(width_, height_), 0, 0, cv::INTER_LINEAR);
        map = std::move(resized);
    }
    AlgoResult result;
    result.label = "depth";
    result.score = 1.0f; // frame payload, not object confidence
    result.box = cv::Rect(0, 0, width_, height_);
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

bool Yolo26Depth::infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    results.clear();
    if (frame.empty() || frame.dims != 2 || frame.type() != CV_8UC3) return false;
    const auto begin = Clock::now();
    cv::Mat source = frame;
    if (frame.cols != width_ || frame.rows != height_)
    {
        cv::resize(frame, resized_, cv::Size(width_, height_), 0, 0, cv::INTER_LINEAR);
        source = resized_;
    }
    if (input_mem_) rgb_ = cv::Mat(height_, width_, CV_8UC3, input_mem_->virt_addr);
    cv::cvtColor(source, rgb_, cv::COLOR_BGR2RGB);
    if (!input_mem_)
    {
        rknn_input input{};
        input.buf = rgb_.data;
        input.size = static_cast<uint32_t>(rgb_.total() * rgb_.elemSize());
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        check(rknn_inputs_set(context_, 1, &input), "set RGB input");
    }
    return run(results, perf, ms(begin));
}

bool Yolo26Depth::infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    if (!input_mem_) { results.clear(); return false; }
    return run(results, perf, 0.0f);
}
