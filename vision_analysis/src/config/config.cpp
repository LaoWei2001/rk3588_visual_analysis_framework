/**
 * @file config.cpp
 * @brief JSON 配置解析与热加载
 */
#include "config.h"
#include "../third_party/json/cJSON.h"
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
        if (!cJSON_IsFalse(enabled) && cJSON_IsString(media) && std::string(media->valuestring) == "video")
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
    cfg.obj_thresh = 0.4f;
    cfg.nms_thresh = 0.45f;
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

    static const char *removed_global_model_fields[] = {"model_type", "model_path", "label_path"};
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
        cJSON *gl_item = nullptr;
        cJSON_ArrayForEach(gl_item, gl_array)
        {
            if (!cJSON_IsObject(gl_item))
                continue;

            GlobalLogicConfig gl_cfg;
            gl_cfg.enable = false;
            gl_cfg.logic = "global_default";
            gl_cfg.poll_interval_ms = 100;

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

            cfg.global_logics.push_back(gl_cfg);

            if (gl_cfg.enable)
            {
                printf("[Config] global_logic[%zu] enabled: logic=%s poll=%dms channels=%zu\n",
                       cfg.global_logics.size() - 1, gl_cfg.logic.c_str(), gl_cfg.poll_interval_ms,
                       gl_cfg.channels.size());
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
        for (auto &model : ch.models)
        {
            if (model.obj_thresh < 0.0f)
                model.obj_thresh = cfg.obj_thresh;
            if (model.nms_thresh < 0.0f)
                model.nms_thresh = cfg.nms_thresh;
            if (model.detect_classes.empty())
                model.detect_classes = cfg.detect_classes;
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
    if (cfg.obj_thresh < 0.0f)
        cfg.obj_thresh = 0.0f;
    if (cfg.obj_thresh > 1.0f)
        cfg.obj_thresh = 1.0f;
    if (cfg.nms_thresh < 0.0f)
        cfg.nms_thresh = 0.0f;
    if (cfg.nms_thresh > 1.0f)
        cfg.nms_thresh = 1.0f;

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
