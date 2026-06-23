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
    std::string src_type; /* 必填:
                             "rtsp"/"file"/"usb"（前后端均不再自动推断，缺省=配置错误） */
    std::string url;
    std::string device;    /* USB设备节点, 例如 "/dev/video0" */
    std::string video_enc; /* "h264" 或 "h265" */
    bool loop = false;     /* 文件播放循环（仅 src_type=file 有效） */
    int usb_width = 0;     /* USB 显式采集分辨率(0=随 fps 自动档)。与 ROI
                              抓帧一致、不随 fps 变 → 三者坐标统一 */
    int usb_height = 0;
};

/*======================== ROI 区域配置 (从 config.json 通道字段加载)
 * ========================*/
struct RoiZoneConfig
{
    std::string name;
    std::vector<std::pair<double, double>> polygon;
};

/*======================== 针对通道的配置(被下面的全局配置AppConfig包含)
 * ========================*/
struct ChannelConfig
{
    int id = -1;
    bool enable = true;
    bool infer_enable = true; /* 是否启用 YOLO 推理。false=传统算法通道:仍解码/显示/逐帧跑 logic,
                                 但不进 NPU, ctx->results 为空、ctx->infer_enabled=0 */
    bool swap_rb = false;     /* 仅显示: 1=该通道画面 R/B 互换显示(跳过显示前
                                 BGR→RGB);不影响推理/上报 */
    StreamConfig stream;
    std::string logic = "logic_default";     /* 自定义逻辑名称 (须为已注册的 logic_xxx) */
    std::string model_type = "";             /* 模型类型: "yolov5"/"yolov8_det"/"yolov8_pose"/"yolov5_seg" */
    std::string model_path = "";             /* 模型路径，为空表示该通道不做YOLO推理 */
    std::string label_path = "";             /* 标签路径（检测/分割模型需要） */
    float obj_thresh = -1.0f;                /* <0 表示使用全局值 */
    float nms_thresh = -1.0f;                /* <0 表示使用全局值 */
    std::vector<std::string> detect_classes; /* 检测类别名称列表, 空=全部 */
    std::vector<RoiZoneConfig> roi_zones;    /* 多ROI区域(名称+归一化顶点), 空=无区域 */
    std::vector<std::pair<double, double>> roi_polygon; /* 单ROI区域(归一化顶点), 兼容旧配置 */
    int threads = -1;                                   /* 单通道并发线程数, <0表示使用全局设置 */
    int playback_fps = -1; /* 播放/处理帧率上限，<0表示不限制(本地文件默认25) */
    int max_fps = -1;      /* 推理帧率上限，<0表示继承全局设置 */
    int npu_core = -1;     /* 指定NPU核心绑定(0,1,2)，<0表示自动分配 */

    /* 跟踪器 (全局默认, 可被通道覆盖) */
    int tracker_enable = -1;         /* -1=未指定(继承全局), 0=关闭, 1=开启 */
    float tracker_iou_thresh = 0.3f; /* IoU 匹配阈值 (0~1), 低于此值视为不匹配 */
    int tracker_max_miss = 10;       /* 连续丢失上限, 超限删除轨迹 */
    int tracker_min_hits = 3;        /* 确认轨迹所需的最小命中帧数 */

    /* 自定义通道逻辑中的变量 */
    int radius = 1;
    int report_interval_sec = 5; /* logic_server / logic_dify 上报间隔(秒): 两次上报间的最小冷却 */
    int line_width = 20;         /* logic_wafer: 擦拭轨迹拓宽的线条宽度(像素), 模拟扫过的面积带 */
    int linger_sec = 10;
    float t_start = 1.0f;             /* logic_wafer: 工序开始确认时长(秒) - cleanwiper 需在 wafer
                                         框内持续此久才开始; 滤手短暂进入/反光误检, 期间不画不计 */
    float t_end = 1.0f;               /* logic_wafer: 工序结束确认时长(秒) - cleanwiper
                                         离开区域/消失持续此久即判定工序结束并结算 */
    float coverage_threshold = 80.0f; /* logic_wafer: 擦拭覆盖率合格阈值(百分比) */
    std::string required_actions = ""; /* logic_wafer: 必做动作列表(逗号分隔, 横擦/纵擦/圆弧擦 或 h/v/arc;
                                          空=跳过动作完整性校验) */

    /* logic_wafer_sop: 晶圆湿法清洗 SOP 合规检测(花篮转移顺序/朝向/纯水槽抖动) */
    std::string sop_sequence = "去胶槽1,去胶槽2,IPA槽,纯水槽"; /* 进槽顺序(逗号分隔); 名字须与本通道各
                                                                  ROI 区域名完全一致 */
    std::string basket_normal_label = "bkt_normal";     /* 花篮"正常朝向"类别名(须与 labels.txt 一致) */
    std::string basket_abnormal_label = "bkt_abnormal"; /* 花篮"翻转朝向"类别名; 出现即判定方向偏转 */
    int sop_shake_amplitude = 15; /* 纯水槽抖动: 一次有效上/下摆动的最小幅度(模型坐标 px) */
    int sop_shake_min_count = 3;  /* 纯水槽抖动: 工序内所需的最小有效抖动次数, 不足则告警 */
    float sop_enter_sec = 0.8f; /* 进入区域确认时长(秒): 花篮须在某槽内持续此久才算"真正进入",
                                   滤除转移时短暂划过 */
    float sop_dir_sec = 0.5f;   /* 朝向切换确认时长(秒):
                                   翻转/恢复类别须持续此久才判定为"真正变向" */

