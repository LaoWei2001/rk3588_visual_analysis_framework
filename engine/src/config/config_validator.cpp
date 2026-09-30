#include "config_validator.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <sys/stat.h>

namespace
{
bool is_supported_model_type(const std::string &model_type)
{
    return model_type == "yolov5" || model_type == "yolov5_seg" || model_type == "yolov8_pose" ||
           model_type == "yolo26_pose" || model_type == "yolov8_det";
}

bool model_type_requires_label(const std::string &model_type)
{
    return model_type == "yolov5" || model_type == "yolov5_seg" || model_type == "yolov8_det";
}

bool point_in_polygon(double x, double y, const std::vector<std::pair<double, double>> &polygon)
{
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
    {
        const auto &a = polygon[i];
        const auto &b = polygon[j];
        const double cross = (x - a.first) * (b.second - a.second) - (y - a.second) * (b.first - a.first);
        if (std::abs(cross) <= 1e-9 && x >= std::min(a.first, b.first) - 1e-9 &&
            x <= std::max(a.first, b.first) + 1e-9 && y >= std::min(a.second, b.second) - 1e-9 &&
            y <= std::max(a.second, b.second) + 1e-9)
            return true;
        const bool crosses = (a.second > y) != (b.second > y);
        if (crosses && x < (b.first - a.first) * (y - a.second) / (b.second - a.second) + a.first)
            inside = !inside;
    }
    return inside;
}

bool inference_region_contains(double x, double y, const InferenceRoiConfig &roi)
{
    if (!roi.has_polygon())
        return x >= roi.x - 1e-9 && x <= roi.x + roi.width + 1e-9 && y >= roi.y - 1e-9 &&
               y <= roi.y + roi.height + 1e-9;
    return point_in_polygon(x, y, roi.polygon);
}

double orientation(const std::pair<double, double> &a, const std::pair<double, double> &b,
                   const std::pair<double, double> &c)
{
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}

bool point_on_segment(const std::pair<double, double> &point, const std::pair<double, double> &a,
                      const std::pair<double, double> &b)
{
    return std::abs(orientation(a, b, point)) <= 1e-10 && point.first >= std::min(a.first, b.first) - 1e-10 &&
           point.first <= std::max(a.first, b.first) + 1e-10 && point.second >= std::min(a.second, b.second) - 1e-10 &&
           point.second <= std::max(a.second, b.second) + 1e-10;
}

bool segments_intersect(const std::pair<double, double> &a, const std::pair<double, double> &b,
                        const std::pair<double, double> &c, const std::pair<double, double> &d)
{
    const double o1 = orientation(a, b, c), o2 = orientation(a, b, d);
    const double o3 = orientation(c, d, a), o4 = orientation(c, d, b);
    if (((o1 > 1e-10 && o2 < -1e-10) || (o1 < -1e-10 && o2 > 1e-10)) &&
        ((o3 > 1e-10 && o4 < -1e-10) || (o3 < -1e-10 && o4 > 1e-10)))
        return true;
    return point_on_segment(c, a, b) || point_on_segment(d, a, b) || point_on_segment(a, c, d) ||
           point_on_segment(b, c, d);
}

bool valid_inference_polygon(const InferenceRoiConfig &roi)
{
    if (roi.shape != "polygon")
        return roi.shape == "rect";
    if (roi.polygon.size() < 3 || roi.polygon.size() > 64)
        return false;
    double twice_area = 0.0;
    for (size_t i = 0; i < roi.polygon.size(); ++i)
    {
        const auto &a = roi.polygon[i];
        const auto &b = roi.polygon[(i + 1) % roi.polygon.size()];
        if (!std::isfinite(a.first) || !std::isfinite(a.second) || a.first < 0.0 || a.first > 1.0 || a.second < 0.0 ||
            a.second > 1.0)
            return false;
        if (std::abs(a.first - b.first) <= 1e-10 && std::abs(a.second - b.second) <= 1e-10)
            return false;
        twice_area += a.first * b.second - b.first * a.second;
    }
    if (std::abs(twice_area) <= 1e-8)
        return false;
    const size_t count = roi.polygon.size();
    for (size_t i = 0; i < count; ++i)
        for (size_t j = i + 1; j < count; ++j)
        {
            if (j == i + 1 || (i == 0 && j == count - 1))
                continue;
            if (segments_intersect(roi.polygon[i], roi.polygon[(i + 1) % count], roi.polygon[j],
                                   roi.polygon[(j + 1) % count]))
                return false;
        }
    return true;
}

bool valid_business_polygon(const std::vector<std::pair<double, double>> &polygon)
{
    InferenceRoiConfig probe;
    probe.shape = "polygon";
    probe.polygon = polygon;
    return valid_inference_polygon(probe);
}

double cross(double ax, double ay, double bx, double by)
{
    return ax * by - ay * bx;
}

bool segment_inside_inference_polygon(const std::pair<double, double> &a, const std::pair<double, double> &b,
                                      const InferenceRoiConfig &roi)
{
    if (!inference_region_contains(a.first, a.second, roi) || !inference_region_contains(b.first, b.second, roi))
        return false;
    if (!roi.has_polygon())
        return true;

    const double rx = b.first - a.first, ry = b.second - a.second;
    std::vector<double> parameters{0.0, 1.0};
    for (size_t edge = 0; edge < roi.polygon.size(); ++edge)
    {
        const auto &c = roi.polygon[edge];
        const auto &d = roi.polygon[(edge + 1) % roi.polygon.size()];
        const double sx = d.first - c.first, sy = d.second - c.second;
        const double qx = c.first - a.first, qy = c.second - a.second;
        const double denominator = cross(rx, ry, sx, sy);
        if (std::abs(denominator) > 1e-12)
        {
            const double t = cross(qx, qy, sx, sy) / denominator;
            const double u = cross(qx, qy, rx, ry) / denominator;
            if (t >= -1e-10 && t <= 1.0 + 1e-10 && u >= -1e-10 && u <= 1.0 + 1e-10)
                parameters.push_back(std::max(0.0, std::min(1.0, t)));
        }
        else if (std::abs(cross(qx, qy, rx, ry)) <= 1e-12)
        {
            const double length_squared = rx * rx + ry * ry;
            if (length_squared > 1e-16)
                for (const auto &point : {c, d})
                {
                    const double t = ((point.first - a.first) * rx + (point.second - a.second) * ry) / length_squared;
                    if (t >= -1e-10 && t <= 1.0 + 1e-10)
                        parameters.push_back(std::max(0.0, std::min(1.0, t)));
                }
        }
    }
    std::sort(parameters.begin(), parameters.end());
    for (size_t i = 1; i < parameters.size(); ++i)
    {
        if (parameters[i] - parameters[i - 1] <= 1e-10)
            continue;
        const double t = (parameters[i] + parameters[i - 1]) * 0.5;
        if (!inference_region_contains(a.first + rx * t, a.second + ry * t, roi))
            return false;
    }
    return true;
}

bool inference_region_contains_polygon(const std::vector<std::pair<double, double>> &polygon,
                                       const InferenceRoiConfig &roi)
{
    if (polygon.size() < 3)
        return false;
    for (size_t i = 0; i < polygon.size(); ++i)
        if (!segment_inside_inference_polygon(polygon[i], polygon[(i + 1) % polygon.size()], roi))
            return false;
    return true;
}
} // namespace

