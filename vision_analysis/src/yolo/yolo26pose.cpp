#include "yolo26pose.h"

#include "yolo_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <rga/im2d.h>
#include <stdexcept>

namespace
{
using Clock = std::chrono::steady_clock;

std::string tensor_shape(const rknn_tensor_attr &attr)
{
    std::string result = "[";
    for (uint32_t i = 0; i < attr.n_dims; ++i)
    {
        if (i)
            result += ',';
        result += std::to_string(attr.dims[i]);
    }
    return result + ']';
}

float elapsed_ms(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<float, std::milli>(end - begin).count();
}

float clamp_float(float value, float minimum, float maximum)
{
    return std::max(minimum, std::min(value, maximum));
}
} // namespace

Yolo26Pose::Yolo26Pose(const std::string &model_path, const std::string &label_path, int core_mask, float obj_thresh,
                       float nms_thresh)
    : obj_thresh_(obj_thresh), nms_thresh_(nms_thresh)
{
    initialize(model_path, core_mask);
    try
    {
        query_model();
        configure_output();
        load_label(label_path);
        initialize_zero_copy_input();
    }
    catch (...)
    {
        if (context_)
        {
            rknn_destroy(context_);
            context_ = 0;
        }
        throw;
    }
}

Yolo26Pose::~Yolo26Pose()
{
    if (input_rga_handle_ != 0)
    {
        releasebuffer_handle(static_cast<rga_buffer_handle_t>(input_rga_handle_));
        input_rga_handle_ = 0;
    }
    if (input_mem_ && context_)
    {
        rknn_destroy_mem(context_, input_mem_);
        input_mem_ = nullptr;
    }
    if (context_)
    {
        rknn_destroy(context_);
        context_ = 0;
    }
}

void Yolo26Pose::initialize(const std::string &model_path, int core_mask)
{
    const int ret = rknn_init(&context_, const_cast<char *>(model_path.c_str()), 0, 0, nullptr);
    if (ret < 0)
        throw std::runtime_error("Yolo26Pose rknn_init failed(" + std::to_string(ret) + "): " + model_path);

    const int core_ret = rknn_set_core_mask(context_, static_cast<rknn_core_mask>(core_mask));
    if (core_ret < 0)
        printf("[Yolo26Pose] warning: set core mask failed(%d), using runtime default\n", core_ret);
    else
        printf("[Yolo26Pose] use core mask 0x%x\n", core_mask);
}

void Yolo26Pose::query_model()
{
    rknn_sdk_version version{};
    if (rknn_query(context_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)) == 0)
        printf("[Yolo26Pose] RKNN API %s, driver %s\n", version.api_version, version.drv_version);

    rknn_input_output_num io{};
    if (rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) < 0)
        throw std::runtime_error("Yolo26Pose cannot query input/output count");
    if (io.n_input != 1 || io.n_output != 1)
        throw std::runtime_error("Yolo26Pose requires exactly one input and one output, got " +
                                 std::to_string(io.n_input) + "/" + std::to_string(io.n_output));

    input_attr_.index = 0;
    output_attr_.index = 0;
    if (rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_, sizeof(input_attr_)) < 0 ||
        rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &output_attr_, sizeof(output_attr_)) < 0)
        throw std::runtime_error("Yolo26Pose cannot query tensor attributes");

    if (input_attr_.n_dims != 4)
        throw std::runtime_error("Yolo26Pose requires a 4-D image input, got " + tensor_shape(input_attr_));
    if (input_attr_.fmt == RKNN_TENSOR_NHWC)
    {
        model_height_ = static_cast<int>(input_attr_.dims[1]);
        model_width_ = static_cast<int>(input_attr_.dims[2]);
        model_channels_ = static_cast<int>(input_attr_.dims[3]);
    }
    else if (input_attr_.fmt == RKNN_TENSOR_NCHW)
    {
        model_channels_ = static_cast<int>(input_attr_.dims[1]);
        model_height_ = static_cast<int>(input_attr_.dims[2]);
        model_width_ = static_cast<int>(input_attr_.dims[3]);
    }
    else
        throw std::runtime_error("Yolo26Pose unsupported input format");
    if (model_channels_ != 3 || model_width_ <= 0 || model_height_ <= 0)
        throw std::runtime_error("Yolo26Pose invalid image input " + tensor_shape(input_attr_));

    printf("[Yolo26Pose] input=%s %s/%s, output=%s %s/%s zp=%d scale=%g\n", tensor_shape(input_attr_).c_str(),
           get_format_string(input_attr_.fmt), get_type_string(input_attr_.type), tensor_shape(output_attr_).c_str(),
           get_format_string(output_attr_.fmt), get_type_string(output_attr_.type), output_attr_.zp,
           output_attr_.scale);
}

