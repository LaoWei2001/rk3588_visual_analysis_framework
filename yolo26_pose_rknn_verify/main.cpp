#include <rknn_api.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string model;
    std::string source;
    std::string output;
    float conf = 0.25F;
    float iou = 0.45F;
    float kpt_conf = 0.50F;
    int classes = 1;
    int keypoints = 17;
    int max_det = 100;
    int max_frames = 0;
    std::string core = "auto";
    bool inspect = false;
    bool show = false;
    bool no_save = false;
};

struct LetterboxInfo {
    float scale = 1.0F;
    int left = 0;
    int top = 0;
};

struct Keypoint {
    float x = 0;
    float y = 0;
    float confidence = 0;
};

struct Detection {
    cv::Rect2f box;
    float confidence = 0;
    int class_id = 0;
    std::vector<Keypoint> keypoints;
};

[[noreturn]] static void fail(const std::string& message) {
    throw std::runtime_error(message);
}

static float parse_float(const std::string& value, const std::string& option) {
    try {
        size_t used = 0;
        float result = std::stof(value, &used);
        if (used != value.size() || !std::isfinite(result)) fail(option + " 参数无效: " + value);
        return result;
    } catch (const std::exception&) {
        fail(option + " 参数无效: " + value);
    }
}

static int parse_int(const std::string& value, const std::string& option) {
    try {
        size_t used = 0;
        int result = std::stoi(value, &used);
        if (used != value.size()) fail(option + " 参数无效: " + value);
        return result;
    } catch (const std::exception&) {
        fail(option + " 参数无效: " + value);
    }
}

static void print_help(const char* executable) {
    std::cout
        << "YOLO26 Pose RKNN 验证工具（RK3588）\n\n"
        << "用法:\n"
        << "  " << executable << " --model MODEL --inspect\n"
        << "  " << executable << " --model MODEL --source IMAGE|VIDEO|CAMERA [选项]\n\n"
        << "参数:\n"
        << "  --model PATH       .rknn 文件，或只包含一个 .rknn 的导出目录\n"
        << "  --source VALUE     图片、视频路径，或摄像头编号（例如 0）\n"
        << "  --output PATH      输出图片/视频路径（不填则保存到 output/）\n"
        << "  --conf FLOAT       框置信度阈值，默认 0.25\n"
        << "  --iou FLOAT        NMS IoU 阈值，默认 0.45\n"
        << "  --kpt-conf FLOAT   关键点绘制阈值，默认 0.50\n"
        << "  --classes N        类别数，COCO Pose 默认 1\n"
        << "  --keypoints N      每个目标的关键点数，默认 17\n"
        << "  --max-det N        每帧最多保留目标数，默认 100\n"
        << "  --max-frames N     视频/摄像头最多处理帧数，0 表示不限\n"
        << "  --core VALUE       auto、0、1、2 或 all，默认 auto\n"
        << "  --show             打开实时显示窗口（无桌面环境不要使用）\n"
        << "  --no-save          不保存结果\n"
        << "  --inspect          只加载模型并检查张量，不执行推理\n"
        << "  -h, --help         显示帮助\n";
}

