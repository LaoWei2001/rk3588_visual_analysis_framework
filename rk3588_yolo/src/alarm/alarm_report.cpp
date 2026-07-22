#include "alarm_report.h"

#include "logic/core/channel_logic.h"
#include "../config/config.h"
#include "../player/display.h"
#include "../recorder/event_video_recorder.h"
#include "../third_party/json/cJSON.h"

#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <pthread.h>
#include <queue>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctime>

namespace {

enum AlarmRoute : uint32_t
{
    ALARM_ROUTE_NONE         = 0,
    ALARM_IMAGE_TO_SERVER    = 1u << 0,
    ALARM_IMAGE_TO_DIFY      = 1u << 1,
    ALARM_VIDEO_TO_DIFY      = 1u << 2
};

enum AlarmMedia : uint32_t
{
    ALARM_MEDIA_NONE  = 0,
    ALARM_MEDIA_IMAGE = 1u << 0,
    ALARM_MEDIA_VIDEO = 1u << 1
};

static const long long OUTBOX_CAP_BYTES = 1024LL * 1024LL * 1024LL;
static const long long OUTBOX_MIN_FREE_BYTES = 512LL * 1024LL * 1024LL;

struct ImageJob
{
    std::string event_dir;
    cv::Mat image;
    cv::Mat raw;
    std::string overlay_mode;
    RenderParams render_params;
    std::vector<RoiZone> rois;
    std::vector<AlgoResult> results;
    std::vector<DrawCommand> commands;
};

struct ActiveEvent
{
    std::string event_id;
    uint64_t last_trigger_ms = 0;
};

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker_tid;
static bool g_worker_started = false;
static bool g_running = false;
static std::queue<ImageJob> g_image_jobs;
static std::map<std::string, ActiveEvent> g_active; /* channel:type/dedup → event */
static unsigned long g_seq = 0;

static uint64_t steady_now_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static int mkdir_p(const std::string &path)
{
    if (path.empty()) return -1;
    std::string cur;
    size_t start = 0;
    if (path[0] == '/') { cur = "/"; start = 1; }
    while (start <= path.size())
    {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty())
        {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += part;
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return -1;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return 0;
}

static std::string store_dir()
{
    const char *env = getenv("ALARM_STORE_DIR");
    std::string dir = (env && *env) ? env : "./alarm_store";
    mkdir_p(dir);
    return dir;
}

static long long directory_bytes(const std::string &dir)
{
    long long total = 0;
    DIR *dp = opendir(dir.c_str());
    if (!dp) return 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string path = dir + "/" + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        total += S_ISDIR(st.st_mode) ? directory_bytes(path) : (long long)st.st_size;
    }
    closedir(dp);
    return total;
}

static void remove_tree(const std::string &dir)
{
    DIR *dp = opendir(dir.c_str());
    if (!dp) return;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string path = dir + "/" + entry->d_name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) remove_tree(path); else unlink(path.c_str());
    }
    closedir(dp);
    rmdir(dir.c_str());
}

/* 容量回收只能删除静态待处理事件。manifest 无法读取/解析时采用保守策略，
 * 防止半写入文件或损坏文件被误判成可删除。 */