void Yolo26Pose::configure_output()
{
    std::vector<int> dimensions;
    for (uint32_t i = 0; i < output_attr_.n_dims; ++i)
        if (output_attr_.dims[i] != 1)
            dimensions.push_back(static_cast<int>(output_attr_.dims[i]));
    if (dimensions.size() != 2)
        throw std::runtime_error("Yolo26Pose unsupported output shape " + tensor_shape(output_attr_));

    if (dimensions[0] == kFeatureCount)
    {
        features_first_ = true;
        candidate_count_ = dimensions[1];
    }
    else if (dimensions[1] == kFeatureCount)
    {
        features_first_ = false;
        candidate_count_ = dimensions[0];
    }
    else
    {
        throw std::runtime_error("Yolo26Pose expects [1,56,N] or [1,N,56], got " + tensor_shape(output_attr_));
    }
    if (candidate_count_ <= 0 || output_attr_.n_elems != static_cast<uint32_t>(candidate_count_ * kFeatureCount))
        throw std::runtime_error("Yolo26Pose output element count does not match its shape");

    switch (output_attr_.type)
    {
    case RKNN_TENSOR_FLOAT16:
    case RKNN_TENSOR_FLOAT32:
    case RKNN_TENSOR_INT8:
    case RKNN_TENSOR_UINT8:
        break;
    default:
        throw std::runtime_error("Yolo26Pose unsupported output type " +
                                 std::string(get_type_string(output_attr_.type)));
    }
    if ((output_attr_.type == RKNN_TENSOR_INT8 || output_attr_.type == RKNN_TENSOR_UINT8) && output_attr_.scale > 1.0f)
    {
        throw std::runtime_error("Yolo26Pose rejects lossy INT8 fused output: scale=" +
                                 std::to_string(output_attr_.scale) + " cannot preserve 0..1 confidence values");
    }

    const size_t output_bytes = std::max<size_t>(output_attr_.size, output_attr_.size_with_stride);
    if (output_bytes == 0)
        throw std::runtime_error("Yolo26Pose output buffer size is zero");
    native_output_.resize(output_bytes);
    candidates_.reserve(std::min(candidate_count_, 1024));
    kept_.reserve(kMaxDetections);
    printf("[Yolo26Pose] layout=%s candidates=%d keypoints=%d native_output=%zu bytes\n",
           features_first_ ? "features-first" : "candidates-first", candidate_count_, kKeypointCount,
           native_output_.size());
}

void Yolo26Pose::load_label(const std::string &label_path)
{
    std::vector<std::string> labels;
    if (!label_path.empty())
        load_label_file(label_path, labels);
    if (!labels.empty())
    {
        label_ = labels.front();
        while (!label_.empty() && (label_.back() == '\r' || label_.back() == '\n'))
            label_.pop_back();
    }
    if (label_.empty())
        label_ = "person";
}

bool Yolo26Pose::initialize_zero_copy_input()
{
    zero_copy_enabled_ = false;
    const uint32_t rgb_bytes = static_cast<uint32_t>(model_width_ * model_height_ * model_channels_);
    const uint32_t allocation = std::max(rgb_bytes, input_attr_.size_with_stride);
    input_mem_ = rknn_create_mem(context_, allocation);
    if (!input_mem_ || !input_mem_->virt_addr)
    {
        input_mem_ = nullptr;
        printf("[Yolo26Pose] zero-copy disabled: rknn_create_mem failed\n");
        return false;
    }

    input_io_attr_ = input_attr_;
    input_io_attr_.index = 0;
    input_io_attr_.type = RKNN_TENSOR_UINT8;
    input_io_attr_.fmt = RKNN_TENSOR_NHWC;
    input_io_attr_.pass_through = 0;
    input_io_attr_.h_stride = 0;
    const int ret = rknn_set_io_mem(context_, input_mem_, &input_io_attr_);
    if (ret < 0)
    {
        printf("[Yolo26Pose] zero-copy disabled: rknn_set_io_mem failed(%d)\n", ret);
        rknn_destroy_mem(context_, input_mem_);
        input_mem_ = nullptr;
        return false;
    }

    im_handle_param_t destination{};
    destination.width = static_cast<uint32_t>(model_width_);
    destination.height = static_cast<uint32_t>(model_height_);
    destination.format = RK_FORMAT_RGB_888;
    input_rga_handle_ = static_cast<int>(importbuffer_fd(input_mem_->fd, &destination));
    if (input_rga_handle_ == 0)
        printf("[Yolo26Pose] warning: cannot cache RGA destination handle\n");
    else
        printf("[Yolo26Pose] zero-copy input enabled, fd=%d, RGA handle=%d\n", input_mem_->fd, input_rga_handle_);
    zero_copy_enabled_ = true;
    return true;
}

