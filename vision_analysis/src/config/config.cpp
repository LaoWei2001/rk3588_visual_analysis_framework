/**
 * @file config.cpp
 * @brief JSON 配置解析与热加载
 */
#include "config.h"
#include "third_party/json/cJSON.h"
#include "config_registry.h"
#include "config_validator.h"
#include "logic/core/logic_parameters.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

void init_config_fields(AppConfig &cfg);

namespace config_utils
{
bool starts_with(const std::string &value, const char *prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string to_lower_copy(const std::string &value)
{
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string normalize_src_type(const StreamConfig &stream)
{
    /* src_type 现为必填字段：仅做大小写归一，不再根据 url/device 自动推断。
     * 返回空字符串表示用户未指定 → 由 load_config 视为配置错误并拒绝。 */
    return to_lower_copy(stream.src_type);
}

std::string resolve_stream_location(const StreamConfig &stream, const std::string &src_type)
{
    if (src_type == "usb" && !stream.device.empty())
        return stream.device;
    return stream.url;
}

bool is_supported_src_type(const std::string &src_type)
{
    return src_type == "rtsp" || src_type == "file" || src_type == "usb";
}

bool is_channel_infer_enabled(const ChannelConfig &ch_cfg)
{
    /* 推理开启需同时满足：用户开关 infer_enable=true 且配置了模型(路径+类型)。
     * infer_enable=false → 跳过 NPU 推理但仍解码/显示；若配置了后处理则以空结果逐帧调用。 */
    if (!ch_cfg.infer_enable)
        return false;
    return std::any_of(ch_cfg.models.begin(), ch_cfg.models.end(), [](const ChannelModelConfig &model) {
        return model.enable && !model.model_path.empty() && !model.model_type.empty();
    });
}
} // namespace config_utils

namespace
{
EventVideoRuntimeConfig event_video_from_report_policy(cJSON *policy)
{
    EventVideoRuntimeConfig runtime;
    if (!cJSON_IsObject(policy))
        return runtime;

    cJSON *value = cJSON_GetObjectItemCaseSensitive(policy, "video_pre_sec");
    if (cJSON_IsNumber(value))
        runtime.pre_sec = std::max(0.0f, static_cast<float>(value->valuedouble));

    value = cJSON_GetObjectItemCaseSensitive(policy, "video_post_sec");
    if (cJSON_IsNumber(value))
        runtime.post_sec = std::max(0.0f, static_cast<float>(value->valuedouble));

    value = cJSON_GetObjectItemCaseSensitive(policy, "video_fps");
    if (cJSON_IsNumber(value))
        runtime.fps = std::max(1, std::min(30, value->valueint));

    value = cJSON_GetObjectItemCaseSensitive(policy, "video_overlay");
    if (cJSON_IsString(value) && value->valuestring)
    {
        const std::string overlay = value->valuestring;
        if (overlay == "none" || overlay == "custom" || overlay == "all")
            runtime.overlay = overlay;
    }

    cJSON *policy_enabled = cJSON_GetObjectItemCaseSensitive(policy, "enabled");
    if (cJSON_IsFalse(policy_enabled))
        return runtime;

    cJSON *deliveries = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    cJSON *delivery = nullptr;
    cJSON_ArrayForEach(delivery, deliveries)
    {
        if (!cJSON_IsObject(delivery))
            continue;
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(delivery, "enabled");
        cJSON *media = cJSON_GetObjectItemCaseSensitive(delivery, "media");
        bool needs_video = false;
        cJSON *kind = nullptr;
        cJSON_ArrayForEach(kind, media)
            needs_video = needs_video ||
                          (cJSON_IsString(kind) && kind->valuestring &&
                           std::string(kind->valuestring) == "video");
        if (!cJSON_IsFalse(enabled) && cJSON_IsArray(media) && needs_video)
        {
            runtime.enable = true;
            break;
        }
    }
    return runtime;
}
} // namespace

bool load_config(const std::string &path, AppConfig &cfg)
{
    // 保存热重载标记（在 init_config_fields 之前，因为后面 cfg.config_path 会被覆盖）
    bool is_hotreload = (cfg.config_path == "HOTRELOAD");

    static bool initialized = false;
    if (!initialized)
    {
        init_config_fields(cfg);
        initialized = true;
    }

    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        fprintf(stderr, "[Config] cannot open: %s\n", path.c_str());
        return false;
    }
    std::stringstream buffer;
    buffer << ifs.rdbuf();
    std::string json_text = buffer.str();

    cJSON *root = cJSON_Parse(json_text.c_str());
    if (!root)
    {
        fprintf(stderr, "[Config] JSON parse failed: %s\n", path.c_str());
        return false;
    }

    cfg.config_path = path;
    cfg.channels.clear();

    // 设置默认值
    cfg.enable_display = true;
    cfg.disp_width = 1920;
    cfg.disp_height = 1080;
    cfg.tile_cols = 2;
    cfg.tile_rows = 2;
    cfg.channel_threads = 1;
    cfg.max_fps = 30;
    cfg.local_default_fps = 25;
    cfg.queue_size = 1;
    cfg.tracker_enable = 1;
    cfg.tracker_iou_thresh = 0.3f;
    cfg.tracker_max_miss = 10;
    cfg.tracker_min_hits = 3;

    cJSON *global = cJSON_GetObjectItemCaseSensitive(root, "global");
    if (!cJSON_IsObject(global))
    {
        fprintf(stderr, "[Config] missing global\n");
        cJSON_Delete(root);
        return false;
    }

    static const char *removed_global_model_fields[] = {"model_type", "model_path", "label_path",
                                                        "obj_thresh", "nms_thresh", "detect_classes"};
    for (const char *field : removed_global_model_fields)
    {
        if (cJSON_GetObjectItemCaseSensitive(global, field))
        {
            fprintf(stderr, "[Config] global field '%s' is not allowed; configure it in channels[].models[]\n", field);
            cJSON_Delete(root);
            return false;
        }
    }

    if (!g_cfg_reg.parse_global(global, &cfg))
    {
        fprintf(stderr, "[Config] parse global failed\n");
        cJSON_Delete(root);
        return false;
    }

    /* 解析 global_logics 数组 (可选, 缺省为空列表) */
    cfg.global_logics.clear();

    cJSON *gl_array = cJSON_GetObjectItemCaseSensitive(global, "global_logics");
    if (cJSON_IsArray(gl_array))
    {
        std::vector<std::string> global_instance_ids;
        cJSON *gl_item = nullptr;
        cJSON_ArrayForEach(gl_item, gl_array)
        {
            if (!cJSON_IsObject(gl_item))
                continue;

            GlobalLogicConfig gl_cfg;
            gl_cfg.enable = false;
            gl_cfg.logic = "global_default";
            gl_cfg.poll_interval_ms = 100;

            cJSON *gl_instance_id = cJSON_GetObjectItemCaseSensitive(gl_item, "instance_id");
            if (!cJSON_IsString(gl_instance_id) || !gl_instance_id->valuestring || !gl_instance_id->valuestring[0])
            {
                fprintf(stderr, "[Config] global_logic[%zu].instance_id must be a non-empty string\n",
                        cfg.global_logics.size());
                cJSON_Delete(root);
                return false;
            }
            gl_cfg.instance_id = gl_instance_id->valuestring;
            if (std::find(global_instance_ids.begin(), global_instance_ids.end(), gl_cfg.instance_id) !=
                global_instance_ids.end())
            {
                fprintf(stderr, "[Config] duplicate global logic instance_id: %s\n", gl_cfg.instance_id.c_str());
                cJSON_Delete(root);
                return false;
            }
            global_instance_ids.push_back(gl_cfg.instance_id);

            cJSON *gl_enable = cJSON_GetObjectItemCaseSensitive(gl_item, "enable");
            if (cJSON_IsBool(gl_enable))
                gl_cfg.enable = cJSON_IsTrue(gl_enable);

            cJSON *gl_logic = cJSON_GetObjectItemCaseSensitive(gl_item, "logic");
            if (cJSON_IsString(gl_logic) && gl_logic->valuestring)
                gl_cfg.logic = gl_logic->valuestring;

            cJSON *gl_interval = cJSON_GetObjectItemCaseSensitive(gl_item, "poll_interval_ms");
            if (cJSON_IsNumber(gl_interval))
                gl_cfg.poll_interval_ms = std::max(10, gl_interval->valueint);

            cJSON *gl_channels = cJSON_GetObjectItemCaseSensitive(gl_item, "channels");
            if (cJSON_IsArray(gl_channels))
            {
                cJSON *ch_item = nullptr;
                cJSON_ArrayForEach(ch_item, gl_channels)
                {
                    if (cJSON_IsNumber(ch_item))
                        gl_cfg.channels.push_back(ch_item->valueint);
                }
            }

            cJSON *gl_parameters = cJSON_GetObjectItemCaseSensitive(gl_item, "logic_parameters");
            if (gl_parameters)
            {
                if (!cJSON_IsObject(gl_parameters))
                {
                    fprintf(stderr, "[Config] global_logic[%zu].logic_parameters must be an object\n",
                            cfg.global_logics.size());
                    cJSON_Delete(root);
                    return false;
                }
                char *parameters_text = cJSON_PrintUnformatted(gl_parameters);
                if (!parameters_text)
                {
                    fprintf(stderr, "[Config] cannot serialize global_logic[%zu].logic_parameters\n",
                            cfg.global_logics.size());
                    cJSON_Delete(root);
                    return false;
                }
                gl_cfg.logic_parameters_json = parameters_text;
                cJSON_free(parameters_text);
            }

            cJSON *gl_report_policy = cJSON_GetObjectItemCaseSensitive(gl_item, "report_policy");
            if (gl_report_policy)
            {
                if (!cJSON_IsObject(gl_report_policy))
                {
                    fprintf(stderr, "[Config] global_logic[%zu].report_policy must be an object\n",
                            cfg.global_logics.size());
                    cJSON_Delete(root);
                    return false;
                }
                char *policy_text = cJSON_PrintUnformatted(gl_report_policy);
                if (!policy_text)
                {
                    cJSON_Delete(root);
                    return false;
                }
                gl_cfg.report_policy_json = policy_text;
                cJSON_free(policy_text);
                gl_cfg.event_video = event_video_from_report_policy(gl_report_policy);
            }

            cJSON *gl_report_parameters = cJSON_GetObjectItemCaseSensitive(gl_item, "report_parameters");
            if (gl_report_parameters)
            {
                if (!cJSON_IsObject(gl_report_parameters))
                {
                    fprintf(stderr, "[Config] global_logic[%zu].report_parameters must be an object\n",
                            cfg.global_logics.size());
                    cJSON_Delete(root);
                    return false;
                }
                char *parameters_text = cJSON_PrintUnformatted(gl_report_parameters);
                if (!parameters_text)
                {
                    cJSON_Delete(root);
                    return false;
                }
                gl_cfg.report_parameters_json = parameters_text;
                cJSON_free(parameters_text);
            }

            cJSON *gl_media_channel = cJSON_GetObjectItemCaseSensitive(gl_item, "media_source_channel_id");
            if (cJSON_IsNumber(gl_media_channel))
                gl_cfg.media_source_channel_id = gl_media_channel->valueint;

            {
                std::vector<LogicParameterError> parameter_errors;
                std::string normalized_parameters;
                if (!logic_parameters_resolve(gl_cfg.logic, gl_cfg.logic_parameters_json, &normalized_parameters,
                                              nullptr, &parameter_errors))
                {
                    fprintf(stderr, "[Config] global_logic[%zu] logic_parameters validation failed:\n",
                            cfg.global_logics.size());
                    for (const auto &error : parameter_errors)
                        fprintf(stderr, "  - %s: %s\n", error.field.c_str(), error.message.c_str());
                    cJSON_Delete(root);
                    return false;
                }
                gl_cfg.logic_parameters_json = std::move(normalized_parameters);
            }

            cfg.global_logics.push_back(gl_cfg);

            if (gl_cfg.enable)
            {
                printf("[Config] global_logic[%zu] enabled: id=%s logic=%s poll=%dms connected=%zu\n",
                       cfg.global_logics.size() - 1, gl_cfg.instance_id.c_str(), gl_cfg.logic.c_str(),
                       gl_cfg.poll_interval_ms, gl_cfg.channels.size());
            }
        }
    }

    if (is_hotreload)
    {
        printf("[Config] Hotreload mode: loading with relaxed model defaults\n");
    }

    cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (!cJSON_IsArray(channels))
    {
        fprintf(stderr, "[Config] missing channels\n");
        cJSON_Delete(root);
        return false;
    }

    int seq_idx = 0;
    std::set<int> used_ids;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, channels)
    {
        ChannelConfig ch;
        ch.id = seq_idx;
        ch.enable = true;
        ch.stream.video_enc = "h264";
        ch.threads = -1;
        ch.playback_fps = -1;
        ch.max_fps = -1;
        ch.tracker_enable = -1;

        // 解析stream对象
        cJSON *stream_obj = cJSON_GetObjectItemCaseSensitive(item, "stream");
        if (stream_obj && cJSON_IsObject(stream_obj))
        {
            cJSON *src_type = cJSON_GetObjectItemCaseSensitive(stream_obj, "src_type");
            if (cJSON_IsString(src_type) && src_type->valuestring)
                ch.stream.src_type = src_type->valuestring;
            cJSON *url = cJSON_GetObjectItemCaseSensitive(stream_obj, "url");
            if (cJSON_IsString(url) && url->valuestring)
                ch.stream.url = url->valuestring;
            cJSON *device = cJSON_GetObjectItemCaseSensitive(stream_obj, "device");
            if (cJSON_IsString(device) && device->valuestring)
                ch.stream.device = device->valuestring;
            cJSON *enc = cJSON_GetObjectItemCaseSensitive(stream_obj, "video_enc");
            if (cJSON_IsString(enc) && enc->valuestring)
                ch.stream.video_enc = enc->valuestring;
            cJSON *loop = cJSON_GetObjectItemCaseSensitive(stream_obj, "loop");
            if (cJSON_IsBool(loop))
                ch.stream.loop = cJSON_IsTrue(loop);
            cJSON *uw = cJSON_GetObjectItemCaseSensitive(stream_obj, "usb_width");
            if (cJSON_IsNumber(uw))
                ch.stream.usb_width = uw->valueint;
            cJSON *uh = cJSON_GetObjectItemCaseSensitive(stream_obj, "usb_height");
            if (cJSON_IsNumber(uh))
                ch.stream.usb_height = uh->valueint;
        }

        /* 解析唯一 ROI 配置入口 roi_zones（归一化坐标 0~1）。 */
        ch.roi_zones.clear();
        {
            cJSON *rz = cJSON_GetObjectItemCaseSensitive(item, "roi_zones");
            if (rz && !cJSON_IsArray(rz))
            {
                fprintf(stderr, "[Config] channel %d roi_zones must be an array\n", ch.id);
                cJSON_Delete(root);
                return false;
            }
            if (cJSON_IsArray(rz))
            {
                cJSON *zone = nullptr;
                cJSON_ArrayForEach(zone, rz)
                {
                    if (!cJSON_IsObject(zone))
                        continue;
                    RoiZoneConfig zc;
                    cJSON *nm = cJSON_GetObjectItemCaseSensitive(zone, "name");
                    if (cJSON_IsString(nm) && nm->valuestring)
                        zc.name = nm->valuestring;
                    cJSON *poly = cJSON_GetObjectItemCaseSensitive(zone, "polygon");
                    if (cJSON_IsArray(poly))
                    {
                        cJSON *pt = nullptr;
                        cJSON_ArrayForEach(pt, poly)
                        {
                            if (cJSON_IsArray(pt) && cJSON_GetArraySize(pt) >= 2)
                            {
                                double x = cJSON_GetArrayItem(pt, 0)->valuedouble;
                                double y = cJSON_GetArrayItem(pt, 1)->valuedouble;
                                zc.polygon.emplace_back(x, y);
                            }
                        }
                    }
                    /* 多边形由渲染和命中算法自动闭合。兼容旧配置中
                     * [首点, ..., 首点] 的写法，但运行配置只保留实际顶点。 */
                    while (zc.polygon.size() > 1 && zc.polygon.front() == zc.polygon.back())
                        zc.polygon.pop_back();
                    if (!zc.polygon.empty())
                        ch.roi_zones.push_back(std::move(zc));
                }
            }
        }

        g_cfg_reg.parse_channel(item, &ch);

        static const char *removed_model_fields[] = {"model_type", "model_path",     "label_path", "obj_thresh",
                                                     "nms_thresh", "detect_classes", "npu_core",   "version"};
        for (const char *field : removed_model_fields)
        {
            if (cJSON_GetObjectItemCaseSensitive(item, field))
            {
                fprintf(stderr, "[Config] channel %d field '%s' is not allowed; move it into models[]\n", ch.id, field);
                cJSON_Delete(root);
                return false;
            }
        }

        static const char *removed_channel_fields[] = {"roi_polygon",         "event_video_enable",
                                                       "event_video_pre_sec", "event_video_post_sec",
                                                       "event_video_fps",     "event_video_overlay"};
        for (const char *field : removed_channel_fields)
        {
            if (cJSON_GetObjectItemCaseSensitive(item, field))
            {
                fprintf(stderr,
                        "[Config] channel %d field '%s' is not allowed; "
                        "use roi_zones[] or report_policy\n",
                        ch.id, field);
                cJSON_Delete(root);
                return false;
            }
        }

        static const char *removed_sop_fields[] = {
            "path_sequence",      "path_target_label",  "path_enter_sec",    "path_dwell_min_sec",
            "path_dwell_max_sec", "path_enter_list",    "path_dwell_list",   "path_dwell_max_list",
            "path_edges",         "path_entries",       "path_exits",        "path_edge_limits",
            "path_reset_sec",     "path_end_mode",      "path_end_zone",     "path_end_dwell_sec",
            "path_total_min_sec", "path_total_max_sec", "path_trigger_mode", "path_trigger_mandatory",
            "path_report_normal", "path_step_x_list",   "path_step_y_list",  "path_end_x",
            "path_end_y"};
        for (const char *field : removed_sop_fields)
        {
            if (cJSON_GetObjectItemCaseSensitive(item, field))
            {
                fprintf(stderr,
                        "[Config] channel %d field '%s' is not allowed; "
                        "use logic_parameters.flow\n",
                        ch.id, field);
                cJSON_Delete(root);
                return false;
            }
        }

        cJSON *logic_parameters_item = cJSON_GetObjectItemCaseSensitive(item, "logic_parameters");
        if (logic_parameters_item && !cJSON_IsObject(logic_parameters_item))
        {
            fprintf(stderr, "[Config] channel %d logic_parameters must be a JSON object\n", ch.id);
            cJSON_Delete(root);
            return false;
        }

        cJSON *report_policy_item = cJSON_GetObjectItemCaseSensitive(item, "report_policy");
        if (report_policy_item && !cJSON_IsObject(report_policy_item))
        {
            fprintf(stderr, "[Config] channel %d report_policy must be a JSON object\n", ch.id);
            cJSON_Delete(root);
            return false;
        }
        ch.event_video = event_video_from_report_policy(report_policy_item);

        /* 唯一模型格式。一个条目是单模型，多个条目在同一帧合并结果。 */
        ch.models.clear();
        cJSON *models = cJSON_GetObjectItemCaseSensitive(item, "models");
        if (models && !cJSON_IsArray(models))
        {
            fprintf(stderr, "[Config] channel %d models must be an array\n", ch.id);
            cJSON_Delete(root);
            return false;
        }
        if (cJSON_IsArray(models))
        {
            cJSON *model_item = nullptr;
            cJSON_ArrayForEach(model_item, models)
            {
                if (!cJSON_IsObject(model_item))
                    continue;
                ChannelModelConfig model;
                cJSON *v = cJSON_GetObjectItemCaseSensitive(model_item, "id");
                if (cJSON_IsString(v) && v->valuestring)
                    model.id = v->valuestring;
                v = cJSON_GetObjectItemCaseSensitive(model_item, "enable");
                if (cJSON_IsBool(v))
                    model.enable = cJSON_IsTrue(v);
                v = cJSON_GetObjectItemCaseSensitive(model_item, "model_type");
                if (cJSON_IsString(v) && v->valuestring)
                    model.model_type = v->valuestring;
                v = cJSON_GetObjectItemCaseSensitive(model_item, "model_path");
                if (cJSON_IsString(v) && v->valuestring)
                    model.model_path = v->valuestring;
                v = cJSON_GetObjectItemCaseSensitive(model_item, "label_path");
                if (cJSON_IsString(v) && v->valuestring)
                    model.label_path = v->valuestring;
                v = cJSON_GetObjectItemCaseSensitive(model_item, "version");
                if (cJSON_IsString(v) && v->valuestring)
                    model.version = v->valuestring;
                v = cJSON_GetObjectItemCaseSensitive(model_item, "obj_thresh");
                if (cJSON_IsNumber(v))
                    model.obj_thresh = static_cast<float>(v->valuedouble);
                v = cJSON_GetObjectItemCaseSensitive(model_item, "nms_thresh");
                if (cJSON_IsNumber(v))
                    model.nms_thresh = static_cast<float>(v->valuedouble);
                v = cJSON_GetObjectItemCaseSensitive(model_item, "npu_core");
                if (cJSON_IsNumber(v))
                    model.npu_core = v->valueint;
                else if (cJSON_IsString(v) && v->valuestring)
                {
                    if (config_utils::to_lower_copy(v->valuestring) == "auto")
                        model.npu_core = -1;
                    else
                    {
                        fprintf(stderr, "[Config] channel %d model npu_core must be auto, -1, 0, 1 or 2\n", ch.id);
                        cJSON_Delete(root);
                        return false;
                    }
                }
                else if (v)
                {
                    fprintf(stderr, "[Config] channel %d model npu_core must be a number or 'auto'\n", ch.id);
                    cJSON_Delete(root);
                    return false;
                }
                v = cJSON_GetObjectItemCaseSensitive(model_item, "detect_classes");
                if (cJSON_IsArray(v))
                {
                    cJSON *class_item = nullptr;
                    cJSON_ArrayForEach(class_item, v) if (cJSON_IsString(class_item) && class_item->valuestring)
                        model.detect_classes.emplace_back(class_item->valuestring);
                }
                ch.models.push_back(std::move(model));
            }
        }

        /* logic 为空表示该通道不执行业务后处理模块；隐式空 Schema 只接受空参数对象。
         * 非空 logic 的专有参数由嵌入二进制的 logic.json Schema 校验并补默认值。
         * 这里在初始启动和热重载时都执行；失败时整份新配置不发布。 */
        {
            std::vector<LogicParameterError> parameter_errors;
            std::string normalized_parameters;
            if (!logic_parameters_resolve(ch.logic, ch.logic_parameters_json, &normalized_parameters, nullptr,
                                          &parameter_errors))
            {
                fprintf(stderr, "[Config] channel %d logic_parameters validation failed:\n", ch.id);
                for (const auto &error : parameter_errors)
                    fprintf(stderr, "  - %s: %s\n", error.field.c_str(), error.message.c_str());
                cJSON_Delete(root);
                return false;
            }
            ch.logic_parameters_json = std::move(normalized_parameters);
        }
        if (ch.threads < 0)
            ch.threads = cfg.channel_threads;
        if (ch.max_fps <= 0)
            ch.max_fps = (cfg.max_fps > 0) ? cfg.max_fps : 30;
        // 注意不要级联 playback_fps！ playback_fps = -1 对于实时流（RTSP/USB）表示不节流！
        // file 类型的播放器已在 decChannel.cpp 内部专门处理了 <=0 回落逻辑。

        if (ch.tracker_enable == -1)
        {
            ch.tracker_enable = cfg.tracker_enable;
            ch.tracker_iou_thresh = cfg.tracker_iou_thresh;
            ch.tracker_max_miss = cfg.tracker_max_miss;
            ch.tracker_min_hits = cfg.tracker_min_hits;
        }

        // 参数钳位
        if (ch.tracker_enable < 0)
            ch.tracker_enable = 1;
        if (ch.tracker_iou_thresh < 0.01f)
            ch.tracker_iou_thresh = 0.01f;
        if (ch.tracker_iou_thresh > 1.0f)
            ch.tracker_iou_thresh = 1.0f;
        if (ch.tracker_max_miss < 1)
            ch.tracker_max_miss = 1;
        if (ch.tracker_max_miss > 100)
            ch.tracker_max_miss = 100;
        if (ch.tracker_min_hits < 1)
            ch.tracker_min_hits = 1;
        if (ch.tracker_min_hits > 100)
            ch.tracker_min_hits = 100;
        if (ch.threads < 1)
            ch.threads = 1;

        // 校验
        if (ch.id < 0 || ch.id >= MAX_CHANNEL_NUM)
        {
            fprintf(stderr, "[Config] channel id out of range: %d\n", ch.id);
            return false;
        }
        if (used_ids.find(ch.id) != used_ids.end())
        {
            fprintf(stderr, "[Config] duplicate channel id: %d\n", ch.id);
            return false;
        }
        used_ids.insert(ch.id);

        /* src_type 必填：前后端均不再自动推断，缺省即配置错误 */
        ch.stream.src_type = config_utils::normalize_src_type(ch.stream);
        if (ch.stream.src_type.empty())
        {
            fprintf(stderr, "[Config] channel %d 缺少 stream.src_type（必填: rtsp/file/usb，已取消自动推断）\n", ch.id);
            cJSON_Delete(root);
            return false;
        }
        if (!config_utils::is_supported_src_type(ch.stream.src_type))
        {
            fprintf(stderr, "[Config] channel %d invalid src_type: %s\n", ch.id, ch.stream.src_type.c_str());
            cJSON_Delete(root);
            return false;
        }

        std::string stream_location = config_utils::resolve_stream_location(ch.stream, ch.stream.src_type);
        if (ch.enable && stream_location.empty())
        {
            fprintf(stderr, "[Config] channel %d missing stream location (url/device)\n", ch.id);
            cJSON_Delete(root);
            return false;
        }

        if (ch.stream.src_type == "usb" && !stream_location.empty() &&
            !config_utils::starts_with(stream_location, "/dev/video"))
        {
            fprintf(stderr, "[Config] channel %d invalid usb device: %s\n", ch.id, stream_location.c_str());
            cJSON_Delete(root);
            return false;
        }

        if (ch.stream.src_type == "rtsp" && !ch.stream.video_enc.empty() && ch.stream.video_enc != "h264" &&
            ch.stream.video_enc != "h265")
        {
            fprintf(stderr, "[Config] channel %d invalid video_enc\n", ch.id);
            cJSON_Delete(root);
            return false;
        }

        if (ch.enable && !stream_location.empty())
        {
            cfg.channels.push_back(ch);
        }
        ++seq_idx;
    }