    /* logic_path_sop: 目标"路径/顺序/停留/合规"检测(单目标·按类别; 不含抖动/朝向)
     */
    std::string path_sequence = "";     /* 设计路径: 逗号分隔的区域名(须与本通道各 ROI
                                           区域名完全一致), 顺序=期望经过顺序 */
    std::string path_target_label = ""; /* 要跟踪的目标类别名(取整帧该类置信度最高的一个) */
    float path_enter_sec = 0.5f;        /* 进入确认【默认】(秒): per-step 列表缺省项的回退值 */
    float path_dwell_min_sec = 0.0f; /* 最小停留【默认】(秒): per-step 列表缺省项的回退值; 0=不要求 */
    std::string path_enter_list = ""; /* 每步进入确认(秒), 逗号分隔, 与 path_sequence 对齐(空项回退默认); 由
                                         SOP 编排画布生成 */
    std::string path_dwell_list = ""; /* 每步最小停留(秒), 逗号分隔, 与 path_sequence 对齐(空项回退默认); 由
                                         SOP 编排画布生成 */
    float path_reset_sec = 5.0f; /* 离场超时(秒): 目标离场持续此久 → 工序结束(漏检结算/复位); leave
                                    模式为主判定, endzone 模式为兜底 */
    std::string path_end_mode = "leave"; /* 工序结束判定: "leave"=离场超时, "endzone"=进入终点区域 */
    std::string path_end_zone = "";      /* 终点区域名(end_mode=endzone 时用) */
    bool path_report = false;            /* 是否上报 SOP 报警(顺序错误/漏检);
                                            由画布是否连"上报配置"节点决定 */

    /* logic_fall_detect: 人员跌倒检测 */
    float fall_ratio_thresh = 1.25f; /* 人员框宽高比阈值: width / height 超过该值时判为横躺嫌疑 */
    float fall_dwell_sec = 2.0f;     /* 跌倒嫌疑须持续多久才报警 */
    int fall_cooldown_sec = 10;      /* 两次跌倒报警之间的最小间隔 */
    int wave_min_swings = 2;         /* 挥手求救: 短时间内左右摆动达到该次数才报警 */
    float wave_window_sec = 2.0f;    /* 挥手求救: 统计左右摆动的时间窗口 */

    /* logic_dify 专用: 发送给 Dify 的提示词, 空则使用默认值 */
    std::string dify_prompt = "";

    /* 上报总开关: 画布上连了"上报配置"节点才置 true。各上报类
       logic(server/dify/hook/fall 及自定义逻辑) 把 report_enabled(ctx) 作为
       *_uploader_enqueue 的 report_enable 参数传入。 缺省 false ——
       没连节点(或老配置未写此字段)即不上报,
       让"上报配置"节点成为真正的开关而非摆设。 */
    bool report_enable = false;

    /* 上报地址 (方案2: 每通道独立可填; 空 = 用 Python 上报服务 config.yaml
     * 的默认值) */
    std::string server_url = "";   /* logic_server 专用: HTTP 上报地址 */
    std::string dify_api_url = ""; /* logic_dify   专用: Dify 服务地址 */
    std::string dify_api_key = ""; /* logic_dify   专用: Dify 应用 API Key */
};

/*======================== 全局逻辑配置 (支持多个并行实例)
 * ========================*/
struct GlobalLogicConfig
{
    bool enable = false;                  /* 是否启用 */
    std::string logic = "global_example"; /* 逻辑名称 */
    std::vector<int> channels;            /* 监控的通道列表，空 = 全部 */
    int poll_interval_ms = 100;           /* 轮询间隔 (毫秒) */
};

/* 用于热重载时检测 global_logics 数组是否变化, 任一字段不同即视为变化 */
inline bool operator==(const GlobalLogicConfig &a, const GlobalLogicConfig &b)
{
    return a.enable == b.enable && a.logic == b.logic && a.channels == b.channels &&
           a.poll_interval_ms == b.poll_interval_ms;
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
    bool debug_display = false;      /* 调试信息打印开关 (JSON: debug_display: 1) */
    bool enable_pause_key = false;   /* 暂停键开关: true=按空格可暂停 (需同时开启 enable_display) */

    /* RTSP 推流 (无显示器时通过 VLC / 配置平台查看与显示屏一致的拼接画面) */
    bool enable_rtsp = false;          /* 是否启用内置 RTSP 服务 */
    int rtsp_port = 8554;              /* RTSP 端口, 地址 rtsp://<板IP>:<port><rtsp_path> */
    std::string rtsp_path = "/live";   /* RTSP 挂载点 (须以 '/' 开头) */
    int rtsp_fps = 25;                 /* 推流帧率 */
    int rtsp_bitrate = 4096;           /* 软件编码码率(kbps); 硬件编码用默认码率 */
    std::string rtsp_codec = "h264";   /* "h264" 或 "h265" */
    std::string rtsp_encoder = "auto"; /* "auto"=有硬件就硬编否则软编; "hw"=强制硬编; "sw"=强制软编 */

    /* 推理引擎 */
    int channel_threads = 1;                 /* 每个通道并发数默认值 */
    int max_fps = 30;                        /* 每通道推理帧率上限默认值 (从15提高到30) */
    int local_default_fps = 25;              /* 本地文件默认播放采样率 */
    int queue_size = 1;                      /* 每核任务队列深度 */
    int npu_cores = 3;                       /* RKNN上下文数 (1-3) */
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

    /* 全局逻辑 (使用 global_logic.h 中定义的 GlobalLogicConfig, 支持多个并行实例)
     */
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