cv::Mat &Yolo26Pose::preprocess(const cv::Mat &image, Letterbox &letterbox)
{
    const float scale =
        std::min(static_cast<float>(model_width_) / image.cols, static_cast<float>(model_height_) / image.rows);
    const int resized_width = std::max(1, static_cast<int>(std::round(image.cols * scale)));
    const int resized_height = std::max(1, static_cast<int>(std::round(image.rows * scale)));
    letterbox.scale = scale;
    letterbox.pad_x = (model_width_ - resized_width) / 2;
    letterbox.pad_y = (model_height_ - resized_height) / 2;

    cv::resize(image, resized_, cv::Size(resized_width, resized_height), 0, 0, cv::INTER_LINEAR);
    rgb_input_.create(model_height_, model_width_, CV_8UC3);
    rgb_input_.setTo(cv::Scalar(114, 114, 114));
    resized_.copyTo(rgb_input_(cv::Rect(letterbox.pad_x, letterbox.pad_y, resized_width, resized_height)));
    cv::cvtColor(rgb_input_, rgb_input_, cv::COLOR_BGR2RGB);
    return rgb_input_;
}

float Yolo26Pose::half_to_float(uint16_t value)
{
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000U) << 16;
    uint32_t exponent = (value >> 10) & 0x1fU;
    uint32_t mantissa = value & 0x03ffU;
    uint32_t bits = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            int shift = 0;
            while ((mantissa & 0x0400U) == 0)
            {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x03ffU;
            bits = sign | static_cast<uint32_t>(127 - 14 - shift) << 23 | mantissa << 13;
        }
    }
    else if (exponent == 0x1fU)
        bits = sign | 0x7f800000U | mantissa << 13;
    else
        bits = sign | (exponent + (127 - 15)) << 23 | mantissa << 13;
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

float Yolo26Pose::output_value(const void *output, int candidate, int feature) const
{
    const size_t offset = features_first_ ? static_cast<size_t>(feature) * candidate_count_ + candidate
                                          : static_cast<size_t>(candidate) * kFeatureCount + feature;
    switch (output_attr_.type)
    {
    case RKNN_TENSOR_FLOAT16:
        return half_to_float(static_cast<const uint16_t *>(output)[offset]);
    case RKNN_TENSOR_FLOAT32:
        return static_cast<const float *>(output)[offset];
    case RKNN_TENSOR_INT8:
        return (static_cast<int>(static_cast<const int8_t *>(output)[offset]) - output_attr_.zp) * output_attr_.scale;
    case RKNN_TENSOR_UINT8:
        return (static_cast<int>(static_cast<const uint8_t *>(output)[offset]) - output_attr_.zp) * output_attr_.scale;
    default:
        return 0.0f;
    }
}

float Yolo26Pose::probability(float value)
{
    if (value >= 0.0f && value <= 1.0f)
        return value;
    if (value > 80.0f)
        return 1.0f;
    if (value < -80.0f)
        return 0.0f;
    return 1.0f / (1.0f + std::exp(-value));
}

