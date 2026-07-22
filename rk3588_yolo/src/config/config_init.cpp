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
    REG_C("logic_parameters", JSON, logic_parameters_json);
    REG_C("threads", INT, threads);
    REG_C("playback_fps", INT, playback_fps);
    REG_C("max_fps", INT, max_fps);
    REG_C("npu_core", INT, npu_core);
    REG_C("tracker_enable", INT, tracker_enable);
    REG_C("tracker_iou_thresh", FLOAT, tracker_iou_thresh);
    REG_C("tracker_max_miss", INT, tracker_max_miss);
    REG_C("tracker_min_hits", INT, tracker_min_hits);

    // 自定义逻辑中的变量配置区域
    REG_C("path_sequence", STRING, path_sequence);
    REG_C("path_target_label", STRING, path_target_label);
    REG_C("path_enter_sec", FLOAT, path_enter_sec);
    REG_C("path_dwell_min_sec", FLOAT, path_dwell_min_sec);
    REG_C("path_enter_list", STRING, path_enter_list);
    REG_C("path_dwell_list", STRING, path_dwell_list);
    REG_C("path_dwell_max_sec", FLOAT, path_dwell_max_sec);
    REG_C("path_dwell_max_list", STRING, path_dwell_max_list);
    REG_C("path_reset_sec", FLOAT, path_reset_sec);
    REG_C("path_end_mode", STRING, path_end_mode);
    REG_C("path_end_zone", STRING, path_end_zone);
    REG_C("path_end_dwell_sec", FLOAT, path_end_dwell_sec);
    REG_C("path_total_min_sec", FLOAT, path_total_min_sec);
    REG_C("path_total_max_sec", FLOAT, path_total_max_sec);
    REG_C("path_edges", STRING, path_edges);
    REG_C("path_entries", STRING, path_entries);
    REG_C("path_exits", STRING, path_exits);
    REG_C("path_edge_limits", STRING, path_edge_limits);
    REG_C("path_trigger_mode", STRING, path_trigger_mode);
    REG_C("path_trigger_mandatory", BOOL, path_trigger_mandatory);
    REG_C("path_report_normal", BOOL, path_report_normal);
    // 由 report_policy 派生，供原始分辨率录像环形缓冲使用。
    REG_C("event_video_enable", BOOL, event_video_enable);
    REG_C("event_video_pre_sec", FLOAT, event_video_pre_sec);
    REG_C("event_video_post_sec", FLOAT, event_video_post_sec);
    REG_C("event_video_fps", INT, event_video_fps);
    REG_C("event_video_overlay", STRING, event_video_overlay);
    REG_C("report_policy", JSON, report_policy_json);
    REG_C("report_parameters", JSON, report_parameters_json);
}
