#pragma once

#include "runtime/constants.h"
#include <config_types.h>
#include <memory>

/*======================== 全局配置 ========================*/
struct AppConfig
{
    /* 显示 */
    bool enable_display = true;
    int disp_width = 1920;
    int disp_height = 1080;
    int tile_cols = 2;
    int tile_rows = 2;
    bool performance_display = true; /* 性能统计显示与调试信息打印开关 */
    bool enable_pause_key = false;   /* 暂停键开关: true=按空格可暂停 (需同时开启 enable_display) */

    /* RTSP 推流 (无显示器时通过 VLC / 配置平台查看与显示屏一致的拼接画面) */
    bool enable_rtsp = false;          /* 是否启用内置 RTSP 服务 */
    int rtsp_port = 8554;              /* RTSP 端口, 地址 rtsp://<板IP>:<port><rtsp_path> */
    std::string rtsp_path = "/live";   /* RTSP 挂载点 (须以 '/' 开头) */
    int rtsp_bitrate = 4096;           /* 软件编码码率(kbps); 硬件编码用默认码率 */
    std::string rtsp_codec = "h264";   /* "h264" 或 "h265" */
    std::string rtsp_encoder = "auto"; /* "auto"=有硬件就硬编否则软编; "hw"=强制硬编 */

    /* 推理引擎 */
    int channel_threads = 1; /* 每个通道并发数默认值 */
    int max_fps = 30;        /* 每通道推理/业务处理帧率上限默认值 */
    int queue_size = 1;      /* 每核任务队列深度 */
    /* 跟踪器 (全局默认，可被通道覆盖) */
    int tracker_enable = 1; /* 0=关闭, 1=开启 */
    std::string tracker_type = "sort";
    float tracker_iou_thresh = 0.3f;
    int tracker_max_miss = 10;
    int tracker_min_hits = 3;
    float bytetrack_low_thresh = 0.1f;
    float bytetrack_low_iou_thresh = 0.2f;

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
bool is_bytetrack_enabled(const ChannelConfig &ch_cfg);
float effective_model_obj_thresh(const ChannelConfig &ch_cfg, const ChannelModelConfig &model);
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