    std::sort(cfg.channels.begin(), cfg.channels.end(),
              [](const ChannelConfig &a, const ChannelConfig &b) { return a.id < b.id; });

    /* channels 只表示画布提供的可选输入子集，但其中每个 ID 仍必须是本应用的有效通道。
     * C++ 按 ID 自选通道不经过这里，直接从 GlobalContext 的应用通道集合中读取。 */
    for (const GlobalLogicConfig &global_logic : cfg.global_logics)
    {
        std::set<int> connected_ids;
        for (int channel_id : global_logic.channels)
        {
            const bool exists = std::any_of(cfg.channels.begin(), cfg.channels.end(), [&](const ChannelConfig &channel) {
                return channel.id == channel_id;
            });
            if (!exists)
            {
                fprintf(stderr, "[Config] global instance %s references unknown connected channel %d\n",
                        global_logic.instance_id.c_str(), channel_id);
                cJSON_Delete(root);
                return false;
            }
            if (!connected_ids.insert(channel_id).second)
            {
                fprintf(stderr, "[Config] global instance %s has duplicate connected channel %d\n",
                        global_logic.instance_id.c_str(), channel_id);
                cJSON_Delete(root);
                return false;
            }
        }
        if (global_logic.media_source_channel_id >= 0)
        {
            const bool exists =
                std::any_of(cfg.channels.begin(), cfg.channels.end(), [&](const ChannelConfig &channel) {
                    return channel.id == global_logic.media_source_channel_id;
                });
            if (!exists)
            {
                fprintf(stderr, "[Config] global instance %s references unknown media source channel %d\n",
                        global_logic.instance_id.c_str(), global_logic.media_source_channel_id);
                cJSON_Delete(root);
                return false;
            }
        }
        if (global_logic.enable && global_logic.event_video.enable && global_logic.media_source_channel_id < 0)
        {
            fprintf(stderr, "[Config] global instance %s enables event video but has no media source channel\n",
                    global_logic.instance_id.c_str());
            cJSON_Delete(root);
            return false;
        }
    }