static Options parse_args(int argc, char** argv) {
    Options options;
    auto next = [&](int& i, const std::string& name) -> std::string {
        if (++i >= argc) fail(name + " 缺少参数");
        return argv[i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else if (arg == "--model") options.model = next(i, arg);
        else if (arg == "--source") options.source = next(i, arg);
        else if (arg == "--output") options.output = next(i, arg);
        else if (arg == "--conf") options.conf = parse_float(next(i, arg), arg);
        else if (arg == "--iou") options.iou = parse_float(next(i, arg), arg);
        else if (arg == "--kpt-conf") options.kpt_conf = parse_float(next(i, arg), arg);
        else if (arg == "--classes") options.classes = parse_int(next(i, arg), arg);
        else if (arg == "--keypoints") options.keypoints = parse_int(next(i, arg), arg);
        else if (arg == "--max-det") options.max_det = parse_int(next(i, arg), arg);
        else if (arg == "--max-frames") options.max_frames = parse_int(next(i, arg), arg);
        else if (arg == "--core") options.core = next(i, arg);
        else if (arg == "--inspect") options.inspect = true;
        else if (arg == "--show") options.show = true;
        else if (arg == "--no-save") options.no_save = true;
        else fail("未知参数: " + arg + "（使用 --help 查看用法）");
    }
    if (options.model.empty()) fail("必须指定 --model");
    if (!options.inspect && options.source.empty()) fail("必须指定 --source，或使用 --inspect");
    if (options.conf < 0 || options.conf > 1 || options.iou < 0 || options.iou > 1 ||
        options.kpt_conf < 0 || options.kpt_conf > 1) {
        fail("--conf、--iou 和 --kpt-conf 必须在 0 到 1 之间");
    }
    if (options.classes <= 0 || options.keypoints <= 0 || options.max_det <= 0 || options.max_frames < 0)
        fail("类别数、关键点数、最大检测数必须为正数，最大帧数不能为负数");
    return options;
}

static fs::path resolve_model(const fs::path& value) {
    if (!fs::exists(value)) fail("模型路径不存在: " + value.string());
    if (fs::is_regular_file(value)) return fs::absolute(value);
    if (!fs::is_directory(value)) fail("模型路径既不是文件也不是目录: " + value.string());

    std::vector<fs::path> models;
    for (const auto& entry : fs::recursive_directory_iterator(value)) {
        if (entry.is_regular_file() && entry.path().extension() == ".rknn") models.push_back(entry.path());
    }
    if (models.empty()) fail("导出目录中没有找到 .rknn 文件: " + value.string());
    if (models.size() > 1) {
        std::ostringstream message;
        message << "导出目录中有多个 .rknn，请直接指定其中一个:";
        for (const auto& path : models) message << "\n  " << path;
        fail(message.str());
    }
    return fs::absolute(models.front());
}

static rknn_core_mask parse_core(const std::string& value) {
    if (value == "auto") return RKNN_NPU_CORE_AUTO;
    if (value == "0") return RKNN_NPU_CORE_0;
    if (value == "1") return RKNN_NPU_CORE_1;
    if (value == "2") return RKNN_NPU_CORE_2;
    if (value == "all") return RKNN_NPU_CORE_0_1_2;
    fail("--core 只支持 auto、0、1、2、all");
}

static std::string dims_string(const rknn_tensor_attr& attr) {
    std::ostringstream out;
    out << '[';
    for (uint32_t i = 0; i < attr.n_dims; ++i) out << (i ? "," : "") << attr.dims[i];
    return out.str() + ']';
}

class RknnModel {
public:
    RknnModel(const fs::path& model, rknn_core_mask core) {
        const std::string path = model.string();
        int ret = rknn_init(&context_, const_cast<char*>(path.c_str()), 0, 0, nullptr);
        if (ret < 0) fail("rknn_init 失败，错误码 " + std::to_string(ret) + ": " + path);

        ret = rknn_set_core_mask(context_, core);
        if (ret < 0) std::cerr << "警告: 设置 NPU core 失败，错误码 " << ret << "，继续使用运行时默认值\n";

        rknn_sdk_version version{};
        ret = rknn_query(context_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
        if (ret == 0) {
            std::cout << "RKNN API: " << version.api_version << "\n"
                      << "RKNPU 驱动: " << version.drv_version << "\n";
        }

        rknn_input_output_num io{};
        check(rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "查询输入输出数量");
        if (io.n_input != 1) fail("当前验证程序仅支持单输入模型，实际输入数: " + std::to_string(io.n_input));

        input_.index = 0;
        check(rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_, sizeof(input_)), "查询输入张量");
        outputs_.resize(io.n_output);
        for (uint32_t i = 0; i < io.n_output; ++i) {
            outputs_[i].index = i;
            check(rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &outputs_[i], sizeof(outputs_[i])),
                  "查询输出张量 " + std::to_string(i));
        }
        read_input_shape();
        print_tensors();
    }

    ~RknnModel() {
        if (context_) rknn_destroy(context_);
    }

    RknnModel(const RknnModel&) = delete;
    RknnModel& operator=(const RknnModel&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<rknn_tensor_attr>& output_attrs() const { return outputs_; }

    struct InferenceOutputs {
        std::vector<rknn_output> values;
        rknn_context context = 0;
        ~InferenceOutputs() {
            if (context && !values.empty()) rknn_outputs_release(context, values.size(), values.data());
        }
        InferenceOutputs(const InferenceOutputs&) = delete;
        InferenceOutputs& operator=(const InferenceOutputs&) = delete;
        InferenceOutputs() = default;
        InferenceOutputs(InferenceOutputs&& other) noexcept
            : values(std::move(other.values)), context(other.context) { other.context = 0; }
    };

    InferenceOutputs infer(cv::Mat& rgb) {
        rknn_input input{};
        input.index = 0;
        input.buf = rgb.data;
        input.size = static_cast<uint32_t>(rgb.total() * rgb.elemSize());
        input.pass_through = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        check(rknn_inputs_set(context_, 1, &input), "设置模型输入");
        check(rknn_run(context_, nullptr), "执行推理");

        InferenceOutputs result;
        result.values.resize(outputs_.size());
        for (size_t i = 0; i < result.values.size(); ++i) {
            result.values[i].index = static_cast<uint32_t>(i);
            result.values[i].want_float = 1;
            result.values[i].is_prealloc = 0;
        }
        check(rknn_outputs_get(context_, result.values.size(), result.values.data(), nullptr), "读取模型输出");
        result.context = context_;
        return result;
    }

private:
    static void check(int code, const std::string& operation) {
        if (code < 0) fail(operation + "失败，RKNN 错误码 " + std::to_string(code));
    }

    void read_input_shape() {
        if (input_.n_dims != 4) fail("仅支持 4 维图像输入，实际为 " + dims_string(input_));
        int channels = 0;
        if (input_.fmt == RKNN_TENSOR_NCHW) {
            channels = input_.dims[1];
            height_ = input_.dims[2];
            width_ = input_.dims[3];
        } else if (input_.fmt == RKNN_TENSOR_NHWC) {
            height_ = input_.dims[1];
            width_ = input_.dims[2];
            channels = input_.dims[3];
        } else {
            fail("不支持的输入格式: " + std::string(get_format_string(input_.fmt)));
        }
        if (channels != 3 || width_ <= 0 || height_ <= 0)
            fail("需要三通道图像模型，实际输入为 " + dims_string(input_));
    }

    void print_tensors() const {
        auto print = [](const char* kind, const rknn_tensor_attr& attr) {
            std::cout << kind << '[' << attr.index << "] name=" << attr.name
                      << " shape=" << dims_string(attr)
                      << " elements=" << attr.n_elems
                      << " format=" << get_format_string(attr.fmt)
                      << " type=" << get_type_string(attr.type)
                      << " quant=" << get_qnt_type_string(attr.qnt_type)
                      << " zp=" << attr.zp << " scale=" << attr.scale << '\n';
        };
        print("input", input_);
        for (const auto& output : outputs_) print("output", output);
        std::cout << "模型输入尺寸: " << width_ << 'x' << height_ << "\n";
    }

    rknn_context context_ = 0;
    rknn_tensor_attr input_{};
    std::vector<rknn_tensor_attr> outputs_;
    int width_ = 0;
    int height_ = 0;
};

struct OutputLayout {
    size_t output_index = 0;
    int features = 0;
    int candidates = 0;
    bool features_first = true;
    bool end_to_end = false;
};

static OutputLayout find_pose_output(const std::vector<rknn_tensor_attr>& attrs, int classes, int keypoints) {
    const int raw_features = 4 + classes + keypoints * 3;
    const int end_to_end_features = 6 + keypoints * 3;
    std::vector<OutputLayout> matches;
    for (size_t index = 0; index < attrs.size(); ++index) {
        const auto& attr = attrs[index];
        std::vector<int> non_singleton;
        for (uint32_t axis = 0; axis < attr.n_dims; ++axis) {
            if (attr.dims[axis] != 1) non_singleton.push_back(static_cast<int>(attr.dims[axis]));
        }
        if (non_singleton.size() != 2) continue;
        if (non_singleton[0] == raw_features)
            matches.push_back({index, raw_features, non_singleton[1], true, false});
        else if (non_singleton[1] == raw_features)
            matches.push_back({index, raw_features, non_singleton[0], false, false});
        if (non_singleton[0] == end_to_end_features)
            matches.push_back({index, end_to_end_features, non_singleton[1], true, true});
        else if (non_singleton[1] == end_to_end_features)
            matches.push_back({index, end_to_end_features, non_singleton[0], false, true});
    }
    if (matches.empty()) {
        std::ostringstream message;
        message << "找不到 YOLO Pose 输出：期望原始输出每个候选包含 " << raw_features
                << " 个值，或 end-to-end 输出包含 " << end_to_end_features << " 个值。"
                << "\n模型实际输出:";
        for (size_t i = 0; i < attrs.size(); ++i)
            message << "\n  output[" << i << "] " << dims_string(attrs[i]);
        message << "\n如果模型的类别数或关键点数不同，请设置 --classes/--keypoints。";
        fail(message.str());
    }
    if (matches.size() != 1) fail("发现多个可能的 Pose 输出，无法安全判断，请检查模型导出结果");
    const auto result = matches.front();
    std::cout << "Pose 输出: output[" << result.output_index << "]，"
              << result.candidates << " 个候选，" << result.features << " 个特征，布局="
              << (result.features_first ? "[features,candidates]" : "[candidates,features]")
              << "，模式=" << (result.end_to_end ? "end-to-end" : "raw+CPU NMS") << "\n";
    return result;
}

static cv::Mat letterbox(const cv::Mat& bgr, int width, int height, LetterboxInfo& info) {
    info.scale = std::min(static_cast<float>(width) / bgr.cols,
                          static_cast<float>(height) / bgr.rows);
    const int resized_w = std::max(1, static_cast<int>(std::round(bgr.cols * info.scale)));
    const int resized_h = std::max(1, static_cast<int>(std::round(bgr.rows * info.scale)));
    info.left = (width - resized_w) / 2;
    info.top = (height - resized_h) / 2;

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(resized_w, resized_h), 0, 0, cv::INTER_LINEAR);
    cv::Mat padded(height, width, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(info.left, info.top, resized_w, resized_h)));
    cv::cvtColor(padded, padded, cv::COLOR_BGR2RGB);
    return padded;
}

