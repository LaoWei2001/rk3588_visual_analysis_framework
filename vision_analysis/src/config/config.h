/**
 * @file config.h
 * @brief 应用配置定义
 *
 * 集中定义所有配置结构:
 * - 全局参数 (模型路径、显示、NPU核心数、阈值等)
 * - 通道独立配置 (RTSP源、逻辑名称、阈值)
 * - 配置热加载 (文件变更自动重载)
 *
 * 配置文件格式: JSON (schema_version = 2)
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

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
    int usb_width  = 0;    /* USB 显式采集分辨率(0=随 fps 自动档)。与 ROI 抓帧一致、不随 fps 变 → 三者坐标统一 */
    int usb_height = 0;
};

/*======================== ROI 区域配置 (从 config.json 通道字段加载) ========================*/
struct RoiZoneConfig
{
    std::string name;
    std::vector<std::pair<double, double>> polygon;
};

/*======================== 单通道模型配置 ========================*/
struct ChannelModelConfig
{
    std::string id;                           /* Web画布模型节点ID，仅用于区分同通道模型 */
    bool enable = true;
    std::string model_type;
    std::string model_path;
    std::string label_path;
    float obj_thresh = -1.0f;
    float nms_thresh = -1.0f;
    std::vector<std::string> detect_classes;
    int npu_core = -1;
};

inline bool operator==(const ChannelModelConfig &a, const ChannelModelConfig &b)
{
    return a.id == b.id && a.enable == b.enable &&
           a.model_type == b.model_type && a.model_path == b.model_path &&
           a.label_path == b.label_path && a.obj_thresh == b.obj_thresh &&
           a.nms_thresh == b.nms_thresh && a.detect_classes == b.detect_classes &&
           a.npu_core == b.npu_core;
}
inline bool operator!=(const ChannelModelConfig &a, const ChannelModelConfig &b) { return !(a == b); }

/*======================== 针对通道的配置(被下面的全局配置AppConfig包含) ========================*/
struct ChannelConfig
{
    int id = -1;
    bool enable = true;
    bool infer_enable = true;                /* 是否启用 YOLO 推理。false=不进 NPU；仍解码/显示，配置了后处理时以空 results 逐帧调用 */
    bool swap_rb = false;                    /* 仅显示: 1=该通道画面 R/B 互换显示(跳过显示前 BGR→RGB);不影响推理/上报 */
    StreamConfig stream;
    std::string logic = "";                  /* 可选后处理模块；空=不执行模块，仅保留视频/模型结果绘制 */
    std::string model_type = "";             /* 模型类型: "yolov5"/"yolov8_det"/"yolov8_pose"/"yolov5_seg" */
    std::string model_path = "";             /* 模型路径，为空表示该通道不做YOLO推理 */
    std::string label_path = "";             /* 标签路径（检测/分割模型需要） */
    std::vector<ChannelModelConfig> models;   /* 多模型配置；非空时优先于上面的旧单模型字段 */
    float obj_thresh = -1.0f;                /* <0 表示使用全局值 */
    float nms_thresh = -1.0f;                /* <0 表示使用全局值 */
    std::vector<std::string> detect_classes; /* 检测类别名称列表, 空=全部 */
    std::vector<RoiZoneConfig> roi_zones;     /* 多ROI区域(名称+归一化顶点), 空=无区域 */
    std::vector<std::pair<double, double>> roi_polygon; /* 单ROI区域(归一化顶点), 兼容旧配置 */
    /* 逻辑模块专有参数：由模块 logic.json Schema 统一定义和校验。
     * 配置文件键为 logic_parameters；新增普通逻辑参数不再扩展 ChannelConfig。 */
    std::string logic_parameters_json = "{}";
    int threads = -1;                        /* 单通道并发线程数, <0表示使用全局设置 */
    int playback_fps = -1;                   /* 播放/处理帧率上限，<0表示不限制(本地文件默认25) */
    int max_fps = -1;                        /* 推理帧率上限，<0表示继承全局设置 */
    int npu_core = -1;                       /* 指定NPU核心绑定(0,1,2)，<0表示自动分配 */