static bool manifest_has_active_work(const std::string &manifest_path)
{
    std::ifstream manifest(manifest_path);
    if (!manifest)
    {
        fprintf(stderr, "[alarm_outbox] cannot read manifest; skip eviction: %s\n",
                manifest_path.c_str());
        return true;
    }

    const std::string text((std::istreambuf_iterator<char>(manifest)),
                           std::istreambuf_iterator<char>());
    cJSON *root = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(root))
    {
        fprintf(stderr, "[alarm_outbox] invalid manifest JSON; skip eviction: %s\n",
                manifest_path.c_str());
        cJSON_Delete(root);
        return true;
    }

    bool active = false;
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state) && state->valuestring &&
        strcmp(state->valuestring, "collecting") == 0)
        active = true;

    /* 当前 schema 的上传状态位于 deliveries[].status；同时兼容旧 manifest
     * 可能存在的顶层 status 字段。 */
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (cJSON_IsString(status) && status->valuestring &&
        strcmp(status->valuestring, "uploading") == 0)
        active = true;

    const cJSON *upload = cJSON_GetObjectItemCaseSensitive(root, "upload");
    const cJSON *upload_status = cJSON_IsObject(upload)
        ? cJSON_GetObjectItemCaseSensitive(upload, "status") : nullptr;
    if (cJSON_IsString(upload_status) && upload_status->valuestring &&
        strcmp(upload_status->valuestring, "uploading") == 0)
        active = true;

    const cJSON *deliveries = cJSON_GetObjectItemCaseSensitive(root, "deliveries");
    if (cJSON_IsArray(deliveries))
    {
        const cJSON *delivery = nullptr;
        cJSON_ArrayForEach(delivery, deliveries)
        {
            const cJSON *delivery_status =
                cJSON_GetObjectItemCaseSensitive(delivery, "status");
            if (cJSON_IsString(delivery_status) && delivery_status->valuestring &&
                strcmp(delivery_status->valuestring, "uploading") == 0)
            {
                active = true;
                break;
            }
        }
    }

    cJSON_Delete(root);
    return active;
}

static void enforce_outbox_cap()
{
    const std::string root = store_dir();
    const char *cap_env = getenv("ALARM_STORE_MAX_BYTES");
    const char *free_env = getenv("ALARM_STORE_MIN_FREE_BYTES");
    const long long cap_bytes = cap_env && *cap_env ? atoll(cap_env) : OUTBOX_CAP_BYTES;
    const long long min_free_bytes = free_env && *free_env ? atoll(free_env) : OUTBOX_MIN_FREE_BYTES;
    struct statvfs fsinfo;
    long long free_bytes = statvfs(root.c_str(), &fsinfo) == 0
        ? (long long)fsinfo.f_bavail * (long long)fsinfo.f_frsize : min_free_bytes;
    struct Candidate { std::string path; time_t mtime; long long bytes; };
    std::vector<Candidate> events;
    long long total = 0;
    DIR *dp = opendir(root.c_str());
    if (!dp) return;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string path = root + "/" + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode) && access((path + "/manifest.json").c_str(), F_OK) == 0)
        {
            long long bytes = directory_bytes(path);
            total += bytes;
            if (!manifest_has_active_work(path + "/manifest.json"))
                events.push_back({path, st.st_mtime, bytes});
        }
    }
    closedir(dp);
    std::sort(events.begin(), events.end(), [](const Candidate &a, const Candidate &b) { return a.mtime < b.mtime; });
    for (const auto &event : events)
    {
        if (total <= cap_bytes && free_bytes >= min_free_bytes) break;
        fprintf(stderr, "[alarm_outbox] cap exceeded; evict oldest pending event: %s\n", event.path.c_str());
        remove_tree(event.path);
        total -= event.bytes;
        free_bytes += event.bytes;
    }
}

static std::string make_event_id(int channel_id)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    time_t sec = ts.tv_sec;
    localtime_r(&sec, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    char id[128];
    snprintf(id, sizeof(id), "ch%d_%s_%03ld_%lu", channel_id, stamp,
             ts.tv_nsec / 1000000L, ++g_seq);
    return id;
}

static cJSON *fields_to_json(const AlarmFields &fields)
{
    cJSON *obj = cJSON_CreateObject();
    for (const auto &entry : fields.values())
    {
        const AlarmValue &v = entry.second;
        if (v.type() == AlarmValue::NUMBER)
            cJSON_AddNumberToObject(obj, entry.first.c_str(), v.number());
        else if (v.type() == AlarmValue::BOOLEAN)
            cJSON_AddBoolToObject(obj, entry.first.c_str(), v.boolean() ? 1 : 0);
        else if (v.type() == AlarmValue::JSON)
        {
            cJSON *parsed = cJSON_Parse(v.text().c_str());
            if (parsed) cJSON_AddItemToObject(obj, entry.first.c_str(), parsed);
            else cJSON_AddStringToObject(obj, entry.first.c_str(), v.text().c_str());
        }
        else
            cJSON_AddStringToObject(obj, entry.first.c_str(), v.text().c_str());
    }
    return obj;
}

