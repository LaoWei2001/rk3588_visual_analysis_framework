#include "config_validator.h"
#include <set>
#include <sys/stat.h>

namespace
{
bool is_supported_model_type(const std::string &model_type)
{
    return model_type == "yolov5" || model_type == "yolov5_seg" || model_type == "yolov8_pose" ||
           model_type == "yolov8_det";
}

bool model_type_requires_label(const std::string &model_type)
{
    return model_type == "yolov5" || model_type == "yolov5_seg" || model_type == "yolov8_det";
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

    // FPS
    if (cfg.max_fps <= 0)
    {
        errors.push_back({"global.max_fps", "必须 > 0"});
        valid = false;
    }
    if (cfg.preview_fps <= 0 || cfg.preview_fps > 60)
    {
        errors.push_back({"global.preview_fps", "必须在 1~60 之间"});
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
        // file 类型：路径为非空即合法（相对/绝对路径均允许），已在上方 empty 检查中覆盖

        // 视频编码仅 RTSP 需要校验
        if (src_type == "rtsp" && ch.stream.video_enc != "h264" && ch.stream.video_enc != "h265")
        {
            errors.push_back({prefix + ".stream.video_enc", "必须是h264或h265"});
            valid = false;
        }

        // 模型只允许出现在 models[]，每个启用项独立校验。
        std::set<std::string> model_ids;
        for (size_t model_index = 0; model_index < ch.models.size(); ++model_index)
        {
            const auto &model = ch.models[model_index];
            const std::string mp = prefix + ".models[" + std::to_string(model_index) + "]";
            if (model.id.empty())
            {
                errors.push_back({mp + ".id", "模型ID不能为空"});
                valid = false;
            }
            else if (!model_ids.insert(model.id).second)
            {
                errors.push_back({mp + ".id", "模型ID在当前通道内重复: " + model.id});
                valid = false;
            }
            if (!model.enable)
                continue;
            if (model.model_path.empty() || !file_exists(model.model_path))
            {
                errors.push_back({mp + ".model_path", "文件不存在: " + model.model_path});
                valid = false;
            }
            const std::string type = config_utils::to_lower_copy(model.model_type);
            if (type.empty() || !is_supported_model_type(type))
            {
                errors.push_back({mp + ".model_type", "无效的模型类型: " + type});
                valid = false;
            }
            if (model_type_requires_label(type) && model.label_path.empty())
            {
                errors.push_back({mp + ".label_path", "该模型类型需要label_path"});
                valid = false;
            }
            else if (!model.label_path.empty() && !file_exists(model.label_path))
            {
                errors.push_back({mp + ".label_path", "文件不存在: " + model.label_path});
                valid = false;
            }
            if (model.obj_thresh < 0.0f || model.obj_thresh > 1.0f || model.nms_thresh < 0.0f ||
                model.nms_thresh > 1.0f)
            {
                errors.push_back({mp + ".threshold", "obj_thresh/nms_thresh必须在[0,1]范围内"});
                valid = false;
            }
        }
    }

    return valid;
}

bool ConfigValidator::validate_critical(const AppConfig &cfg, std::vector<ValidationError> &errors)
{
    errors.clear();
    bool valid = true;

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

        std::set<std::string> model_ids;
        for (size_t model_index = 0; model_index < ch.models.size(); ++model_index)
        {
            const auto &model = ch.models[model_index];
            const std::string mp = prefix + ".models[" + std::to_string(model_index) + "]";
            if (model.id.empty())
            {
                errors.push_back({mp + ".id", "模型ID不能为空"});
                valid = false;
            }
            else if (!model_ids.insert(model.id).second)
            {
                errors.push_back({mp + ".id", "模型ID在当前通道内重复: " + model.id});
                valid = false;
            }
            if (!model.enable)
                continue;
            if (model.model_path.empty() || !file_exists(model.model_path))
            {
                errors.push_back({mp + ".model_path", "文件不存在: " + model.model_path});
                valid = false;
            }
            const std::string type = config_utils::to_lower_copy(model.model_type);
            if (type.empty() || !is_supported_model_type(type))
            {
                errors.push_back({mp + ".model_type", "无效的模型类型: " + type});
                valid = false;
            }
            if (model_type_requires_label(type) && model.label_path.empty())
            {
                errors.push_back({mp + ".label_path", "该模型类型需要label_path"});
                valid = false;
            }
            else if (!model.label_path.empty() && !file_exists(model.label_path))
            {
                errors.push_back({mp + ".label_path", "文件不存在: " + model.label_path});
                valid = false;
            }
            if (model.obj_thresh < 0.0f || model.obj_thresh > 1.0f || model.nms_thresh < 0.0f ||
                model.nms_thresh > 1.0f)
            {
                errors.push_back({mp + ".threshold", "obj_thresh/nms_thresh必须在[0,1]范围内"});
                valid = false;
            }
        }
    }

    return valid;
}