    /* 跟踪器 (全局默认, 可被通道覆盖) */
    int tracker_enable = -1;         /* -1=未指定(继承全局), 0=关闭, 1=开启 */
    float tracker_iou_thresh = 0.3f; /* IoU 匹配阈值 (0~1), 低于此值视为不匹配 */
    int tracker_max_miss = 10;       /* 连续丢失上限, 超限删除轨迹 */
    int tracker_min_hits = 3;        /* 确认轨迹所需的最小命中帧数 */

    /* logic_path_sop: 目标"路径/顺序/停留/合规"检测(单目标·按类别; 不含抖动/朝向) */
    std::string path_sequence = "";           /* 设计路径: 逗号分隔的区域名(须与本通道各 ROI 区域名完全一致), 顺序=期望经过顺序 */
    std::string path_target_label = "";       /* 要跟踪的目标类别名(取整帧该类置信度最高的一个) */
    float path_enter_sec = 0.5f;              /* 进入确认【默认】(秒): per-step 列表缺省项的回退值 */
    float path_dwell_min_sec = 0.0f;          /* 最小停留【默认】(秒): per-step 列表缺省项的回退值; 0=不要求 */
    float path_dwell_max_sec = 0.0f;          /* 最大停留【默认】(秒): per-step 列表缺省项的回退值; 0=不限(用户可忽略) */
    std::string path_enter_list = "";         /* 每步进入确认(秒), 逗号分隔, 与 path_sequence 对齐(空项回退默认); 由 SOP 编排画布生成 */
    std::string path_dwell_list = "";         /* 每步最小停留(秒), 逗号分隔, 与 path_sequence 对齐(空项回退默认); 由 SOP 编排画布生成 */
    std::string path_dwell_max_list = "";     /* 每步最大停留(秒), 逗号分隔, 与 path_sequence 对齐(空项回退默认; 0=不限); 由 SOP 编排画布生成 */
    std::string path_edges = "";              /* 图边列表(可空, 空=默认线性链 0→1→...→N-1); 形如 "0-1,0-3,1-2,3-2": 每条边 src-dst, 索引基于 path_sequence 位置。允许多分支(同源多出 / 多源汇合) / 环 */
    std::string path_entries = "";            /* 起点 step 索引(逗号分隔, 如 "0,2"): 被标记为「🚩 起点」的步骤。允许多起点(多路线)+ 同 zone 多起点(靠后续区域区分)。空 → fallback step 0 */
    std::string path_exits = "";              /* 出口 step 索引(逗号分隔, 如 "3,5"): 用户在 SOP 子画布上连到「🏁 结束判定」的 source step。漏检判定: visited 子图必须存在 entry→exit 路径。空 → fallback 到出度0(老 DAG 行为) */
    std::string path_edge_limits = "";        /* 边循环次数约束: "src-dst:min-max,..."(如 "1-0:2-5" = 1→0 边必须走 2~5 次)。settle 时判 count∈[min,max], 不在范围内 → 报"循环次数不符"。min/max 为 0 = 该侧不限 */
    float path_reset_sec = 5.0f;              /* 离场超时(秒): 目标离场持续此久 → 工序结束(漏检结算/复位); leave 模式为主判定, endzone 模式为兜底 */
    std::string path_end_mode = "leave";      /* 工序结束判定: "leave"=离场超时, "endzone"=进入终点区域, "trigger"=外部触发信号 */
    std::string path_end_zone = "";           /* 终点区域名(end_mode=endzone 时用) */
    float path_end_dwell_sec = 0.0f;          /* 终点连续停留达到此秒数才结束; 0=通过终点进入确认后立即结束(兼容旧配置) */
    float path_total_min_sec = 0.0f;          /* 工序总耗时下限(秒): 一轮总耗时 < 此值 → 报"总耗时不足" (赶工); 0=不限 */
    float path_total_max_sec = 0.0f;          /* 工序总耗时上限(秒): 一轮总耗时 > 此值 → 报"总耗时超时" (卡壳); 0=不限 */
    std::string path_trigger_mode = "auto";   /* 起点触发方式: "auto"=目标进入即开始; "external"=等待sop_trigger外部信号 */
    bool path_trigger_mandatory = false;       /* 仅 external 模式有效: 未触发而进入区域 → 报 sop_untracked_entry */
    bool path_report_normal = false;           /* 一轮正式结算且完全合规时是否上报 sop_normal；默认关闭以兼容旧配置 */
    /* 报警事件视频内部运行参数；由 report_policy 派生，供源帧预缓冲使用。 */
    bool event_video_enable = false;
    float event_video_pre_sec = 3.0f;
    float event_video_post_sec = 3.0f;
    int event_video_fps = 15;                 /* 环形缓冲采样/输出帧率 */
    std::string event_video_overlay = "custom"; /* none=原始源帧；custom/all=按实时播放规则独立渲染 */