float Yolo26Pose::box_iou(const cv::Rect2f &a, const cv::Rect2f &b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float union_area = a.area() + b.area() - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

void Yolo26Pose::decode(const Letterbox &letterbox, int image_width, int image_height, const void *output,
                        std::vector<AlgoResult> &results)
{
    candidates_.clear();
    for (int candidate = 0; candidate < candidate_count_; ++candidate)
    {
        const float score = probability(output_value(output, candidate, 4));
        if (!std::isfinite(score) || score < obj_thresh_)
            continue;

        const float cx = output_value(output, candidate, 0);
        const float cy = output_value(output, candidate, 1);
        const float width = output_value(output, candidate, 2);
        const float height = output_value(output, candidate, 3);
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(width) || !std::isfinite(height) ||
            width <= 0.0f || height <= 0.0f)
            continue;

        const float max_x = static_cast<float>(std::max(0, image_width - 1));
        const float max_y = static_cast<float>(std::max(0, image_height - 1));
        const float left = clamp_float((cx - width * 0.5f - letterbox.pad_x) / letterbox.scale, 0.0f, max_x);
        const float top = clamp_float((cy - height * 0.5f - letterbox.pad_y) / letterbox.scale, 0.0f, max_y);
        const float right = clamp_float((cx + width * 0.5f - letterbox.pad_x) / letterbox.scale, 0.0f, max_x);
        const float bottom = clamp_float((cy + height * 0.5f - letterbox.pad_y) / letterbox.scale, 0.0f, max_y);
        if (right <= left || bottom <= top)
            continue;
        candidates_.push_back({cv::Rect2f(left, top, right - left, bottom - top), score, candidate});
    }

    std::sort(candidates_.begin(), candidates_.end(),
              [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    kept_.clear();
    for (const Candidate &candidate : candidates_)
    {
        bool suppressed = false;
        for (const Candidate &accepted : kept_)
        {
            if (box_iou(candidate.box, accepted.box) > nms_thresh_)
            {
                suppressed = true;
                break;
            }
        }
        if (!suppressed)
        {
            kept_.push_back(candidate);
            if (static_cast<int>(kept_.size()) >= kMaxDetections)
                break;
        }
    }

    results.clear();
    results.reserve(std::max(results.capacity(), kept_.size()));
    for (const Candidate &candidate : kept_)
    {
        AlgoResult result;
        result.box = candidate.box;
        result.label = label_;
        result.class_id = 0;
        result.score = candidate.score;
        result.pose_schema = PoseKeypointSchema::Coco17;
        result.keypoints.resize(kKeypointCount);
        result.keypoint_scores.resize(kKeypointCount);
        for (int keypoint = 0; keypoint < kKeypointCount; ++keypoint)
        {
            const int feature = 5 + keypoint * 3;
            const float x = (output_value(output, candidate.tensor_index, feature) - letterbox.pad_x) / letterbox.scale;
            const float y =
                (output_value(output, candidate.tensor_index, feature + 1) - letterbox.pad_y) / letterbox.scale;
            result.keypoints[keypoint] = cv::Point2f(x, y);
            result.keypoint_scores[keypoint] = probability(output_value(output, candidate.tensor_index, feature + 2));
        }
        results.push_back(std::move(result));
    }
}

bool Yolo26Pose::run_and_decode(const Letterbox &letterbox, int image_width, int image_height,
                                std::vector<AlgoResult> &results, YoloPerfStat *perf, float preprocess_ms)
{
    const auto inference_begin = Clock::now();
    if (rknn_run(context_, nullptr) < 0)
        return false;

    rknn_output output{};
    output.index = 0;
    output.want_float = 0;
    output.is_prealloc = 1;
    output.buf = native_output_.data();
    output.size = static_cast<uint32_t>(native_output_.size());
    if (rknn_outputs_get(context_, 1, &output, nullptr) < 0)
        return false;
    const auto inference_end = Clock::now();

    try
    {
        decode(letterbox, image_width, image_height, output.buf, results);
    }
    catch (...)
    {
        rknn_outputs_release(context_, 1, &output);
        throw;
    }
    rknn_outputs_release(context_, 1, &output);
    const auto postprocess_end = Clock::now();
    if (perf)
    {
        perf->preprocess_ms = preprocess_ms;
        perf->infer_ms = elapsed_ms(inference_begin, inference_end);
        perf->postprocess_ms = elapsed_ms(inference_end, postprocess_end);
    }
    return true;
}

bool Yolo26Pose::infer(cv::Mat &frame, std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    if (frame.empty() || frame.type() != CV_8UC3)
        return false;
    const auto preprocess_begin = Clock::now();
    Letterbox letterbox;
    cv::Mat &input = preprocess(frame, letterbox);
    const auto preprocess_end = Clock::now();

    rknn_input request{};
    request.index = 0;
    request.buf = input.data;
    request.size = static_cast<uint32_t>(input.total() * input.elemSize());
    request.pass_through = 0;
    request.type = RKNN_TENSOR_UINT8;
    request.fmt = RKNN_TENSOR_NHWC;
    if (rknn_inputs_set(context_, 1, &request) < 0)
        return false;

    return run_and_decode(letterbox, frame.cols, frame.rows, results, perf,
                          elapsed_ms(preprocess_begin, preprocess_end));
}

bool Yolo26Pose::infer_zero_copy(std::vector<AlgoResult> &results, YoloPerfStat *perf)
{
    if (!zero_copy_enabled_ || !input_mem_ || input_mem_->fd < 0)
        return false;
    /* 上游 RGA 已 stretch 成模型尺寸 RGB888；结果坐标保持模型空间，由统一管线映射。 */
    const Letterbox transform{1.0f, 0, 0};
    const float preprocess_ms = perf ? perf->preprocess_ms : 0.0f;
    return run_and_decode(transform, model_width_, model_height_, results, perf, preprocess_ms);
}
