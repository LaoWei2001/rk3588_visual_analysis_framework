#include "config_validator.h"
#include <sys/stat.h>

namespace
{
bool is_supported_model_type(const std::string &model_type)
{
    return model_type == "yolo" || model_type == "yolov5" || model_type == "yolov5_seg" ||
           model_type == "yolov8_pose" || model_type == "yolov8_det";
}

bool model_type_requires_label(const std::string &model_type)
{
    return model_type == "yolo" || model_type == "yolov5" || model_type == "yolov5_seg" || model_type == "yolov8_det";
}
} // namespace

bool ConfigValidator::file_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool ConfigValidator::is_valid_url(const std::string &url)
{
    if (url.empty())
        return false;
    if (url.substr(0, 7) == "rtsp://")
        return true;
    if (url.substr(0, 8) == "rtsps://")
        return true;
    if (url.substr(0, 7) == "http://")
        return true;
    if (url.substr(0, 8) == "https://")
        return true;
    if (url[0] == '/')
        return true; // 本地文件绝对路径
    return false;
}

bool ConfigValidator::validate(const AppConfig &cfg, std::vector<ValidationError> &errors)
{
    errors.clear();
    bool valid = true;
    valid &= validate_global(cfg, errors);
    valid &= validate_channels(cfg, errors);
    return valid;
}

bool ConfigValidator::validate_global(const AppConfig &cfg, std::vector<ValidationError> &errors)
{
    bool valid = true;

    // 模型路径（可选，配置了才检查）
    if (!cfg.model_path.empty() && !file_exists(cfg.model_path))
    {
        errors.push_back({"global.model_path", "文件不存在: " + cfg.model_path});
        valid = false;
    }

    // 标签路径（可选，配置了才检查）
    if (!cfg.label_path.empty() && !file_exists(cfg.label_path))
    {
        errors.push_back({"global.label_path", "文件不存在: " + cfg.label_path});
        valid = false;
    }

    // 模型类型（可选，配置了才检查）
    if (!cfg.model_type.empty() && !is_supported_model_type(cfg.model_type))
    {
        errors.push_back({"global.model_type", "无效的模型类型: " + cfg.model_type});
        valid = false;
    }

    // 显示配置
    if (cfg.disp_width <= 0)
    {
        errors.push_back({"global.disp_width", "必须 > 0"});
        valid = false;
    }
    if (cfg.disp_height <= 0)
    {
        errors.push_back({"global.disp_height", "必须 > 0"});
        valid = false;
    }
    if (cfg.tile_cols <= 0)
    {
        errors.push_back({"global.tile_cols", "必须 > 0"});
        valid = false;
    }
    if (cfg.tile_rows <= 0)
    {
        errors.push_back({"global.tile_rows", "必须 > 0"});
        valid = false;
    }

    // NPU核心
    if (cfg.npu_cores < 1 || cfg.npu_cores > 3)
    {
        errors.push_back({"global.npu_cores", "必须在[1, 3]范围内"});
        valid = false;
    }

    // 阈值
    if (cfg.obj_thresh < 0.0f || cfg.obj_thresh > 1.0f)
    {
        errors.push_back({"global.obj_thresh", "必须在[0, 1]范围内"});
        valid = false;
    }
    if (cfg.nms_thresh < 0.0f || cfg.nms_thresh > 1.0f)
    {
        errors.push_back({"global.nms_thresh", "必须在[0, 1]范围内"});
        valid = false;
    }

    // FPS
    if (cfg.max_fps <= 0)
    {
        errors.push_back({"global.max_fps", "必须 > 0"});
        valid = false;
    }
    if (cfg.local_default_fps <= 0)
    {
        errors.push_back({"global.local_default_fps", "必须 > 0"});
        valid = false;
    }

    return valid;
}

