/**
 * @file config.h
 * @brief 应用配置定义
 *
 * 集中定义所有配置结构:
 * - 全局参数 (模型路径、显示、NPU核心数、阈值等)
 * - 通道独立配置 (RTSP源、逻辑名称、阈值)
 * - 配置热加载 (文件变更自动重载)
 *
 * 配置文件格式: JSON
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* global_logic.h 被移除了，以防止循环依赖 */

/*======================== 最大通道数 ========================*/
#include "../core/constants.h"
static constexpr int MAX_CHANNEL_NUM = constants::MAX_CHANNELS;

/*======================== 流配置 ========================*/
struct StreamConfig
{
    std::string src_type; /* 必填: "rtsp"/"file"/"usb"（前后端均不再自动推断，缺省=配置错误） */
    std::string url;
    std::string device;    /* USB设备节点, 例如 "/dev/video0" */
    std::string video_enc; /* "h264" 或 "h265" */
    bool loop = false;     /* 文件播放循环（仅 src_type=file 有效） */
    int usb_width = 0; /* USB 显式采集分辨率(0=随 fps 自动档)。与 ROI 抓帧一致、不随 fps 变 → 三者坐标统一 */
    int usb_height = 0;
};

/*======================== ROI 区域配置 (从 config.json 通道字段加载) ========================*/
struct RoiZoneConfig
{
    std::string name;
    std::vector<std::pair<double, double>> polygon;
};

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
    int npu_core = -1;
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
    std::vector<RoiZoneConfig> roi_zones;   /* 多ROI区域(名称+归一化顶点), 空=无区域 */
    /* 逻辑模块专有参数：由模块 logic.json Schema 统一定义和校验。
     * 配置文件键为 logic_parameters；新增普通逻辑参数不再扩展 ChannelConfig。 */
    std::string logic_parameters_json = "{}";
    int threads = -1;      /* 单通道并发线程数, <0表示使用全局设置 */
    int playback_fps = -1; /* 播放/处理帧率上限，<0表示不限制(本地文件默认25) */
    int max_fps = -1;      /* 推理帧率上限，<0表示继承全局设置 */

    /* 跟踪器 (全局默认, 可被通道覆盖) */
    int tracker_enable = -1;         /* -1=未指定(继承全局), 0=关闭, 1=开启 */
    float tracker_iou_thresh = 0.3f; /* IoU 匹配阈值 (0~1), 低于此值视为不匹配 */
    int tracker_max_miss = 10;       /* 连续丢失上限, 超限删除轨迹 */
    int tracker_min_hits = 3;        /* 确认轨迹所需的最小命中帧数 */

    /* 通用告警配置：Web 直接保存对象/数组，C++ 以 JSON 文本解析，新增参数无需改结构体。 */
    std::string report_policy_json = "{}";
    std::string report_parameters_json = "{}";
    EventVideoRuntimeConfig event_video; /* 仅运行时使用，不对应独立 JSON 字段 */
};

/*======================== 全局逻辑配置 (支持多个并行实例) ========================*/
struct GlobalLogicConfig
{
    std::string instance_id;                 /* Web/配置持久化的稳定实例 ID，用于实例级热更新 */
    bool enable = false;                     /* 是否启用 */
    std::string logic = "global_default";    /* 逻辑名称 */
    std::vector<int> channels;               /* Web 画布连入的通道列表；空表示没有画布输入 */
    int poll_interval_ms = 100;              /* 轮询间隔 (毫秒) */
    /* 全局逻辑模块专有参数：由 global_modules/<name>/logic.json 统一定义和校验。 */
    std::string logic_parameters_json = "{}";
    /* 与 ChannelConfig 完全相同的统一事件上报配置，由画布上的上报节点生成。 */
    std::string report_policy_json = "{}";
    std::string report_parameters_json = "{}";
    /* 默认事件/单通道图片来源；启用事件视频时也是唯一预录来源且必须明确设置。 */
    int media_source_channel_id = -1;
    EventVideoRuntimeConfig event_video;
};