static float probability(float value) {
    if (value >= 0.0F && value <= 1.0F) return value;
    if (value > 80.0F) return 1.0F;
    if (value < -80.0F) return 0.0F;
    return 1.0F / (1.0F + std::exp(-value));
}

static float intersection_over_union(const cv::Rect2f& a, const cv::Rect2f& b) {
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a.x + a.width, b.x + b.width);
    const float bottom = std::min(a.y + a.height, b.y + b.height);
    const float intersection = std::max(0.0F, right - left) * std::max(0.0F, bottom - top);
    const float union_area = a.area() + b.area() - intersection;
    return union_area > 0 ? intersection / union_area : 0.0F;
}

static std::vector<Detection> decode_pose(const float* data, const OutputLayout& layout,
                                          const Options& options, const LetterboxInfo& transform,
                                          const cv::Size& original_size, float& maximum_class_score) {
    auto at = [&](int candidate, int feature) -> float {
        if (layout.features_first)
            return data[static_cast<size_t>(feature) * layout.candidates + candidate];
        return data[static_cast<size_t>(candidate) * layout.features + feature];
    };

    std::vector<Detection> proposals;
    proposals.reserve(std::min(layout.candidates, 1000));
    maximum_class_score = 0.0F;
    for (int candidate = 0; candidate < layout.candidates; ++candidate) {
        int class_id = 0;
        float score = 0.0F;
        if (layout.end_to_end) {
            score = probability(at(candidate, 4));
            class_id = cvRound(at(candidate, 5));
        } else {
            score = -std::numeric_limits<float>::infinity();
            for (int c = 0; c < options.classes; ++c) {
                const float current = probability(at(candidate, 4 + c));
                if (current > score) {
                    score = current;
                    class_id = c;
                }
            }
        }
        maximum_class_score = std::max(maximum_class_score, score);
        if (!std::isfinite(score) || score < options.conf) continue;

        const float box0 = at(candidate, 0);
        const float box1 = at(candidate, 1);
        const float box2 = at(candidate, 2);
        const float box3 = at(candidate, 3);
        if (!std::isfinite(box0) || !std::isfinite(box1) || !std::isfinite(box2) || !std::isfinite(box3)) continue;

        float left = 0;
        float top = 0;
        float right = 0;
        float bottom = 0;
        if (layout.end_to_end) {
            left = (box0 - transform.left) / transform.scale;
            top = (box1 - transform.top) / transform.scale;
            right = (box2 - transform.left) / transform.scale;
            bottom = (box3 - transform.top) / transform.scale;
        } else {
            if (box2 <= 0 || box3 <= 0) continue;
            left = (box0 - box2 * 0.5F - transform.left) / transform.scale;
            top = (box1 - box3 * 0.5F - transform.top) / transform.scale;
            right = (box0 + box2 * 0.5F - transform.left) / transform.scale;
            bottom = (box1 + box3 * 0.5F - transform.top) / transform.scale;
        }
        left = std::clamp(left, 0.0F, static_cast<float>(original_size.width - 1));
        top = std::clamp(top, 0.0F, static_cast<float>(original_size.height - 1));
        right = std::clamp(right, 0.0F, static_cast<float>(original_size.width - 1));
        bottom = std::clamp(bottom, 0.0F, static_cast<float>(original_size.height - 1));
        if (right <= left || bottom <= top) continue;

        Detection detection;
        detection.box = cv::Rect2f(left, top, right - left, bottom - top);
        detection.confidence = score;
        detection.class_id = class_id;
        detection.keypoints.reserve(options.keypoints);
        const int keypoint_start = layout.end_to_end ? 6 : 4 + options.classes;
        for (int k = 0; k < options.keypoints; ++k) {
            const float x = (at(candidate, keypoint_start + k * 3) - transform.left) / transform.scale;
            const float y = (at(candidate, keypoint_start + k * 3 + 1) - transform.top) / transform.scale;
            const float confidence = probability(at(candidate, keypoint_start + k * 3 + 2));
            detection.keypoints.push_back({x, y, confidence});
        }
        proposals.push_back(std::move(detection));
    }

    std::sort(proposals.begin(), proposals.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });

    std::vector<Detection> kept;
    kept.reserve(std::min(static_cast<int>(proposals.size()), options.max_det));
    for (auto& proposal : proposals) {
        bool suppressed = false;
        if (!layout.end_to_end) {
            for (const auto& accepted : kept) {
                if (proposal.class_id == accepted.class_id &&
                    intersection_over_union(proposal.box, accepted.box) > options.iou) {
                    suppressed = true;
                    break;
                }
            }
        }
        if (!suppressed) {
            kept.push_back(std::move(proposal));
            if (static_cast<int>(kept.size()) >= options.max_det) break;
        }
    }
    return kept;
}