    /* 全局逻辑复用事件录像器。预录只作用于明确选择的媒体来源通道，绝不因为
     * 没有画布连线而隐式扩展到应用全部通道。 */
    for (const GlobalLogicConfig &global_logic : cfg.global_logics)
    {
        if (!global_logic.enable || !global_logic.event_video.enable)
            continue;
        for (ChannelConfig &channel : cfg.channels)
        {
            if (channel.id != global_logic.media_source_channel_id)
                continue;
            if (!channel.event_video.enable)
            {
                channel.event_video = global_logic.event_video;
            }
            else
            {
                channel.event_video.pre_sec = std::max(channel.event_video.pre_sec, global_logic.event_video.pre_sec);
                channel.event_video.post_sec = std::max(channel.event_video.post_sec, global_logic.event_video.post_sec);
                channel.event_video.fps = std::max(channel.event_video.fps, global_logic.event_video.fps);
                if (global_logic.event_video.overlay == "custom" || global_logic.event_video.overlay == "all")
                    channel.event_video.overlay = global_logic.event_video.overlay;
            }
        }
    }

    if (cfg.channels.empty())
    {
        fprintf(stderr, "[Config] no enabled channels\n");
        cJSON_Delete(root);
        return false;
    }

    if (cfg.tile_cols <= 0)
        cfg.tile_cols = 2;
    if (cfg.tile_rows <= 0)
        cfg.tile_rows = 2;
    if (cfg.queue_size <= 0)
        cfg.queue_size = 1;
    cJSON_Delete(root);

    // 【修改点】：分级验证 — 首次加载做完整验证，热重载只做关键字段验证
    if (!is_hotreload)
    {
        std::vector<ConfigValidator::ValidationError> errors;
        if (!ConfigValidator::validate(cfg, errors))
        {
            fprintf(stderr, "[Config] Validation failed:\n");
            for (const auto &err : errors)
            {
                fprintf(stderr, "  - %s: %s\n", err.field.c_str(), err.message.c_str());
            }
            return false;
        }
    }
    else
    {
        std::vector<ConfigValidator::ValidationError> errors;
        if (!ConfigValidator::validate_critical(cfg, errors))
        {
            fprintf(stderr, "[Config] Hotreload critical validation failed:\n");
            for (const auto &err : errors)
            {
                fprintf(stderr, "  - %s: %s\n", err.field.c_str(), err.message.c_str());
            }
            return false;
        }
        printf("[Config] Hotreload critical validation passed (%zu channels)\n", cfg.channels.size());
    }

    g_cfg_reg.trigger_reload();
    return true;
}

/*======================== 配置文件修改时间（纳秒） ========================*/
uint64_t config_get_mtime(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL + static_cast<uint64_t>(st.st_mtim.tv_nsec);
}
