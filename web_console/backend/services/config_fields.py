"""Startup configuration field contract, checked before Web writes.

Keep structural fields aligned with application/config/config.cpp and
config_init.cpp (covered by the contract test). Module parameter values remain dictionaries owned by their schemas.
"""

GLOBAL = set("""enable_display disp_width disp_height tile_cols tile_rows
performance_display enable_rtsp rtsp_port rtsp_path rtsp_bitrate
channel_threads max_fps queue_size tracker_enable
tracker_type tracker_iou_thresh tracker_max_miss tracker_min_hits
bytetrack_low_thresh bytetrack_low_iou_thresh global_logics""".split())
CHANNEL = set("""id enable logic logic_parameters threads max_fps
tracker_enable tracker_type tracker_iou_thresh tracker_max_miss tracker_min_hits
bytetrack_low_thresh bytetrack_low_iou_thresh report_policy
stream models inference_roi roi_zones""".split())
GLOBAL_LOGIC = set("""instance_id enable logic channels poll_interval_ms
logic_parameters report_policy media_source_channel_id""".split())
STREAM = set("src_type url device video_enc loop usb_width usb_height".split())
MODEL = set("""id enable model_type model_path label_path version obj_thresh
nms_thresh detect_classes npu_core""".split())
ROI = {"mode", "resize_mode", "polygon"}
ZONE = {"name", "polygon"}
POLICY = set("""enabled deliveries image_overlay image_selection video_overlay
video_pre_sec video_post_sec video_fps merge_window_sec""".split())
DELIVERY = set("id enabled connection_id contract_id contract_label contract_revision media when".split())


def _object(value, allowed, path):
    if not isinstance(value, dict):
        raise ValueError(f"{path} 必须为对象")
    unknown = value.keys() - allowed
    if unknown:
        fields = ", ".join(f"{path}.{key}" for key in sorted(unknown))
        raise ValueError(f"配置包含不支持的字段：{fields}")


def _items(value, path):
    if not isinstance(value, list):
        raise ValueError(f"{path} 必须为数组")
    return ((item, f"{path}[{index}]") for index, item in enumerate(value))


def _report(owner, path):
    if "logic_parameters" in owner and not isinstance(owner["logic_parameters"], dict):
        raise ValueError(f"{path}.logic_parameters 必须为对象")
    if "report_policy" not in owner:
        return
    policy = owner["report_policy"]
    path += ".report_policy"
    _object(policy, POLICY, path)
    if "image_selection" in policy:
        _object(policy["image_selection"], {"mode", "channel_ids"}, path + ".image_selection")
    for delivery, where in _items(policy.get("deliveries", []), path + ".deliveries"):
        _object(delivery, DELIVERY, where)
        if "when" in delivery:
            _object(delivery["when"], {"event_types"}, where + ".when")


def validate_config_fields(config):
    """Reject unknown keys without mutating the submitted configuration."""
    _object(config, {"global", "channels", "_editor_layout"}, "root")
    global_config = config.get("global", {})
    _object(global_config, GLOBAL, "global")
    for entry, path in _items(global_config.get("global_logics", []), "global.global_logics"):
        _object(entry, GLOBAL_LOGIC, path)
        _report(entry, path)
    for channel, path in _items(config.get("channels", []), "channels"):
        _object(channel, CHANNEL, path)
        _report(channel, path)
        for key, fields in (("stream", STREAM), ("inference_roi", ROI)):
            if key in channel:
                _object(channel[key], fields, path + "." + key)
        for key, fields in (("models", MODEL), ("roi_zones", ZONE)):
            for entry, where in _items(channel.get(key, []), path + "." + key):
                _object(entry, fields, where)