static void draw_detections(cv::Mat& image, const std::vector<Detection>& detections, float keypoint_threshold) {
    static const std::vector<std::pair<int, int>> coco_skeleton = {
        {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12},
        {5, 6}, {5, 7}, {6, 8}, {7, 9}, {8, 10}, {1, 2}, {0, 1}, {0, 2},
        {1, 3}, {2, 4}, {3, 5}, {4, 6}
    };
    const int thickness = std::max(2, static_cast<int>(std::round(std::min(image.cols, image.rows) / 350.0)));
    for (const auto& detection : detections) {
        for (const auto& [a, b] : coco_skeleton) {
            if (a >= static_cast<int>(detection.keypoints.size()) || b >= static_cast<int>(detection.keypoints.size())) continue;
            const auto& p1 = detection.keypoints[a];
            const auto& p2 = detection.keypoints[b];
            if (p1.confidence < keypoint_threshold || p2.confidence < keypoint_threshold) continue;
            if (p1.x < 0 || p1.y < 0 || p1.x >= image.cols || p1.y >= image.rows ||
                p2.x < 0 || p2.y < 0 || p2.x >= image.cols || p2.y >= image.rows) continue;
            cv::line(image, cv::Point(cvRound(p1.x), cvRound(p1.y)),
                     cv::Point(cvRound(p2.x), cvRound(p2.y)), cv::Scalar(0, 165, 255), thickness, cv::LINE_AA);
        }
        for (const auto& point : detection.keypoints) {
            if (point.confidence >= keypoint_threshold && point.x >= 0 && point.y >= 0 &&
                point.x < image.cols && point.y < image.rows) {
                cv::circle(image, cv::Point(cvRound(point.x), cvRound(point.y)),
                           thickness + 2, cv::Scalar(0, 255, 255), -1, cv::LINE_AA);
            }
        }
        cv::rectangle(image, detection.box, cv::Scalar(60, 220, 60), thickness, cv::LINE_AA);
        std::ostringstream label;
        label << (detection.class_id == 0 ? "person" : "class" + std::to_string(detection.class_id))
              << ' ' << std::fixed << std::setprecision(2) << detection.confidence;
        int baseline = 0;
        const cv::Size text = cv::getTextSize(label.str(), cv::FONT_HERSHEY_SIMPLEX, 0.55, 1, &baseline);
        const int x = std::max(0, cvRound(detection.box.x));
        const int y = std::max(text.height + 6, cvRound(detection.box.y));
        cv::rectangle(image, cv::Rect(x, y - text.height - 6, text.width + 8, text.height + 8),
                      cv::Scalar(60, 220, 60), -1);
        cv::putText(image, label.str(), cv::Point(x + 4, y), cv::FONT_HERSHEY_SIMPLEX,
                    0.55, cv::Scalar(10, 25, 10), 1, cv::LINE_AA);
    }
}