static cJSON *parse_object_or_empty(const std::string &text)
{
    cJSON *obj = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(obj))
    {
        cJSON_Delete(obj);
        obj = cJSON_CreateObject();
    }
    return obj;
}

static bool policy_has_delivery(cJSON *policy, const char *media, const char *target)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    if (!cJSON_IsArray(arr)) return false;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr)
    {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(item, "media");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(item, "target");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        if (cJSON_IsFalse(enabled)) continue;
        if (cJSON_IsString(m) && cJSON_IsString(t) &&
            strcmp(m->valuestring, media) == 0 && strcmp(t->valuestring, target) == 0)
            return true;
    }
    return false;
}

static void add_delivery_statuses(cJSON *policy, cJSON *root)
{
    cJSON *out = cJSON_CreateArray();
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    cJSON *item = nullptr;
    if (cJSON_IsArray(arr))
    {
        int index = 0;
        cJSON_ArrayForEach(item, arr)
        {
            cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
            if (cJSON_IsFalse(enabled)) { ++index; continue; }
            cJSON *copy = cJSON_Duplicate(item, 1);
            cJSON *id = cJSON_GetObjectItemCaseSensitive(copy, "id");
            if (!cJSON_IsString(id))
            {
                char generated[32]; snprintf(generated, sizeof(generated), "delivery_%d", index);
                cJSON_AddStringToObject(copy, "id", generated);
            }
            cJSON_AddStringToObject(copy, "status", "pending");
            cJSON_AddNumberToObject(copy, "attempts", 0);
            cJSON_AddStringToObject(copy, "last_error", "");
            cJSON_AddItemToArray(out, copy);
            ++index;
        }
    }
    cJSON_AddItemToObject(root, "deliveries", out);
}