/* 用于热重载时检测 global_logics 数组是否变化, 任一字段不同即视为变化 */
inline bool operator==(const GlobalLogicConfig &a, const GlobalLogicConfig &b)
{
    return a.instance_id == b.instance_id && a.enable == b.enable && a.logic == b.logic && a.channels == b.channels &&
           a.poll_interval_ms == b.poll_interval_ms &&
           a.logic_parameters_json == b.logic_parameters_json && a.report_policy_json == b.report_policy_json &&
           a.report_parameters_json == b.report_parameters_json &&
           a.media_source_channel_id == b.media_source_channel_id;
}
inline bool operator!=(const GlobalLogicConfig &a, const GlobalLogicConfig &b)
{
    return !(a == b);
}

/*======================== 全局配置 ========================*/
struct AppConfig
{
    /* 显示 */
    bool enable_display = true;
    int disp_width = 1920;
    int disp_height = 1080;
    int tile_cols = 2;
    int tile_rows = 2;
    bool performance_display = true; /* 性能统计显示开关 */
    bool debug_display = false;      /* 调试信息打印开关 (JSON: debug_display: 1) */
    bool enable_pause_key = false;   /* 暂停键开关: true=按空格可暂停 (需同时开启 enable_display) */

    /* RTSP 推流 (无显示器时通过 VLC / 配置平台查看与显示屏一致的拼接画面) */
    bool enable_rtsp = false;          /* 是否启用内置 RTSP 服务 */
    int rtsp_port = 8554;              /* RTSP 端口, 地址 rtsp://<板IP>:<port><rtsp_path> */
    std::string rtsp_path = "/live";   /* RTSP 挂载点 (须以 '/' 开头) */
    int rtsp_fps = 25;                 /* 推流帧率 */
    int rtsp_bitrate = 4096;           /* 软件编码码率(kbps); 硬件编码用默认码率 */
    std::string rtsp_codec = "h264";   /* "h264" 或 "h265" */
    std::string rtsp_encoder = "auto"; /* "auto"=有硬件就硬编否则软编; "hw"=强制硬编 */

    /* 推理引擎 */
    int channel_threads = 1;                 /* 每个通道并发数默认值 */
    int max_fps = 30;                        /* 每通道推理帧率上限默认值 (从15提高到30) */
    int local_default_fps = 25;              /* 本地文件默认播放采样率 */
    int queue_size = 1;                      /* 每核任务队列深度 */
    /* 跟踪器 (全局默认，可被通道覆盖) */
    int tracker_enable = 1; /* 0=关闭, 1=开启 */
    float tracker_iou_thresh = 0.3f;
    int tracker_max_miss = 10;
    int tracker_min_hits = 3;

    /* 通道列表 */
    std::vector<ChannelConfig> channels;

    /* 全局逻辑（支持多个并行实例） */
    std::vector<GlobalLogicConfig> global_logics;

    /* 配置文件路径 (用于热加载监控) */
    std::string config_path;
};

namespace config_utils
{
bool starts_with(const std::string &value, const char *prefix);
std::string to_lower_copy(const std::string &value);
std::string normalize_src_type(const StreamConfig &stream);
std::string resolve_stream_location(const StreamConfig &stream, const std::string &src_type);
bool is_supported_src_type(const std::string &src_type);
bool is_channel_infer_enabled(const ChannelConfig &ch_cfg);
} // namespace config_utils

/*======================== 接口 ========================*/
/**
 * @brief 从JSON文件加载配置
 * @param path 配置文件路径
 * @param cfg  输出配置结构
 * @return true=成功, false=失败
 */
bool load_config(const std::string &path, AppConfig &cfg);

/**
 * @brief 获取配置文件的最后修改时间 (用于热加载检测)
 * @param path 文件路径
 * @return 修改时间戳, 失败返回0
 */
uint64_t config_get_mtime(const std::string &path);

/**
 * @brief 验证配置有效性
 * @param cfg 配置结构
 * @return true=有效, false=无效
 */