struct FrameResult {
    std::vector<Detection> detections;
    float maximum_class_score = 0;
    double preprocess_ms = 0;
    double inference_ms = 0;
    double postprocess_ms = 0;
};

static FrameResult process_frame(RknnModel& model, const OutputLayout& layout,
                                 const Options& options, cv::Mat& frame) {
    const auto start = Clock::now();
    LetterboxInfo transform;
    cv::Mat input = letterbox(frame, model.width(), model.height(), transform);
    const auto preprocessed = Clock::now();
    auto outputs = model.infer(input);
    const auto inferred = Clock::now();
    if (layout.output_index >= outputs.values.size() || !outputs.values[layout.output_index].buf)
        fail("RKNN 返回了空的 Pose 输出");
    const auto* data = static_cast<const float*>(outputs.values[layout.output_index].buf);
    float maximum_class_score = 0.0F;
    auto detections = decode_pose(data, layout, options, transform, frame.size(), maximum_class_score);
    draw_detections(frame, detections, options.kpt_conf);
    const auto finished = Clock::now();
    auto milliseconds = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    return {std::move(detections), maximum_class_score, milliseconds(start, preprocessed),
            milliseconds(preprocessed, inferred), milliseconds(inferred, finished)};
}

static bool is_camera_number(const std::string& source, int& index) {
    if (source.empty() || !std::all_of(source.begin(), source.end(), ::isdigit)) return false;
    index = parse_int(source, "--source");
    return true;
}

