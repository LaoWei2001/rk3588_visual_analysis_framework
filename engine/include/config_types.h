/** @file config_types.h
 * @brief 通用配置数据：只读查看帧源、模型和通道配置；模块业务参数使用 ctx->param_*()。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <version.h>

static constexpr int MAX_CHANNEL_NUM = vision::MAX_CHANNELS;

/*======================== 流配置 ========================*/
struct StreamConfig
{
    std::string src_type; /* 必填: "rtsp"/"file"/"usb"（前后端均不再自动推断，缺省=配置错误） */
    std::string url;
    std::string device;    /* USB设备节点, 例如 "/dev/video0" */
    std::string video_enc; /* "h264" 或 "h265" */
    bool loop = false;     /* 文件播放循环（仅 src_type=file 有效） */
    int usb_width = 0;     /* USB 显式采集分辨率；0 表示默认 1280x720 */
    int usb_height = 0;
};

/*======================== ROI 区域配置 (从 config.json 通道字段加载) ========================*/
struct RoiZoneConfig
{
    std::string name;
    std::vector<std::pair<double, double>> polygon;
};

/*======================== 推理 ROI 配置 ========================*/
struct InferenceRoiConfig
{
    /* 空字符串表示整帧推理。配置只保存 polygon；矩形同样表示为四顶点多边形。
     * x/y/width/height 是加载时计算的运行时包围框，不对应 JSON 字段。
     * stretch/letterbox 会把非矩形多边形外输入填黑；expand 保留扩展后的周边上下文。 */
    std::string mode;
    std::string resize_mode = "stretch"; /* stretch / expand / letterbox */
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    std::vector<std::pair<double, double>> polygon;

    bool roi_only() const
    {
        return mode == "roi_only";
    }
    bool full_plus_roi() const
    {
        return mode == "full_plus_roi";
    }
    bool full_frame_roi_filter() const
    {
        return mode == "full_frame_roi_filter";
    }
    bool restricts_results_to_roi() const
    {
        return roi_only() || full_frame_roi_filter();
    }
    bool has_roi() const
    {
        return roi_only() || full_plus_roi() || full_frame_roi_filter();
    }
    bool has_polygon() const
    {
        return polygon.size() >= 3;
    }
    bool is_axis_aligned_rectangle() const
    {
        if (polygon.size() != 4 || width <= 0.0 || height <= 0.0)
            return false;
        const double right = x + width;
        const double bottom = y + height;
        bool corners[4] = {false, false, false, false};
        for (const auto &point : polygon)
        {
            const bool left_x = std::abs(point.first - x) <= 1e-9;
            const bool right_x = std::abs(point.first - right) <= 1e-9;
            const bool top_y = std::abs(point.second - y) <= 1e-9;
            const bool bottom_y = std::abs(point.second - bottom) <= 1e-9;
            if ((!left_x && !right_x) || (!top_y && !bottom_y))
                return false;
            const size_t index = (bottom_y ? 2U : 0U) + (right_x ? 1U : 0U);
            if (corners[index])
                return false;
            corners[index] = true;
        }
        return corners[0] && corners[1] && corners[2] && corners[3];
    }
};

inline bool operator==(const InferenceRoiConfig &a, const InferenceRoiConfig &b)
{
    return a.mode == b.mode && a.resize_mode == b.resize_mode && a.x == b.x && a.y == b.y && a.width == b.width &&
           a.height == b.height && a.polygon == b.polygon;
}
inline bool operator!=(const InferenceRoiConfig &a, const InferenceRoiConfig &b)
{
    return !(a == b);
}

/*======================== report_policy 派生的事件录像运行参数 ========================*/
struct EventVideoRuntimeConfig
{
    bool enable = false;
    float pre_sec = 3.0f;
    float post_sec = 3.0f;
    int fps = 15;
    std::string overlay = "custom"; /* none=源帧；custom/all=按实时播放规则渲染 */
};

/*======================== 单通道模型配置 ========================*/
struct ChannelModelConfig
{
    std::string id; /* 通道内稳定模型ID；Web画布与 OTA 均使用 */
    bool enable = true;
    std::string model_type;
    std::string model_path;
    std::string label_path;
    std::string version; /* OTA 版本；空表示未设置 */
    float obj_thresh = 0.3f;
    float nms_thresh = 0.45f;
    std::vector<std::string> detect_classes;
    int npu_core = -1; /* -1=RKNN runtime 自动调度, 0/1/2=固定 NPU 核 */
};

