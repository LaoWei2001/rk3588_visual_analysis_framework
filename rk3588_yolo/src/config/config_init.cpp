/**
 * @file config_init.cpp
 * @brief 配置字段注册
 */
#include "config_registry.h"
#include "config.h"

void init_config_fields(AppConfig &cfg)
{
    (void)cfg;

    // 全局配置
    REG_G("model_type", STRING, model_type);
    REG_G("model_path", STRING, model_path);
    REG_G("label_path", STRING, label_path);
    REG_G("enable_display", BOOL, enable_display);
    REG_G("disp_width", INT, disp_width);
    REG_G("disp_height", INT, disp_height);
    REG_G("tile_cols", INT, tile_cols);
    REG_G("tile_rows", INT, tile_rows);
    REG_G("performance_display", BOOL, performance_display);
    REG_G("debug_display", BOOL, debug_display);
    REG_G("enable_pause_key", BOOL, enable_pause_key);

    // RTSP 推流
    REG_G("enable_rtsp", BOOL, enable_rtsp);
    REG_G("rtsp_port", INT, rtsp_port);
    REG_G("rtsp_path", STRING, rtsp_path);
    REG_G("rtsp_fps", INT, rtsp_fps);
    REG_G("rtsp_bitrate", INT, rtsp_bitrate);
    REG_G("rtsp_codec", STRING, rtsp_codec);
    REG_G("rtsp_encoder", STRING, rtsp_encoder);
    REG_G("channel_threads", INT, channel_threads);
    REG_G("max_fps", INT, max_fps);
    REG_G("local_default_fps", INT, local_default_fps);
    REG_G("queue_size", INT, queue_size);
    REG_G("npu_cores", INT, npu_cores);
    REG_G("obj_thresh", FLOAT, obj_thresh);
    REG_G("nms_thresh", FLOAT, nms_thresh);
    REG_G("detect_classes", STRING_ARRAY, detect_classes);
    REG_G("tracker_enable", INT, tracker_enable);
    REG_G("tracker_iou_thresh", FLOAT, tracker_iou_thresh);
    REG_G("tracker_max_miss", INT, tracker_max_miss);
    REG_G("tracker_min_hits", INT, tracker_min_hits);

    // 通道配置
    REG_C("id", INT, id);
    REG_C("enable", BOOL, enable);
    REG_C("infer_enable", BOOL, infer_enable);
    REG_C("swap_rb", BOOL, swap_rb);
    REG_C("logic", STRING, logic);
    REG_C("model_type", STRING, model_type);
    REG_C("model_path", STRING, model_path);
    REG_C("label_path", STRING, label_path);
    REG_C("obj_thresh", FLOAT, obj_thresh);
    REG_C("nms_thresh", FLOAT, nms_thresh);
    REG_C("detect_classes", STRING_ARRAY, detect_classes);
    REG_C("threads", INT, threads);
    REG_C("playback_fps", INT, playback_fps);
    REG_C("max_fps", INT, max_fps);
    REG_C("npu_core", INT, npu_core);
    REG_C("tracker_enable", INT, tracker_enable);
    REG_C("tracker_iou_thresh", FLOAT, tracker_iou_thresh);
    REG_C("tracker_max_miss", INT, tracker_max_miss);
    REG_C("tracker_min_hits", INT, tracker_min_hits);

    // 自定义逻辑中的变量配置区域
    REG_C("radius", INT, radius);
    REG_C("report_interval_sec", INT, report_interval_sec);
    REG_C("linger_sec", INT, linger_sec);
    REG_C("line_width", INT, line_width);
    REG_C("t_start", FLOAT, t_start);
    REG_C("t_end", FLOAT, t_end);
    REG_C("coverage_threshold", FLOAT, coverage_threshold);
    REG_C("required_actions", STRING, required_actions);
    REG_C("sop_sequence", STRING, sop_sequence);
    REG_C("basket_normal_label", STRING, basket_normal_label);
    REG_C("basket_abnormal_label", STRING, basket_abnormal_label);
    REG_C("sop_shake_amplitude", INT, sop_shake_amplitude);
    REG_C("sop_shake_min_count", INT, sop_shake_min_count);
    REG_C("sop_enter_sec", FLOAT, sop_enter_sec);
    REG_C("sop_dir_sec", FLOAT, sop_dir_sec);
    REG_C("fall_ratio_thresh", FLOAT, fall_ratio_thresh);
    REG_C("fall_dwell_sec", FLOAT, fall_dwell_sec);
    REG_C("fall_cooldown_sec", INT, fall_cooldown_sec);
    REG_C("wave_min_swings", INT, wave_min_swings);
    REG_C("wave_window_sec", FLOAT, wave_window_sec);
    REG_C("dify_prompt", STRING, dify_prompt);

    // 上报地址 (方案2: 每通道独立, 空 = 用上报服务默认值)
    REG_C("server_url", STRING, server_url);
    REG_C("dify_api_url", STRING, dify_api_url);
    REG_C("dify_api_key", STRING, dify_api_key);
}