static fs::path default_output(const std::string& source, bool image) {
    fs::path directory = fs::current_path() / "output";
    if (image) {
        fs::path input(source);
        return directory / (input.stem().string() + "_pose.jpg");
    }
    int camera = 0;
    if (is_camera_number(source, camera)) return directory / ("camera" + std::to_string(camera) + "_pose.mp4");
    return directory / (fs::path(source).stem().string() + "_pose.mp4");
}

static void ensure_parent(const fs::path& output) {
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
}

static void run_image(RknnModel& model, const OutputLayout& layout,
                      const Options& options, cv::Mat image) {
    auto result = process_frame(model, layout, options, image);
    std::cout << "检测到 " << result.detections.size() << " 个目标 | 预处理 "
              << std::fixed << std::setprecision(2) << result.preprocess_ms << " ms | NPU "
              << result.inference_ms << " ms | 后处理+绘制 " << result.postprocess_ms << " ms\n";
    std::cout << "所有候选中的最大类别置信度: " << std::fixed << std::setprecision(6)
              << result.maximum_class_score << "\n";
    if (result.maximum_class_score < options.conf) {
        const auto& attr = model.output_attrs()[layout.output_index];
        std::cerr << "警告: 最大类别置信度低于阈值 " << options.conf;
        if (attr.type == RKNN_TENSOR_INT8 && attr.scale > 1.0F) {
            std::cerr << "；当前 INT8 输出 scale=" << attr.scale
                      << "，无法分辨 0～1 的类别概率，模型输出量化可能已经丢失置信度信息";
        }
        std::cerr << "\n";
    }
    for (size_t i = 0; i < result.detections.size(); ++i) {
        const auto& d = result.detections[i];
        std::cout << "  #" << i << " score=" << std::setprecision(3) << d.confidence
                  << " box=[" << cvRound(d.box.x) << ',' << cvRound(d.box.y) << ','
                  << cvRound(d.box.width) << ',' << cvRound(d.box.height) << "]\n";
    }
    if (!options.no_save) {
        const fs::path output = options.output.empty() ? default_output(options.source, true) : fs::path(options.output);
        ensure_parent(output);
        if (!cv::imwrite(output.string(), image)) fail("保存图片失败: " + output.string());
        std::cout << "结果已保存: " << fs::absolute(output) << "\n";
    }
    if (options.show) {
        cv::imshow("YOLO26 Pose RKNN", image);
        std::cout << "按任意键关闭窗口\n";
        cv::waitKey(0);
    }
}