inline bool operator==(const ChannelModelConfig &a, const ChannelModelConfig &b)
{
    return a.id == b.id && a.enable == b.enable && a.model_type == b.model_type && a.model_path == b.model_path &&
           a.label_path == b.label_path && a.version == b.version && a.obj_thresh == b.obj_thresh &&
           a.nms_thresh == b.nms_thresh && a.detect_classes == b.detect_classes && a.npu_core == b.npu_core;
}
inline bool operator!=(const ChannelModelConfig &a, const ChannelModelConfig &b)
{
    return !(a == b);
}

/*======================== 针对通道的配置(被下面的全局配置AppConfig包含) ========================*/
struct ChannelConfig
{
    int id = -1;
    bool enable = true;
    bool infer_enable = true; /* false=不进 NPU；仍解码/显示，并在 max_fps 节流命中的业务帧以空 results 调用后处理 */
    bool swap_rb = false; /* 仅显示: 1=该通道画面 R/B 互换显示(跳过显示前 BGR→RGB);不影响推理/上报 */
    StreamConfig stream;
    std::string logic = ""; /* 可选后处理模块；空=不执行模块，仅保留视频/模型结果绘制 */
    std::vector<ChannelModelConfig> models; /* 唯一模型配置入口；空表示该通道不做模型推理 */
    InferenceRoiConfig inference_roi; /* 通道内全部模型共享的单个推理/结果区域；缺省为整帧 */
    std::vector<RoiZoneConfig> roi_zones; /* 多ROI区域(名称+归一化顶点), 空=无区域 */
    /* 逻辑模块专有参数：由模块 logic.json Schema 统一定义和校验。
     * 配置文件键为 logic_parameters；新增普通逻辑参数不再扩展 ChannelConfig。 */
    std::string logic_parameters_json = "{}";
    int threads = -1; /* 单通道并发线程数, <0表示使用全局设置 */
    int max_fps = -1; /* 推理/业务处理帧率上限，<0表示继承全局设置 */

    /* 跟踪器 (全局默认, 可被通道覆盖) */
    int tracker_enable = -1;               /* -1=未指定(继承全局), 0=关闭, 1=开启 */
    std::string tracker_type = "";         /* 空=继承全局, "sort"/"bytetrack" */
    float tracker_iou_thresh = 0.3f;       /* IoU 匹配阈值 (0~1), 低于此值视为不匹配 */
    int tracker_max_miss = 10;             /* 连续丢失上限, 超限删除轨迹 */
    int tracker_min_hits = 3;              /* 确认轨迹所需的最小命中帧数 */
    float bytetrack_low_thresh = 0.1f;     /* 低分检测的最低置信度 */
    float bytetrack_low_iou_thresh = 0.2f; /* 第二轮低分关联的最小 IoU */

    /* 通用告警配置：Web 直接保存对象/数组，C++ 以 JSON 文本解析，新增参数无需改结构体。 */
    std::string report_policy_json = "{}";
    std::string report_parameters_json = "{}";
    EventVideoRuntimeConfig event_video; /* 仅运行时使用，不对应独立 JSON 字段 */
};

/*======================== 全局逻辑配置 (支持多个并行实例) ========================*/
struct GlobalLogicConfig
{
    std::string instance_id;              /* Web/配置持久化的稳定实例 ID，用于实例级热更新 */
    bool enable = false;                  /* 是否启用 */
    std::string logic = "global_default"; /* 逻辑名称 */
    std::vector<int> channels;            /* Web 画布连入的通道列表；空表示没有画布输入 */
    int poll_interval_ms = 100;           /* 无通道更新时的兜底运行间隔 (毫秒) */
    /* 全局逻辑模块专有参数：由 global_modules/<name>/logic.json 统一定义和校验。 */
    std::string logic_parameters_json = "{}";
    /* 与 ChannelConfig 完全相同的统一事件上报配置，由画布上的上报节点生成。 */
    std::string report_policy_json = "{}";
    std::string report_parameters_json = "{}";
    /* 事件视频的唯一预录来源；启用视频上报时必须明确设置。 */
    int media_source_channel_id = -1;
    EventVideoRuntimeConfig event_video;
};

/* 用于热重载时检测 global_logics 数组是否变化, 任一字段不同即视为变化 */
inline bool operator==(const GlobalLogicConfig &a, const GlobalLogicConfig &b)
{
    return a.instance_id == b.instance_id && a.enable == b.enable && a.logic == b.logic && a.channels == b.channels &&
           a.poll_interval_ms == b.poll_interval_ms && a.logic_parameters_json == b.logic_parameters_json &&
           a.report_policy_json == b.report_policy_json && a.report_parameters_json == b.report_parameters_json &&
           a.media_source_channel_id == b.media_source_channel_id;
}
inline bool operator!=(const GlobalLogicConfig &a, const GlobalLogicConfig &b)
{
    return !(a == b);
}