    /* 通用告警配置：Web 直接保存对象/数组，C++ 以 JSON 文本解析，新增参数无需改结构体。 */
    std::string report_policy_json = "{}";
    std::string report_parameters_json = "{}";
};

/*======================== 全局逻辑配置 (支持多个并行实例) ========================*/
struct GlobalLogicConfig
{
    bool enable = false;                  /* 是否启用 */
    std::string logic = "global_default"; /* 逻辑名称 */
    std::vector<int> channels;            /* 监控的通道列表，空 = 全部 */
    int poll_interval_ms = 100;           /* 轮询间隔 (毫秒) */
};

/* 用于热重载时检测 global_logics 数组是否变化, 任一字段不同即视为变化 */
inline bool operator==(const GlobalLogicConfig &a, const GlobalLogicConfig &b)
{
    return a.enable == b.enable
        && a.logic == b.logic
        && a.channels == b.channels
        && a.poll_interval_ms == b.poll_interval_ms;
}
inline bool operator!=(const GlobalLogicConfig &a, const GlobalLogicConfig &b)
{
    return !(a == b);
}

/*======================== 全局配置 ========================*/
struct AppConfig
{
    /* 模型 (全局默认) */
    std::string model_type = "yolo";
    std::string model_path;
    std::string label_path;

    /* 显示 */
    bool enable_display = true;
    int disp_width = 1920;
    int disp_height = 1080;
    int tile_cols = 2;
    int tile_rows = 2;
    bool performance_display = true; /* 性能统计显示开关 */
    bool debug_display = false;       /* 调试信息打印开关 (JSON: debug_display: 1) */
    bool enable_pause_key = false;    /* 暂停键开关: true=按空格可暂停 (需同时开启 enable_display) */

    /* RTSP 推流 (无显示器时通过 VLC / 配置平台查看与显示屏一致的拼接画面) */
    bool        enable_rtsp  = false;   /* 是否启用内置 RTSP 服务 */
    int         rtsp_port    = 8554;    /* RTSP 端口, 地址 rtsp://<板IP>:<port><rtsp_path> */
    std::string rtsp_path    = "/live"; /* RTSP 挂载点 (须以 '/' 开头) */
    int         rtsp_fps     = 25;      /* 推流帧率 */
    int         rtsp_bitrate = 4096;    /* 软件编码码率(kbps); 硬件编码用默认码率 */
    std::string rtsp_codec   = "h264";  /* "h264" 或 "h265" */
    std::string rtsp_encoder = "auto";  /* "auto"=有硬件就硬编否则软编; "hw"=强制硬编 */

    /* 推理引擎 */
    int channel_threads = 1;                 /* 每个通道并发数默认值 */
    int max_fps = 30;                        /* 每通道推理帧率上限默认值 (从15提高到30) */
    int local_default_fps = 25;              /* 本地文件默认播放采样率 */
    int queue_size = 1;                      /* 每核任务队列深度 */
    float obj_thresh = 0.4f;                 /* 全局默认置信度阈值 */
    float nms_thresh = 0.45f;                /* 全局默认NMS阈值 */
    std::vector<std::string> detect_classes; /* 全局默认检测类别, 空=全部 */

    /* 跟踪器 (全局默认，可被通道覆盖) */
    int tracker_enable = 1; /* 0=关闭, 1=开启 */
    float tracker_iou_thresh = 0.3f;
    int tracker_max_miss = 10;
    int tracker_min_hits = 3;

    /* 通道列表 */
    std::vector<ChannelConfig> channels;

    /* 全局逻辑 (使用 global_logic.h 中定义的 GlobalLogicConfig, 支持多个并行实例) */
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