bool ConfigValidator::file_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
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

        for (size_t zone_index = 0; zone_index < ch.roi_zones.size(); ++zone_index)
            if (!valid_business_polygon(ch.roi_zones[zone_index].polygon))
            {
                errors.push_back({prefix + ".roi_zones[" + std::to_string(zone_index) + "].polygon",
                                  "必须是单个有效且不自相交的归一化多边形（3~64 个顶点）"});
                valid = false;
            }

        if (!ch.inference_roi.mode.empty() && !ch.inference_roi.has_roi())
        {
            errors.push_back(
                {prefix + ".inference_roi.mode", "必须是 roi_only、full_plus_roi 或 full_frame_roi_filter"});
            valid = false;
        }
        if (ch.inference_roi.has_roi())
        {
            const auto &r = ch.inference_roi;
            const bool valid_rect = r.x >= 0.0 && r.y >= 0.0 && r.width > 0.0 && r.height > 0.0 &&
                                    r.x + r.width <= 1.0 + 1e-9 && r.y + r.height <= 1.0 + 1e-9;
            if (!valid_rect)
            {
                errors.push_back(
                    {prefix + ".inference_roi.rect", "必须是位于完整画面内的 [x,y,width,height] 归一化矩形"});
                valid = false;
            }
            if (r.resize_mode != "stretch" && r.resize_mode != "expand" && r.resize_mode != "letterbox")
            {
                errors.push_back({prefix + ".inference_roi.resize_mode", "必须是 stretch、expand 或 letterbox"});
                valid = false;
            }
            if (!valid_inference_polygon(r))
            {
                errors.push_back(
                    {prefix + ".inference_roi.polygon", "必须是单个有效且不自相交的归一化多边形（3~64 个顶点）"});
                valid = false;
            }
            else if (ch.inference_roi.restricts_results_to_roi())
            {
                for (size_t zone_index = 0; zone_index < ch.roi_zones.size(); ++zone_index)
                {
                    const auto &zone = ch.roi_zones[zone_index];
                    if (!inference_region_contains_polygon(zone.polygon, r))
                    {
                        errors.push_back({prefix + ".roi_zones[" + std::to_string(zone_index) + "].polygon",
                                          "结果受限模式下业务 ROI 的顶点和边必须完全位于推理 ROI 内"});
                        valid = false;
                    }
                }
            }
        }

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
        else if (src_type == "file" && !file_exists(stream_location))
        {
            errors.push_back({prefix + ".stream.url", "文件不存在: " + stream_location});
            valid = false;
        }

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
            if (model.npu_core < -1 || model.npu_core > 2)
            {
                errors.push_back({mp + ".npu_core", "必须是auto(-1)、0、1或2"});
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

        for (size_t zone_index = 0; zone_index < ch.roi_zones.size(); ++zone_index)
            if (!valid_business_polygon(ch.roi_zones[zone_index].polygon))
            {
                errors.push_back({prefix + ".roi_zones[" + std::to_string(zone_index) + "].polygon",
                                  "必须是单个有效且不自相交的归一化多边形（3~64 个顶点）"});
                valid = false;
            }

        if (!ch.inference_roi.mode.empty() && !ch.inference_roi.has_roi())
        {
            errors.push_back(
                {prefix + ".inference_roi.mode", "必须是 roi_only、full_plus_roi 或 full_frame_roi_filter"});
            valid = false;
        }
        if (ch.inference_roi.has_roi())
        {
            const auto &r = ch.inference_roi;
            const bool valid_rect = r.x >= 0.0 && r.y >= 0.0 && r.width > 0.0 && r.height > 0.0 &&
                                    r.x + r.width <= 1.0 + 1e-9 && r.y + r.height <= 1.0 + 1e-9;
            if (!valid_rect)
            {
                errors.push_back(
                    {prefix + ".inference_roi.rect", "必须是位于完整画面内的 [x,y,width,height] 归一化矩形"});
                valid = false;
            }
            if (r.resize_mode != "stretch" && r.resize_mode != "expand" && r.resize_mode != "letterbox")
            {
                errors.push_back({prefix + ".inference_roi.resize_mode", "必须是 stretch、expand 或 letterbox"});
                valid = false;
            }
            if (!valid_inference_polygon(r))
            {
                errors.push_back(
                    {prefix + ".inference_roi.polygon", "必须是单个有效且不自相交的归一化多边形（3~64 个顶点）"});
                valid = false;
            }
            for (size_t zone_index = 0;
                 valid_rect && ch.inference_roi.restricts_results_to_roi() && zone_index < ch.roi_zones.size();
                 ++zone_index)
            {
                if (!inference_region_contains_polygon(ch.roi_zones[zone_index].polygon, r))
                {
                    errors.push_back({prefix + ".roi_zones[" + std::to_string(zone_index) + "].polygon",
                                      "结果受限模式下业务 ROI 的顶点和边必须完全位于推理 ROI 内"});
                    valid = false;
                }
            }
        }

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
