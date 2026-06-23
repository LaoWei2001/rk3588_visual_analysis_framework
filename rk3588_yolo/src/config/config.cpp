/**
 * @file config.cpp
 * @brief JSON 配置解析与热加载
 */
#include "config.h"
#include "../third_party/json/cJSON.h"
#include "config_registry.h"
#include "config_validator.h"
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
     * infer_enable=false → 传统算法通道：跳过 NPU 推理，但仍解码/显示/逐帧跑
     * logic。 */
    return ch_cfg.infer_enable && !ch_cfg.model_path.empty() && !ch_cfg.model_type.empty();
}
} // namespace config_utils

bool load_config(const std::string &path, AppConfig &cfg)
{
    // 保存热重载标记（在 init_config_fields 之前，因为后面 cfg.config_path
    // 会被覆盖）
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

    int schema_ver = 2;
    cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (cJSON_IsNumber(sv))
        schema_ver = sv->valueint;
    if (schema_ver != 1 && schema_ver != 2)
    {
        fprintf(stderr, "[Config] unsupported schema_version: %d\n", schema_ver);
        cJSON_Delete(root);
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
    cfg.npu_cores = 3;
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
            gl_cfg.logic = "global_example";
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
                printf("[Config] global_logic[%zu] enabled: logic=%s poll=%dms "
                       "channels=%zu\n",
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
        ch.obj_thresh = -1.0f;
        ch.nms_thresh = -1.0f;
        ch.threads = -1;
        ch.playback_fps = -1;
        ch.max_fps = -1;
        ch.npu_core = -1;
        ch.tracker_enable = -1;

        // 解析stream对象
        if (schema_ver >= 2)
        {
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
        }
        else
        {
            cJSON *rtsp = cJSON_GetObjectItemCaseSensitive(item, "rtsp");
            if (cJSON_IsString(rtsp) && rtsp->valuestring)
                ch.stream.url = rtsp->valuestring;
            cJSON *enc = cJSON_GetObjectItemCaseSensitive(item, "video_enc");
            if (cJSON_IsString(enc) && enc->valuestring)
                ch.stream.video_enc = enc->valuestring;
        }

        /* 解析通道 ROI 区域 (roi_zones / roi_polygon, 归一化坐标 0~1) */
        ch.roi_zones.clear();
        ch.roi_polygon.clear();
        {
            cJSON *rz = cJSON_GetObjectItemCaseSensitive(item, "roi_zones");
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
                    if (!zc.polygon.empty())
                        ch.roi_zones.push_back(std::move(zc));
                }
            }
            cJSON *rp = cJSON_GetObjectItemCaseSensitive(item, "roi_polygon");
            if (cJSON_IsArray(rp))
            {
                cJSON *pt = nullptr;
                cJSON_ArrayForEach(pt, rp)
                {
                    if (cJSON_IsArray(pt) && cJSON_GetArraySize(pt) >= 2)
                    {
                        double x = cJSON_GetArrayItem(pt, 0)->valuedouble;
                        double y = cJSON_GetArrayItem(pt, 1)->valuedouble;
                        ch.roi_polygon.emplace_back(x, y);
                    }
                }
            }
        }

        g_cfg_reg.parse_channel(item, &ch);

        // 通道配置继承全局配置
        // 如果通道逻辑是空的, 则分配默认逻辑
        if (ch.logic.empty())
            ch.logic = "logic1";
        // 如果模型路径存在但模型类型是空的, 就分配全局设置的模型类型
        if (!ch.model_path.empty() && ch.model_type.empty())
            ch.model_type = cfg.model_type;

        // 如果模型路径存在但标签是空的, 就分配全局设置的标签
        if (!ch.model_path.empty() && ch.label_path.empty())
            ch.label_path = cfg.label_path;
        if (ch.obj_thresh < 0.0f)
            ch.obj_thresh = cfg.obj_thresh;
        if (ch.nms_thresh < 0.0f)
            ch.nms_thresh = cfg.nms_thresh;
        if (ch.threads < 0)
            ch.threads = cfg.channel_threads;
        if (ch.detect_classes.empty())
            ch.detect_classes = cfg.detect_classes;
        if (ch.max_fps <= 0)
            ch.max_fps = (cfg.max_fps > 0) ? cfg.max_fps : 30;
        // 注意不要级联 playback_fps！ playback_fps = -1
        // 对于实时流（RTSP/USB）表示不节流！ file 类型的播放器已在 decChannel.cpp
        // 内部专门处理了 <=0 回落逻辑。

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
        if (ch.obj_thresh > 1.0f)
            ch.obj_thresh = 1.0f;
        if (ch.nms_thresh > 1.0f)
            ch.nms_thresh = 1.0f;
        if (ch.npu_core > 2)
            ch.npu_core = 2;

        // 校验
        if (ch.id < 0 || ch.id >= MAX_CHANNEL_NUM)
        {
            fprintf(stderr, "[Config] channel id out of range: %d\n", ch.id);
            cJSON_Delete(root);
            return false;
        }
        if (used_ids.find(ch.id) != used_ids.end())
        {
            fprintf(stderr, "[Config] duplicate channel id: %d\n", ch.id);
            cJSON_Delete(root);
            return false;
        }
        used_ids.insert(ch.id);

        /* src_type 必填：前后端均不再自动推断，缺省即配置错误 */
        ch.stream.src_type = config_utils::normalize_src_type(ch.stream);
        if (ch.stream.src_type.empty())
        {
            fprintf(stderr,
                    "[Config] channel %d 缺少 stream.src_type（必填: "
                    "rtsp/file/usb，已取消自动推断）\n",
                    ch.id);
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
    if (cfg.npu_cores <= 0)
        cfg.npu_cores = 1;
    if (cfg.npu_cores > 3)
        cfg.npu_cores = 3;
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
            cJSON_Delete(root);
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
            cJSON_Delete(root);
            return false;
        }
        printf("[Config] Hotreload critical validation passed (%zu channels)\n", cfg.channels.size());
    }

    g_cfg_reg.trigger_reload();
    return true;
}

/*======================== 配置文件修改时间 ========================*/
uint64_t config_get_mtime(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<uint64_t>(st.st_mtime);
}