static void run_stream(RknnModel& model, const OutputLayout& layout, const Options& options) {
    cv::VideoCapture capture;
    int camera_index = 0;
    const bool camera = is_camera_number(options.source, camera_index);
    if (camera) capture.open(camera_index);
    else capture.open(options.source);
    if (!capture.isOpened()) fail("无法打开视频或摄像头: " + options.source);

    cv::Mat frame;
    if (!capture.read(frame) || frame.empty()) fail("无法读取第一帧: " + options.source);
    const fs::path output = options.output.empty() ? default_output(options.source, false) : fs::path(options.output);
    cv::VideoWriter writer;
    if (!options.no_save) {
        ensure_parent(output);
        double fps = capture.get(cv::CAP_PROP_FPS);
        if (!std::isfinite(fps) || fps < 1 || fps > 240) fps = 25;
        writer.open(output.string(), cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, frame.size());
        if (!writer.isOpened()) fail("无法创建输出视频: " + output.string());
    }

    int count = 0;
    double npu_total = 0;
    const auto stream_start = Clock::now();
    do {
        auto result = process_frame(model, layout, options, frame);
        npu_total += result.inference_ms;
        ++count;
        if (writer.isOpened()) writer.write(frame);
        if (options.show) {
            cv::imshow("YOLO26 Pose RKNN", frame);
            const int key = cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q') break;
        }
        if (count == 1 || count % 30 == 0) {
            const double elapsed = std::chrono::duration<double>(Clock::now() - stream_start).count();
            std::cout << "frame=" << count << " detections=" << result.detections.size()
                      << " NPU=" << std::fixed << std::setprecision(2) << result.inference_ms
                      << "ms average_fps=" << count / std::max(elapsed, 1e-9) << '\n';
        }
        if (options.max_frames > 0 && count >= options.max_frames) break;
    } while (capture.read(frame) && !frame.empty());

    const double seconds = std::chrono::duration<double>(Clock::now() - stream_start).count();
    std::cout << "处理完成: " << count << " 帧，端到端平均 " << std::fixed << std::setprecision(2)
              << count / std::max(seconds, 1e-9) << " FPS，平均 NPU "
              << npu_total / std::max(count, 1) << " ms\n";
    if (writer.isOpened()) std::cout << "结果已保存: " << fs::absolute(output) << "\n";
}

int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);
        const fs::path model_path = resolve_model(options.model);
        std::cout << "加载模型: " << model_path << "\n";
        RknnModel model(model_path, parse_core(options.core));
        const OutputLayout layout = find_pose_output(model.output_attrs(), options.classes, options.keypoints);
        if (options.inspect) {
            std::cout << "模型结构检查通过，符合 YOLO Pose " << options.classes << " 类 / "
                      << options.keypoints << " 关键点输出。\n";
            return 0;
        }

        cv::Mat image = cv::imread(options.source, cv::IMREAD_COLOR);
        if (!image.empty()) run_image(model, layout, options, std::move(image));
        else run_stream(model, layout, options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "错误: " << error.what() << '\n';
        return 1;
    }
}