static bool atomic_write_json(const std::string &path, cJSON *root)
{
    /* manifest 是运行时状态文件，同时也是板端排障的第一现场。
     * 统一使用格式化 JSON，并在文件末尾保留换行，确保命令行和编辑器都能直接阅读。
     * 后续所有 C++ 更新（媒体就绪、合并告警等）都会继续走此函数，不会重新压成一行。 */
    char *text = cJSON_Print(root);
    if (!text) return false;
    std::string tmp = path + ".tmp";
    FILE *fp = fopen(tmp.c_str(), "wb");
    const size_t text_len = strlen(text);
    bool ok = fp && fwrite(text, 1, text_len, fp) == text_len;
    if (ok && (text_len == 0 || text[text_len - 1] != '\n'))
        ok = fwrite("\n", 1, 1, fp) == 1;
    if (fp) { fflush(fp); fsync(fileno(fp)); fclose(fp); }
    cJSON_free(text);
    if (!ok) { ::unlink(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}

static cJSON *read_manifest(const std::string &event_dir)
{
    std::ifstream in(event_dir + "/manifest.json");
    if (!in) return nullptr;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return cJSON_Parse(text.c_str());
}

static bool record_merged_trigger(const std::string &event_id, uint64_t trigger_unix_ms,
                                  const std::string &trigger_time, const AlarmFields &fields)
{
    bool updated = false;
    const std::string event_dir = store_dir() + "/" + event_id;
    pthread_mutex_lock(&g_mtx);
    cJSON *root = read_manifest(event_dir);
    if (root)
    {
        cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "trigger_count");
        const int next_count = cJSON_IsNumber(count) ? count->valueint + 1 : 2;
        cJSON_ReplaceItemInObjectCaseSensitive(root, "trigger_count", cJSON_CreateNumber(next_count));
        cJSON_ReplaceItemInObjectCaseSensitive(root, "last_trigger_unix_ms",
                                                cJSON_CreateNumber((double)trigger_unix_ms));
        cJSON_ReplaceItemInObjectCaseSensitive(root, "end_time", cJSON_CreateString(trigger_time.c_str()));
        if (!fields.values().empty())
            cJSON_ReplaceItemInObjectCaseSensitive(root, "fields", fields_to_json(fields));
        cJSON *history = cJSON_GetObjectItemCaseSensitive(root, "merged_triggers");
        if (!cJSON_IsArray(history))
        {
            history = cJSON_CreateArray();
            cJSON_AddItemToObject(root, "merged_triggers", history);
        }
        cJSON *trigger = cJSON_CreateObject();
        cJSON_AddNumberToObject(trigger, "trigger_unix_ms", (double)trigger_unix_ms);
        cJSON_AddItemToObject(trigger, "fields", fields_to_json(fields));
        cJSON_AddItemToArray(history, trigger);
        updated = atomic_write_json(event_dir + "/manifest.json", root);
        cJSON_Delete(root);
    }
    pthread_mutex_unlock(&g_mtx);
    return updated;
}

static void clear_active_event(const std::string &merge_key, const std::string &event_id)
{
    pthread_mutex_lock(&g_mtx);
    auto found = g_active.find(merge_key);
    if (found != g_active.end() && found->second.event_id == event_id) g_active.erase(found);
    pthread_mutex_unlock(&g_mtx);
}

static void update_media_ready(const std::string &event_dir, const char *key, const char *filename)
{
    pthread_mutex_lock(&g_mtx);
    cJSON *root = read_manifest(event_dir);
    if (root)
    {
        cJSON *media = cJSON_GetObjectItemCaseSensitive(root, "media");
        if (!cJSON_IsObject(media)) { media = cJSON_CreateObject(); cJSON_AddItemToObject(root, "media", media); }
        cJSON_ReplaceItemInObjectCaseSensitive(media, key, cJSON_CreateString(filename));
        cJSON *requested = cJSON_GetObjectItemCaseSensitive(root, "media_requested");
        cJSON *want_image = cJSON_GetObjectItemCaseSensitive(requested, "image");
        cJSON *want_video = cJSON_GetObjectItemCaseSensitive(requested, "video");
        cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(media, "snapshot");
        cJSON *raw = cJSON_GetObjectItemCaseSensitive(media, "raw");
        cJSON *video = cJSON_GetObjectItemCaseSensitive(media, "video");
        const bool image_ready = !cJSON_IsTrue(want_image) ||
                                 (cJSON_IsString(snapshot) && snapshot->valuestring[0] &&
                                  cJSON_IsString(raw) && raw->valuestring[0]);
        const bool video_ready = !cJSON_IsTrue(want_video) ||
                                 (cJSON_IsString(video) && video->valuestring[0]);
        if (image_ready && video_ready)
            cJSON_ReplaceItemInObjectCaseSensitive(root, "state", cJSON_CreateString("pending"));
        atomic_write_json(event_dir + "/manifest.json", root);
        cJSON_Delete(root);
    }
    pthread_mutex_unlock(&g_mtx);
}

static void *image_worker(void *)
{
    while (true)
    {
        ImageJob job;
        pthread_mutex_lock(&g_mtx);
        while (g_image_jobs.empty() && g_running) pthread_cond_wait(&g_cv, &g_mtx);
        if (g_image_jobs.empty() && !g_running) { pthread_mutex_unlock(&g_mtx); break; }
        job = std::move(g_image_jobs.front());
        g_image_jobs.pop();
        pthread_mutex_unlock(&g_mtx);

        if (job.overlay_mode != "none" && !job.image.empty())
        {
            job.render_params.roi_zones = &job.rois;
            job.render_params.results = &job.results;
            job.render_params.draw_cmds = &job.commands;
            render_overlays(job.image, job.render_params);
        }
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        if (!job.image.empty() && cv::imwrite(job.event_dir + "/snapshot.jpg", job.image, params))
            update_media_ready(job.event_dir, "snapshot", "snapshot.jpg");
        if (!job.raw.empty() && cv::imwrite(job.event_dir + "/raw.jpg", job.raw, params))
            update_media_ready(job.event_dir, "raw", "raw.jpg");
        enforce_outbox_cap();
    }
    return nullptr;
}

static bool ensure_worker_locked()
{
    if (g_worker_started) return true;
    g_running = true;
    if (pthread_create(&g_worker_tid, nullptr, image_worker, nullptr) != 0)
    {
        g_running = false;
        return false;
    }
    g_worker_started = true;
    return true;
}

static uint32_t policy_routes(cJSON *policy)
{
    uint32_t routes = ALARM_ROUTE_NONE;
    if (policy_has_delivery(policy, "image", "server")) routes |= ALARM_IMAGE_TO_SERVER;
    if (policy_has_delivery(policy, "image", "dify")) routes |= ALARM_IMAGE_TO_DIFY;
    if (policy_has_delivery(policy, "video", "dify")) routes |= ALARM_VIDEO_TO_DIFY;
    return routes;
}

static double policy_number(cJSON *policy, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(policy, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static std::string policy_string(cJSON *policy, const char *key, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(policy, key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

static void synthesize_deliveries(cJSON *policy, uint32_t routes)
{
    cJSON *arr = cJSON_CreateArray();
    auto add = [&](const char *id, const char *media, const char *target) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", id);
        cJSON_AddStringToObject(d, "media", media);
        cJSON_AddStringToObject(d, "target", target);
        cJSON_AddItemToArray(arr, d);
    };
    if (routes & ALARM_IMAGE_TO_SERVER) add("image_server", "image", "server");
    if (routes & ALARM_IMAGE_TO_DIFY) add("image_dify", "image", "dify");
    if (routes & ALARM_VIDEO_TO_DIFY) add("video_dify", "video", "dify");
    cJSON_DeleteItemFromObjectCaseSensitive(policy, "deliveries");
    cJSON_AddItemToObject(policy, "deliveries", arr);
}

/* JSON-only 记录复用当前通道第一条启用的 Dify 投递连接，但不继承其图片/视频要求。
 * 这样正常SOP结果仍经过同一个可靠发件箱，同时不会触发截图或录像。 */
static bool configure_dify_json_delivery(cJSON *policy)
{
    cJSON *configured = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    if (!cJSON_IsArray(configured)) return false;

    cJSON *source = nullptr;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, configured)
    {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        cJSON *target = cJSON_GetObjectItemCaseSensitive(item, "target");
        if (!cJSON_IsFalse(enabled) && cJSON_IsString(target) &&
            strcmp(target->valuestring, "dify") == 0)
        {
            source = item;
            break;
        }
    }
    if (!source) return false;

    cJSON *delivery = cJSON_Duplicate(source, 1);
    if (!delivery) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(delivery, "media");
    cJSON_AddStringToObject(delivery, "media", "json");
    cJSON_DeleteItemFromObjectCaseSensitive(delivery, "file_variable");
    cJSON_DeleteItemFromObjectCaseSensitive(delivery, "file_input_mode");
    cJSON *event_variable = cJSON_GetObjectItemCaseSensitive(delivery, "event_variable");
    if (!cJSON_IsString(event_variable) || !event_variable->valuestring[0])
    {
        cJSON_DeleteItemFromObjectCaseSensitive(delivery, "event_variable");
        cJSON_AddStringToObject(delivery, "event_variable", "event_json");
    }

    cJSON *json_deliveries = cJSON_CreateArray();
    cJSON_AddItemToArray(json_deliveries, delivery);
    cJSON_DeleteItemFromObjectCaseSensitive(policy, "deliveries");
    cJSON_AddItemToObject(policy, "deliveries", json_deliveries);
    return true;
}

} // namespace

std::string alarm_report(ChannelContext *ctx, const AlarmRequest &input)
{
    if (!ctx || !ctx->config || input.type.empty()) return "";
    enforce_outbox_cap();
    const ChannelConfig &cfg = *ctx->config;
    cJSON *policy = parse_object_or_empty(cfg.report_policy_json);
    uint32_t routes = policy_routes(policy);

    cJSON *configured_deliveries = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    if (!cJSON_IsArray(configured_deliveries) || cJSON_GetArraySize(configured_deliveries) == 0)
        synthesize_deliveries(policy, routes);

    if (input.dify_json_only && !configure_dify_json_delivery(policy))
    {
        cJSON_Delete(policy);
        return "";
    }

    uint32_t media_flags = ALARM_MEDIA_NONE;
    if (!input.dify_json_only)
    {
        if (routes & (ALARM_IMAGE_TO_SERVER | ALARM_IMAGE_TO_DIFY)) media_flags |= ALARM_MEDIA_IMAGE;
        if (routes & ALARM_VIDEO_TO_DIFY) media_flags |= ALARM_MEDIA_VIDEO;
        if (media_flags == ALARM_MEDIA_NONE) { cJSON_Delete(policy); return ""; }
    }

    float merge_sec = (float)policy_number(policy, "merge_window_sec", 5.0);
    merge_sec = std::max(0.0f, std::min(60.0f, merge_sec));
    const float video_pre_sec = (float)policy_number(policy, "video_pre_sec", cfg.event_video_pre_sec);
    const float video_post_sec = (float)policy_number(policy, "video_post_sec", cfg.event_video_post_sec);
    const int video_fps = (int)policy_number(policy, "video_fps", cfg.event_video_fps);
    const std::string image_overlay = policy_string(policy, "image_overlay", "custom");
    /* 普通告警按“同通道 + 同报警类型”合并；SOP正式结果一轮一条，不参与合并。 */
    std::string merge_key = std::to_string(ctx->chnId) + ":" + input.type;
    uint64_t now_ms = steady_now_ms();
    const bool allow_merge = input.merge_enabled && merge_sec > 0.0f;

    pthread_mutex_lock(&g_mtx);
    auto found = g_active.find(merge_key);
    if (allow_merge && found != g_active.end() &&
        now_ms - found->second.last_trigger_ms <= (uint64_t)(merge_sec * 1000.0f))
    {
        found->second.last_trigger_ms = now_ms;
        std::string existing = found->second.event_id;
        pthread_mutex_unlock(&g_mtx);
        if (record_merged_trigger(existing, ctx->unix_ms, ctx->time_str(), input.fields))
        {
            if (media_flags & ALARM_MEDIA_VIDEO)
                event_video_recorder_extend(ctx->chnId, input.type);
            cJSON_Delete(policy);
            return existing;
        }
        pthread_mutex_lock(&g_mtx);
        auto stale = g_active.find(merge_key);
        if (stale != g_active.end() && stale->second.event_id == existing) g_active.erase(stale);
    }

    if (media_flags != ALARM_MEDIA_NONE && !ensure_worker_locked())
    {
        pthread_mutex_unlock(&g_mtx);
        cJSON_Delete(policy);
        return "";
    }
    std::string event_id = make_event_id(ctx->chnId);
    if (allow_merge)
    {
        ActiveEvent active;
        active.event_id = event_id;
        active.last_trigger_ms = now_ms;
        g_active[merge_key] = active;
    }
    pthread_mutex_unlock(&g_mtx);

    std::string dir = store_dir() + "/" + event_id;
    if (mkdir_p(dir) != 0)
    {
        clear_active_event(merge_key, event_id);
        cJSON_Delete(policy);
        return "";
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "event_id", event_id.c_str());
    cJSON_AddNumberToObject(root, "channel_id", ctx->chnId);
    cJSON_AddStringToObject(root, "alarm_type", input.type.c_str());
    cJSON_AddStringToObject(root, "record_kind",
                            input.record_kind.empty() ? "alarm" : input.record_kind.c_str());
    cJSON_AddStringToObject(root, "delivery_mode", input.dify_json_only ? "json_only" : "media");
    cJSON_AddStringToObject(root, "message", input.message.c_str());
    cJSON_AddNumberToObject(root, "ts", (double)time(nullptr));
    cJSON_AddNumberToObject(root, "trigger_unix_ms", (double)ctx->unix_ms);
    cJSON_AddNumberToObject(root, "last_trigger_unix_ms", (double)ctx->unix_ms);
    cJSON_AddNumberToObject(root, "trigger_count", 1);
    const std::string snap_time = ctx->time_str();
    cJSON_AddStringToObject(root, "snap_time", snap_time.c_str());
    cJSON_AddStringToObject(root, "end_time", snap_time.c_str());
    cJSON_AddItemToObject(root, "merged_triggers", cJSON_CreateArray());
    cJSON_AddStringToObject(root, "state",
                            media_flags == ALARM_MEDIA_NONE ? "pending" : "collecting");
    cJSON_AddItemToObject(root, "fields", fields_to_json(input.fields));
    cJSON_AddItemToObject(root, "channel_parameters", parse_object_or_empty(cfg.report_parameters_json));
    cJSON *media = cJSON_CreateObject();
    cJSON_AddStringToObject(media, "snapshot", "");
    cJSON_AddStringToObject(media, "raw", "");
    cJSON_AddStringToObject(media, "video", "");
    cJSON_AddItemToObject(root, "media", media);
    cJSON *requested = cJSON_CreateObject();
    cJSON_AddBoolToObject(requested, "image", (media_flags & ALARM_MEDIA_IMAGE) ? 1 : 0);
    cJSON_AddBoolToObject(requested, "video", (media_flags & ALARM_MEDIA_VIDEO) ? 1 : 0);
    cJSON_AddItemToObject(root, "media_requested", requested);
    add_delivery_statuses(policy, root);
    cJSON_AddItemToObject(root, "policy_snapshot", policy); /* ownership transferred */
    if (!atomic_write_json(dir + "/manifest.json", root))
    {
        cJSON_Delete(root);
        clear_active_event(merge_key, event_id);
        remove_tree(dir);
        return "";
    }
    cJSON_Delete(root);

    if ((media_flags & ALARM_MEDIA_IMAGE) && ctx->frame && !ctx->frame->empty())
    {
        ImageJob job;
        job.event_dir = dir;
        job.raw = ctx->frame->clone();
        job.overlay_mode = image_overlay;
        /* 无叠加时 snapshot/raw 共享同一不可变像素块，少一次 640x640 深拷贝。 */
        job.image = image_overlay == "none" ? job.raw : job.raw.clone();
        if (image_overlay != "none")
        {
            job.render_params = ctx->render_params();
            job.render_params.show_fps = 0;
            /* 上报图片自动复用实时画面的完整叠加层；同时允许开发者额外声明
             * IMAGE 专用指令。业务 logic 不需要再在原图上重复画框/文字。 */
            job.render_params.target_mask = static_cast<uint8_t>(
                DrawCommand::DISPLAY | DrawCommand::IMAGE);
            job.render_params.show_system_overlays = true;
            job.render_params.show_custom_overlays = true;
            if (ctx->rois) job.rois = *ctx->rois;
            if (ctx->results) job.results = *ctx->results;
            if (ctx->draw_cmds) job.commands = *ctx->draw_cmds;
            job.render_params.roi_zones = nullptr;
            job.render_params.results = nullptr;
            job.render_params.draw_cmds = nullptr;
        }
        pthread_mutex_lock(&g_mtx);
        g_image_jobs.push(std::move(job));
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mtx);
    }

    if (media_flags & ALARM_MEDIA_VIDEO)
    {
        EventVideoRequest vr;
        vr.event_id = event_id;
        vr.channel_id = ctx->chnId;
        vr.alarm_type = input.type;
        vr.pre_sec = video_pre_sec;
        vr.post_sec = video_post_sec;
        vr.fps = video_fps;
        vr.output_path = dir + "/clip.mp4";
        event_video_recorder_trigger(vr);
    }
    return event_id;
}

std::string report_alarm(ChannelContext *ctx,
                         const std::string &type,
                         const std::string &message,
                         std::initializer_list<AlarmField> fields)
{
    AlarmRequest request;
    request.type = type;
    request.message = message;
    for (const AlarmField &field : fields)
        request.fields.set_value(field.key, field.value);
    return alarm_report(ctx, request);
}

void alarm_report_video_ready(const std::string &event_id, const std::string &video_path)
{
    size_t slash = video_path.find_last_of('/');
    if (slash == std::string::npos) return;
    const std::string event_dir = video_path.substr(0, slash);
    size_t dir_slash = event_dir.find_last_of('/');
    const std::string dir_name = dir_slash == std::string::npos ? event_dir : event_dir.substr(dir_slash + 1);
    if (dir_name != event_id) return;
    update_media_ready(event_dir, "video", video_path.substr(slash + 1).c_str());
    enforce_outbox_cap();
}

void alarm_report_deinit(void)
{
    pthread_mutex_lock(&g_mtx);
    if (!g_worker_started) { pthread_mutex_unlock(&g_mtx); return; }
    g_running = false;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    pthread_join(g_worker_tid, nullptr);
    pthread_mutex_lock(&g_mtx);
    g_worker_started = false;
    g_active.clear();
    pthread_mutex_unlock(&g_mtx);
}