bool ConfigValidator::validate_channels(const AppConfig &cfg, std::vector<ValidationError> &errors)
{
    bool valid = true;

    // 检查至少有一个通道
    if (cfg.channels.empty())
    {
        errors.push_back({"channels", "至少需要一个启用的通道"});
        return false;
    }

    // 检查显示网格容量
    int grid_capacity = cfg.tile_cols * cfg.tile_rows;
    int enabled_count = cfg.channels.size();
    if (cfg.enable_display && grid_capacity < enabled_count)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "显示网格容量不足: 需要%d个单元格，但只有%d个 (%d*%d)", enabled_count, grid_capacity,
                 cfg.tile_cols, cfg.tile_rows);
        errors.push_back({"global.tile_cols/tile_rows", buf});
        valid = false;
    }

    // 逐个验证通道
    for (size_t i = 0; i < cfg.channels.size(); ++i)
    {
        const auto &ch = cfg.channels[i];
        std::string prefix = "channels[" + std::to_string(i) + "]";

        std::string src_type = config_utils::normalize_src_type(ch.stream);
        std::string stream_location = config_utils::resolve_stream_location(ch.stream, src_type);

        if (src_type.empty())
        {
            errors.push_back({prefix + ".stream.src_type", "必填: rtsp/file/usb（已取消自动推断，必须显式指定）"});
            valid = false;
        }
        else if (!config_utils::is_supported_src_type(src_type))
        {
            errors.push_back({prefix + ".stream.src_type", "必须是rtsp/file/usb"});
            valid = false;
        }

        // 源地址验证
        if (stream_location.empty())
        {
            errors.push_back({prefix + ".stream", "源地址不能为空(url/device)"});
            valid = false;
        }
        else if (src_type == "usb")
        {
            if (!config_utils::starts_with(stream_location, "/dev/video"))
            {
                errors.push_back({prefix + ".stream.device", "USB设备节点必须是/dev/video*"});
                valid = false;
            }
        }
        else if (src_type == "rtsp")
        {
            // RTSP 需要合法的 URL scheme
            if (!is_valid_url(stream_location))
            {
                errors.push_back({prefix + ".stream.url", "RTSP地址必须以 rtsp:// 或 rtsps:// 开头"});
                valid = false;
            }
        }
        // file 类型：路径为非空即合法（相对/绝对路径均允许），已在上方 empty
        // 检查中覆盖

        // 视频编码仅 RTSP 需要校验
        if (src_type == "rtsp" && ch.stream.video_enc != "h264" && ch.stream.video_enc != "h265")
        {
            errors.push_back({prefix + ".stream.video_enc", "必须是h264或h265"});
            valid = false;
        }

        // 通道模型配置：仅当指定 model_path 时启用 YOLO 推理。
        if (ch.model_path.empty())
        {
            continue;
        }

        std::string model_type = ch.model_type.empty() ? cfg.model_type : config_utils::to_lower_copy(ch.model_type);
        if (model_type.empty())
        {
            errors.push_back({prefix + ".model_type", "启用YOLO时必须指定model_type（或设置global.model_type）"});
            valid = false;
            continue;
        }

        if (!is_supported_model_type(model_type))
        {
            errors.push_back({prefix + ".model_type", "无效的模型类型: " + model_type});
            valid = false;
        }

        if (!file_exists(ch.model_path))
        {
            errors.push_back({prefix + ".model_path", "文件不存在: " + ch.model_path});
            valid = false;
        }

        std::string label_path = ch.label_path.empty() ? cfg.label_path : ch.label_path;
        if (model_type_requires_label(model_type) && label_path.empty())
        {
            errors.push_back({prefix + ".label_path", "该模型类型需要label_path（通道或global）"});
            valid = false;
        }
        else if (!label_path.empty() && !file_exists(label_path))
        {
            errors.push_back({prefix + ".label_path", "文件不存在: " + label_path});
            valid = false;
        }
    }

    return valid;
}

bool ConfigValidator::validate_critical(const AppConfig &cfg, std::vector<ValidationError> &errors)
{
    errors.clear();
    bool valid = true;

    // 阈值范围 — 阻断型
    if (cfg.obj_thresh < 0.0f || cfg.obj_thresh > 1.0f)
    {
        errors.push_back({"global.obj_thresh", "必须在[0, 1]范围内"});
        valid = false;
    }
    if (cfg.nms_thresh < 0.0f || cfg.nms_thresh > 1.0f)
    {
        errors.push_back({"global.nms_thresh", "必须在[0, 1]范围内"});
        valid = false;
    }

    // 通道关键参数
    valid &= validate_channels_critical(cfg, errors);
    return valid;
}

bool ConfigValidator::validate_channels_critical(const AppConfig &cfg, std::vector<ValidationError> &errors)
{
    bool valid = true;

    if (cfg.channels.empty())
    {
        // 热更新时通道列表不会变空（数量取 min），此处防御性检查
        errors.push_back({"channels", "通道列表不能为空"});
        return false;
    }

    for (size_t i = 0; i < cfg.channels.size(); ++i)
    {
        const auto &ch = cfg.channels[i];
        std::string prefix = "channels[" + std::to_string(i) + "]";

        // 仅检查启用了推理的通道
        if (ch.model_path.empty())
            continue;

        // 模型文件必须存在 — 阻断型
        if (!file_exists(ch.model_path))
        {
            errors.push_back({prefix + ".model_path", "文件不存在: " + ch.model_path});
            valid = false;
        }

        // 模型类型必须合法 — 阻断型
        std::string model_type = ch.model_type.empty() ? cfg.model_type : config_utils::to_lower_copy(ch.model_type);
        if (!model_type.empty() && !is_supported_model_type(model_type))
        {
            errors.push_back({prefix + ".model_type", "无效的模型类型: " + model_type});
            valid = false;
        }

        // 标签文件
        std::string label_path = ch.label_path.empty() ? cfg.label_path : ch.label_path;
        if (!label_path.empty() && !file_exists(label_path))
        {
            errors.push_back({prefix + ".label_path", "文件不存在: " + label_path});
            valid = false;
        }

        // 阈值范围
        if (ch.obj_thresh < 0.0f || ch.obj_thresh > 1.0f)
        {
            errors.push_back({prefix + ".obj_thresh", "必须在[0, 1]范围内"});
            valid = false;
        }
        if (ch.nms_thresh < 0.0f || ch.nms_thresh > 1.0f)
        {
            errors.push_back({prefix + ".nms_thresh", "必须在[0, 1]范围内"});
            valid = false;
        }
    }

    return valid;
}
