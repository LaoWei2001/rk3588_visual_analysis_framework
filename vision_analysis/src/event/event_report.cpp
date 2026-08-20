#include "event_report.h"

#include "../config/config.h"
#include "../player/display.h"
#include "../recorder/event_video_recorder.h"
#include "../third_party/json/cJSON.h"
#include "logic/core/channel_logic.h"
#include "logic/core/global_logic.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <opencv2/imgcodecs.hpp>
#include <pthread.h>
#include <queue>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

enum EventMedia : uint32_t
{
    EVENT_MEDIA_NONE = 0,
    EVENT_MEDIA_IMAGE = 1u << 0,
    EVENT_MEDIA_VIDEO = 1u << 1
};

static const long long OUTBOX_CAP_BYTES = 1024LL * 1024LL * 1024LL;
static const long long OUTBOX_MIN_FREE_BYTES = 512LL * 1024LL * 1024LL;

struct ImageJob
{
    struct Pane
    {
        int channel_id = -1;
        cv::Mat frame;
        RenderParams render_params;
        std::vector<RoiZone> rois;
        std::vector<AlgoResult> results;
        std::vector<DrawCommand> commands;
    };

    std::string event_dir;
    cv::Mat annotated;
    cv::Mat raw;
    bool need_annotated = false;
    bool need_raw = false;
    std::string overlay_mode;
    RenderParams render_params;
    std::vector<RoiZone> rois;
    std::vector<AlgoResult> results;
    std::vector<DrawCommand> commands;
    int composite_width = 0;
    int composite_height = 0;
    int composite_rows = 0;
    int composite_cols = 0;
    std::vector<Pane> panes;
};

struct CompositeImage
{
    int width = 0;
    int height = 0;
    int rows = 0;
    int cols = 0;
    std::vector<ImageJob::Pane> panes;
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
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

static int mkdir_p(const std::string &path)
{
    if (path.empty())
        return -1;
    std::string cur;
    size_t start = 0;
    if (path[0] == '/')
    {
        cur = "/";
        start = 1;
    }
    while (start <= path.size())
    {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty())
        {
            if (!cur.empty() && cur.back() != '/')
                cur += '/';
            cur += part;
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                return -1;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return 0;
}

static std::string store_dir()
{
    const char *env = getenv("EVENT_STORE_DIR");
    std::string dir = (env && *env) ? env : "./event_store";
    mkdir_p(dir);
    return dir;
}

static long long directory_bytes(const std::string &dir)
{
    long long total = 0;
    DIR *dp = opendir(dir.c_str());
    if (!dp)
        return 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        std::string path = dir + "/" + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            continue;
        total += S_ISDIR(st.st_mode) ? directory_bytes(path) : (long long)st.st_size;
    }
    closedir(dp);
    return total;
}

static void remove_tree(const std::string &dir)
{
    DIR *dp = opendir(dir.c_str());
    if (!dp)
        return;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        std::string path = dir + "/" + entry->d_name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            remove_tree(path);
        else
            unlink(path.c_str());
    }
    closedir(dp);
    rmdir(dir.c_str());
}

static cJSON *read_json_file(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        return nullptr;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return cJSON_Parse(text.c_str());
}

static bool delivery_root_has_uploading(const cJSON *root)
{
    const cJSON *deliveries = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "deliveries") : nullptr;
    if (!cJSON_IsArray(deliveries))
        return false;
    const cJSON *delivery = nullptr;
    cJSON_ArrayForEach(delivery, deliveries)
    {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(delivery, "status");
        if (cJSON_IsString(status) && status->valuestring && strcmp(status->valuestring, "uploading") == 0)
            return true;
    }
    return false;
}

/* 容量回收只能删除静态待处理事件。状态文件无法读取/解析时采用保守策略，
 * 防止半写入文件或损坏文件被误判成可删除。新 schema 按文件划分所有者：
 * C++ 只改 event/media_state，Python 只改 delivery_state。 */
static bool event_has_active_work(const std::string &event_dir)
{
    const std::string event_path = event_dir + "/event.json";
    cJSON *event = read_json_file(event_path);
    cJSON *media = read_json_file(event_dir + "/media_state.json");
    cJSON *delivery = read_json_file(event_dir + "/delivery_state.json");
    if (!cJSON_IsObject(event) || !cJSON_IsObject(media) || !cJSON_IsObject(delivery))
    {
        fprintf(stderr, "[event_outbox] cannot read event state; skip eviction: %s\n", event_dir.c_str());
        cJSON_Delete(event);
        cJSON_Delete(media);
        cJSON_Delete(delivery);
        return true;
    }
    bool generating = false;
    const cJSON *media_entries = cJSON_GetObjectItemCaseSensitive(media, "media");
    const cJSON *media_entry = nullptr;
    cJSON_ArrayForEach(media_entry, media_entries)
    {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(media_entry, "status");
        if (cJSON_IsString(status) && status->valuestring &&
            (strcmp(status->valuestring, "requested") == 0 || strcmp(status->valuestring, "generating") == 0))
        {
            generating = true;
            break;
        }
    }
    const bool uploading = delivery_root_has_uploading(delivery);
    cJSON_Delete(event);
    cJSON_Delete(media);
    cJSON_Delete(delivery);
    return generating || uploading;
}

static void enforce_outbox_cap()
{
    const std::string root = store_dir();
    const char *cap_env = getenv("EVENT_STORE_MAX_BYTES");
    const char *free_env = getenv("EVENT_STORE_MIN_FREE_BYTES");
    const long long cap_bytes = cap_env && *cap_env ? atoll(cap_env) : OUTBOX_CAP_BYTES;
    const long long min_free_bytes = free_env && *free_env ? atoll(free_env) : OUTBOX_MIN_FREE_BYTES;
    struct statvfs fsinfo;
    long long free_bytes =
        statvfs(root.c_str(), &fsinfo) == 0 ? (long long)fsinfo.f_bavail * (long long)fsinfo.f_frsize : min_free_bytes;
    struct Candidate
    {
        std::string path;
        time_t mtime;
        long long bytes;
    };
    std::vector<Candidate> events;
    long long total = 0;
    DIR *dp = opendir(root.c_str());
    if (!dp)
        return;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        std::string path = root + "/" + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode) && access((path + "/event.json").c_str(), F_OK) == 0)
        {
            long long bytes = directory_bytes(path);
            total += bytes;
            if (!event_has_active_work(path))
                events.push_back({path, st.st_mtime, bytes});
        }
    }
    closedir(dp);
    std::sort(events.begin(), events.end(), [](const Candidate &a, const Candidate &b) { return a.mtime < b.mtime; });
    for (const auto &event : events)
    {
        if (total <= cap_bytes && free_bytes >= min_free_bytes)
            break;
        fprintf(stderr, "[event_outbox] cap exceeded; evict oldest pending event: %s\n", event.path.c_str());
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
    snprintf(id, sizeof(id), "ch%d_%s_%03ld_%lu", channel_id, stamp, ts.tv_nsec / 1000000L, ++g_seq);
    return id;
}

static cJSON *fields_to_json(const EventFields &fields)
{
    cJSON *obj = cJSON_CreateObject();
    for (const auto &entry : fields.values())
    {
        const EventValue &v = entry.second;
        if (v.type() == EventValue::NUMBER)
            cJSON_AddNumberToObject(obj, entry.first.c_str(), v.number());
        else if (v.type() == EventValue::BOOLEAN)
            cJSON_AddBoolToObject(obj, entry.first.c_str(), v.boolean() ? 1 : 0);
        else if (v.type() == EventValue::JSON)
        {
            cJSON *parsed = cJSON_Parse(v.text().c_str());
            if (parsed)
                cJSON_AddItemToObject(obj, entry.first.c_str(), parsed);
            else
                cJSON_AddStringToObject(obj, entry.first.c_str(), v.text().c_str());
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

static bool delivery_matches_event(const cJSON *delivery, const std::string &event_type)
{
    const cJSON *when = cJSON_GetObjectItemCaseSensitive(delivery, "when");
    if (!cJSON_IsObject(when))
        return true;
    const cJSON *types = cJSON_GetObjectItemCaseSensitive(when, "event_types");
    if (!cJSON_IsArray(types) || cJSON_GetArraySize(types) == 0)
        return true;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, types)
    {
        if (cJSON_IsString(item) && item->valuestring && event_type == item->valuestring)
            return true;
    }
    return false;
}

struct DeliveryRequirements
{
    int count = 0;
    uint32_t media_flags = EVENT_MEDIA_NONE;
    bool annotated_image = false;
    bool raw_image = false;
};

static DeliveryRequirements delivery_requirements(cJSON *policy, const std::string &event_type)
{
    DeliveryRequirements requirements;
    cJSON *deliveries = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    cJSON *delivery = nullptr;
    cJSON_ArrayForEach(delivery, deliveries)
    {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(delivery, "enabled");
        const cJSON *connection_id = cJSON_GetObjectItemCaseSensitive(delivery, "connection_id");
        const cJSON *contract_id = cJSON_GetObjectItemCaseSensitive(delivery, "contract_id");
        const cJSON *contract_revision = cJSON_GetObjectItemCaseSensitive(delivery, "contract_revision");
        const cJSON *media = cJSON_GetObjectItemCaseSensitive(delivery, "media");
        if (cJSON_IsFalse(enabled) || !cJSON_IsString(connection_id) || !connection_id->valuestring[0] ||
            !cJSON_IsString(contract_id) || !contract_id->valuestring[0] || !cJSON_IsString(contract_revision) ||
            !contract_revision->valuestring[0] || !cJSON_IsArray(media) ||
            !delivery_matches_event(delivery, event_type))
            continue;

        ++requirements.count;
        const cJSON *kind = nullptr;
        cJSON_ArrayForEach(kind, media)
        {
            if (!cJSON_IsString(kind) || !kind->valuestring)
                continue;
            if (strcmp(kind->valuestring, "annotated_image") == 0)
            {
                requirements.annotated_image = true;
                requirements.media_flags |= EVENT_MEDIA_IMAGE;
            }
            else if (strcmp(kind->valuestring, "raw_image") == 0)
            {
                requirements.raw_image = true;
                requirements.media_flags |= EVENT_MEDIA_IMAGE;
            }
            else if (strcmp(kind->valuestring, "video") == 0)
                requirements.media_flags |= EVENT_MEDIA_VIDEO;
        }
    }
    return requirements;
}

static void add_delivery_statuses(cJSON *policy, const std::string &event_type, cJSON *root)
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
            cJSON *connection_id = cJSON_GetObjectItemCaseSensitive(item, "connection_id");
            cJSON *contract_id = cJSON_GetObjectItemCaseSensitive(item, "contract_id");
            cJSON *contract_revision = cJSON_GetObjectItemCaseSensitive(item, "contract_revision");
            cJSON *media = cJSON_GetObjectItemCaseSensitive(item, "media");
            if (cJSON_IsFalse(enabled) || !cJSON_IsString(connection_id) || !connection_id->valuestring[0] ||
                !cJSON_IsString(contract_id) || !contract_id->valuestring[0] || !cJSON_IsString(contract_revision) ||
                !contract_revision->valuestring[0] || !cJSON_IsArray(media) ||
                !delivery_matches_event(item, event_type))
            {
                ++index;
                continue;
            }
            cJSON *copy = cJSON_Duplicate(item, 1);
            cJSON *id = cJSON_GetObjectItemCaseSensitive(copy, "id");
            if (!cJSON_IsString(id))
            {
                char generated[32];
                snprintf(generated, sizeof(generated), "delivery_%d", index);
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
    /* 事件 JSON 同时是运行时状态和板端排障入口。统一使用格式化 JSON，并在文件末尾
     * 保留换行；C++ 后续的媒体就绪、合并告警更新也继续走此函数。 */
    char *text = cJSON_Print(root);
    if (!text)
        return false;
    std::string tmp = path + ".tmp";
    FILE *fp = fopen(tmp.c_str(), "wb");
    const size_t text_len = strlen(text);
    bool ok = fp && fwrite(text, 1, text_len, fp) == text_len;
    if (ok && (text_len == 0 || text[text_len - 1] != '\n'))
        ok = fwrite("\n", 1, 1, fp) == 1;
    if (fp)
    {
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
    }
    cJSON_free(text);
    if (!ok)
    {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0)
    {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

static cJSON *read_event_document(const std::string &event_dir)
{
    return read_json_file(event_dir + "/event.json");
}

static bool record_merged_trigger(const std::string &event_id, uint64_t trigger_unix_ms,
                                  const std::string &trigger_time, const EventFields &fields)
{
    bool updated = false;
    const std::string event_dir = store_dir() + "/" + event_id;
    pthread_mutex_lock(&g_mtx);
    cJSON *root = read_event_document(event_dir);
    if (root)
    {
        cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (!cJSON_IsObject(event) || !cJSON_IsObject(data))
        {
            cJSON_Delete(root);
            pthread_mutex_unlock(&g_mtx);
            return false;
        }
        cJSON *count = cJSON_GetObjectItemCaseSensitive(event, "trigger_count");
        const int next_count = cJSON_IsNumber(count) ? count->valueint + 1 : 2;
        cJSON_ReplaceItemInObjectCaseSensitive(event, "trigger_count", cJSON_CreateNumber(next_count));
        cJSON_ReplaceItemInObjectCaseSensitive(event, "last_trigger_unix_ms",
                                               cJSON_CreateNumber((double)trigger_unix_ms));
        cJSON_ReplaceItemInObjectCaseSensitive(event, "end_time", cJSON_CreateString(trigger_time.c_str()));
        if (!fields.values().empty())
            cJSON_ReplaceItemInObjectCaseSensitive(data, "fields", fields_to_json(fields));
        cJSON *history = cJSON_GetObjectItemCaseSensitive(event, "merged_triggers");
        if (!cJSON_IsArray(history))
        {
            history = cJSON_CreateArray();
            cJSON_AddItemToObject(event, "merged_triggers", history);
        }
        cJSON *trigger = cJSON_CreateObject();
        cJSON_AddNumberToObject(trigger, "trigger_unix_ms", (double)trigger_unix_ms);
        cJSON_AddItemToObject(trigger, "fields", fields_to_json(fields));
        cJSON_AddItemToArray(history, trigger);
        updated = atomic_write_json(event_dir + "/event.json", root);
        cJSON_Delete(root);
    }
    pthread_mutex_unlock(&g_mtx);
    return updated;
}

static void clear_active_event(const std::string &merge_key, const std::string &event_id)
{
    pthread_mutex_lock(&g_mtx);
    auto found = g_active.find(merge_key);
    if (found != g_active.end() && found->second.event_id == event_id)
        g_active.erase(found);
    pthread_mutex_unlock(&g_mtx);
}

static void clear_active_event_by_id(const std::string &event_id)
{
    pthread_mutex_lock(&g_mtx);
    for (auto entry = g_active.begin(); entry != g_active.end();)
    {
        if (entry->second.event_id == event_id)
            entry = g_active.erase(entry);
        else
            ++entry;
    }
    pthread_mutex_unlock(&g_mtx);
}

static void refresh_media_status(cJSON *root)
{
    cJSON *media = cJSON_GetObjectItemCaseSensitive(root, "media");
    bool any = false;
    bool failed = false;
    bool generating = false;
    bool requested = false;
    bool all_ready = true;
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, media)
    {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(entry, "status");
        const char *value = cJSON_IsString(status) && status->valuestring ? status->valuestring : "";
        any = true;
        failed = failed || strcmp(value, "failed") == 0;
        generating = generating || strcmp(value, "generating") == 0;
        requested = requested || strcmp(value, "requested") == 0;
        all_ready = all_ready && strcmp(value, "ready") == 0;
    }

    const char *overall = !any || all_ready ? "ready"
                          : failed          ? "failed"
                          : generating      ? "generating"
                          : requested       ? "requested"
                                            : "failed";
    cJSON_ReplaceItemInObjectCaseSensitive(root, "status", cJSON_CreateString(overall));
}

static bool update_media_state(const std::string &event_dir, const char *kind, const char *status, const char *error,
                               std::initializer_list<std::pair<const char *, const char *>> files = {})
{
    bool updated = false;
    pthread_mutex_lock(&g_mtx);
    cJSON *root = read_json_file(event_dir + "/media_state.json");
    cJSON *media = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "media") : nullptr;
    cJSON *entry = cJSON_IsObject(media) ? cJSON_GetObjectItemCaseSensitive(media, kind) : nullptr;
    if (cJSON_IsObject(entry))
    {
        cJSON_ReplaceItemInObjectCaseSensitive(entry, "status", cJSON_CreateString(status));
        cJSON_ReplaceItemInObjectCaseSensitive(entry, "error", cJSON_CreateString(error ? error : ""));
        cJSON *stored_files = cJSON_GetObjectItemCaseSensitive(entry, "files");
        if (cJSON_IsObject(stored_files))
            for (const auto &file : files)
                cJSON_ReplaceItemInObjectCaseSensitive(stored_files, file.first, cJSON_CreateString(file.second));
        refresh_media_status(root);
        updated = atomic_write_json(event_dir + "/media_state.json", root);
    }
    cJSON_Delete(root);
    pthread_mutex_unlock(&g_mtx);
    return updated;
}

static cv::Rect composite_tile_rect(const ImageJob &job, size_t pane_index)
{
    if (job.composite_width <= 0 || job.composite_height <= 0 || job.composite_cols <= 0 || job.composite_rows <= 0)
        return cv::Rect();

    const int index = static_cast<int>(pane_index);
    const int col = index % job.composite_cols;
    const int row = index / job.composite_cols;
    if (row >= job.composite_rows)
        return cv::Rect();

    /* 与实时显示窗格使用相同的 4 像素宽 / 2 像素高边界对齐规则。这样即使显示宽高
     * 不能被行列数整除，上报图中每个窗格的位置和尺寸也仍与 HDMI/RTSP 一致。 */
    const int grid_width = job.composite_width / job.composite_cols;
    const int grid_height = job.composite_height / job.composite_rows;
    const int left = (col * grid_width) & ~3;
    const int top = (row * grid_height) & ~1;
    const int right = col == job.composite_cols - 1 ? job.composite_width & ~3 : ((col + 1) * grid_width) & ~3;
    const int bottom = row == job.composite_rows - 1 ? job.composite_height & ~1 : ((row + 1) * grid_height) & ~1;
    return right > left && bottom > top ? cv::Rect(left, top, right - left, bottom - top) : cv::Rect();
}

static void render_composite_image(ImageJob &job)
{
    if (job.panes.empty())
        return;

    if (job.need_raw)
        job.raw = cv::Mat::zeros(job.composite_height, job.composite_width, CV_8UC3);
    if (job.need_annotated)
        job.annotated = cv::Mat::zeros(job.composite_height, job.composite_width, CV_8UC3);

    for (size_t index = 0; index < job.panes.size(); ++index)
    {
        ImageJob::Pane &pane = job.panes[index];
        const cv::Rect tile = composite_tile_rect(job, index);
        if (pane.frame.empty() || tile.empty())
            continue; /* 与实时拼接一致：暂时无帧的已选通道保留黑色窗格。 */

        cv::Mat resized;
        cv::resize(pane.frame, resized, tile.size());
        if (job.need_raw)
            resized.copyTo(job.raw(tile));
        if (!job.need_annotated)
            continue;

        cv::Mat annotated_tile = job.annotated(tile);
        resized.copyTo(annotated_tile);
        if (job.overlay_mode == "none")
            continue;

        pane.render_params.roi_zones = &pane.rois;
        pane.render_params.results = &pane.results;
        pane.render_params.draw_cmds = &pane.commands;
        render_overlays(annotated_tile, pane.render_params);
    }
}

static void *image_worker(void *)
{
    while (true)
    {
        ImageJob job;
        pthread_mutex_lock(&g_mtx);
        while (g_image_jobs.empty() && g_running)
            pthread_cond_wait(&g_cv, &g_mtx);
        if (g_image_jobs.empty() && !g_running)
        {
            pthread_mutex_unlock(&g_mtx);
            break;
        }
        job = std::move(g_image_jobs.front());
        g_image_jobs.pop();
        pthread_mutex_unlock(&g_mtx);

        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        bool snapshot_ok = false;
        bool raw_ok = false;
        std::string write_error;
        try
        {
            if (!job.panes.empty())
            {
                render_composite_image(job);
            }
            else if (job.need_annotated && job.overlay_mode != "none" && !job.annotated.empty())
            {
                job.render_params.roi_zones = &job.rois;
                job.render_params.results = &job.results;
                job.render_params.draw_cmds = &job.commands;
                render_overlays(job.annotated, job.render_params);
            }
            snapshot_ok = !job.need_annotated || (!job.annotated.empty() &&
                                                  cv::imwrite(job.event_dir + "/annotated.jpg", job.annotated, params));
            raw_ok = !job.need_raw || (!job.raw.empty() && cv::imwrite(job.event_dir + "/raw.jpg", job.raw, params));
        }
        catch (const cv::Exception &exc)
        {
            write_error = exc.what();
        }
        if (snapshot_ok && raw_ok)
        {
            if (job.need_annotated && job.need_raw)
                update_media_state(job.event_dir, "image", "ready", "",
                                   {{"annotated_image", "annotated.jpg"}, {"raw_image", "raw.jpg"}});
            else if (job.need_annotated)
                update_media_state(job.event_dir, "image", "ready", "", {{"annotated_image", "annotated.jpg"}});
            else
                update_media_state(job.event_dir, "image", "ready", "", {{"raw_image", "raw.jpg"}});
        }
        else
        {
            const char *fallback = !snapshot_ok && !raw_ok ? "snapshot and raw image write failed"
                                   : !snapshot_ok          ? "snapshot image write failed"
                                                           : "raw image write failed";
            const char *reason = write_error.empty() ? fallback : write_error.c_str();
            if (snapshot_ok)
                update_media_state(job.event_dir, "image", "failed", reason, {{"annotated_image", "annotated.jpg"}});
            else if (raw_ok)
                update_media_state(job.event_dir, "image", "failed", reason, {{"raw_image", "raw.jpg"}});
            else
                update_media_state(job.event_dir, "image", "failed", reason);
            const size_t slash = job.event_dir.find_last_of('/');
            clear_active_event_by_id(slash == std::string::npos ? job.event_dir : job.event_dir.substr(slash + 1));
        }
        enforce_outbox_cap();
    }
    return nullptr;
}

static bool ensure_worker_locked()
{
    if (g_worker_started)
        return true;
    g_running = true;
    if (pthread_create(&g_worker_tid, nullptr, image_worker, nullptr) != 0)
    {
        g_running = false;
        return false;
    }
    g_worker_started = true;
    return true;
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

} // namespace

const char *event_report_status_name(EventReportStatus status)
{
    switch (status)
    {
    case EventReportStatus::CREATED:
        return "created";
    case EventReportStatus::MERGED:
        return "merged";
    case EventReportStatus::CREATED_MEDIA_FAILED:
        return "created_media_failed";
    case EventReportStatus::DISABLED:
        return "disabled";
    case EventReportStatus::INVALID_REQUEST:
        return "invalid_request";
    case EventReportStatus::NO_DELIVERY:
        return "no_delivery";
    case EventReportStatus::WORKER_UNAVAILABLE:
        return "worker_unavailable";
    case EventReportStatus::STORAGE_ERROR:
        return "storage_error";
    }
    return "unknown";
}

static EventReportResult report_event_impl(ChannelContext *ctx, const EventRequest &input,
                                           const CompositeImage *composite_image,
                                           int video_source_channel_id = -1)
{
    auto result = [](EventReportStatus status, const std::string &event_id, const std::string &detail) {
        EventReportResult value;
        value.status = status;
        value.event_id = event_id;
        value.detail = detail;
        return value;
    };

    if (!ctx || !ctx->config || input.event_type.empty())
        return result(EventReportStatus::INVALID_REQUEST, "", "ctx/config is null or event type is empty");
    enforce_outbox_cap();
    const ChannelConfig &cfg = *ctx->config;
    cJSON *policy = parse_object_or_empty(cfg.report_policy_json);
    if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(policy, "enabled")))
    {
        cJSON_Delete(policy);
        return result(EventReportStatus::DISABLED, "", "report policy is disabled");
    }
    cJSON *configured_deliveries = cJSON_GetObjectItemCaseSensitive(policy, "deliveries");
    if (!cJSON_IsArray(configured_deliveries) || cJSON_GetArraySize(configured_deliveries) == 0)
    {
        cJSON_Delete(policy);
        return result(EventReportStatus::NO_DELIVERY, "", "report policy has no deliveries");
    }

    const DeliveryRequirements requirements = delivery_requirements(policy, input.event_type);
    if (requirements.count == 0)
    {
        cJSON_Delete(policy);
        return result(EventReportStatus::NO_DELIVERY, "", "no enabled delivery matches this event type");
    }
    const uint32_t media_flags = requirements.media_flags;
    const int resolved_video_source_channel_id =
        video_source_channel_id >= 0 ? video_source_channel_id : ctx->chnId;
    if ((media_flags & EVENT_MEDIA_VIDEO) && !app_ctrl_has_channel(resolved_video_source_channel_id))
    {
        cJSON_Delete(policy);
        return result(EventReportStatus::INVALID_REQUEST, "", "event video source channel is not available");
    }

    float merge_sec = (float)policy_number(policy, "merge_window_sec", 5.0);
    merge_sec = std::max(0.0f, std::min(60.0f, merge_sec));
    const float video_pre_sec = cfg.event_video.pre_sec;
    const float video_post_sec = cfg.event_video.post_sec;
    const int video_fps = cfg.event_video.fps;
    const std::string image_overlay = policy_string(policy, "image_overlay", "custom");
    std::string merge_key = std::to_string(ctx->chnId) + ":" + input.event_type;
    uint64_t now_ms = steady_now_ms();
    const bool allow_merge = input.merge_mode == EventMergeMode::POLICY && merge_sec > 0.0f;

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
            if (media_flags & EVENT_MEDIA_VIDEO)
                event_video_recorder_extend(existing);
            cJSON_Delete(policy);
            return result(EventReportStatus::MERGED, existing, "merged into active event");
        }
        pthread_mutex_lock(&g_mtx);
        auto stale = g_active.find(merge_key);
        if (stale != g_active.end() && stale->second.event_id == existing)
            g_active.erase(stale);
    }

    if ((media_flags & EVENT_MEDIA_IMAGE) && !ensure_worker_locked())
    {
        pthread_mutex_unlock(&g_mtx);
        cJSON_Delete(policy);
        return result(EventReportStatus::WORKER_UNAVAILABLE, "", "image worker could not start");
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
        return result(EventReportStatus::STORAGE_ERROR, "", "event directory could not be created");
    }

    /*
     * schema v3 按写入者拆分状态，避免 C++ 和 Python 对同一 JSON 做读改写：
     *   event.json          C++ 拥有事件、来源、业务字段和策略快照
     *   media_state.json    C++ 图片/录像模块拥有
     *   delivery_state.json Python 上传服务在初始化后独占
     * event.json 最后落盘，兼作“事件目录创建完成”的可见标记。
     */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", 3);

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "id", event_id.c_str());
    cJSON_AddStringToObject(event, "type", input.event_type.c_str());
    cJSON_AddStringToObject(event, "message", input.message.c_str());
    cJSON_AddNumberToObject(event, "created_unix_sec", (double)time(nullptr));
    cJSON_AddNumberToObject(event, "trigger_unix_ms", (double)ctx->unix_ms);
    cJSON_AddNumberToObject(event, "last_trigger_unix_ms", (double)ctx->unix_ms);
    cJSON_AddNumberToObject(event, "trigger_count", 1);
    const std::string snap_time = ctx->time_str();
    cJSON_AddStringToObject(event, "snap_time", snap_time.c_str());
    cJSON_AddStringToObject(event, "end_time", snap_time.c_str());
    cJSON_AddItemToObject(event, "merged_triggers", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "event", event);

    cJSON *source = cJSON_CreateObject();
    cJSON_AddNumberToObject(source, "channel_id", ctx->chnId);
    if (media_flags & EVENT_MEDIA_VIDEO)
        cJSON_AddNumberToObject(source, "video_channel_id", resolved_video_source_channel_id);
    cJSON_AddItemToObject(source, "parameters", parse_object_or_empty(cfg.report_parameters_json));
    cJSON_AddItemToObject(root, "source", source);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "fields", fields_to_json(input.fields));
    cJSON_AddItemToObject(root, "data", data);

    cJSON *media_root = cJSON_CreateObject();
    cJSON_AddNumberToObject(media_root, "schema_version", 3);
    cJSON_AddStringToObject(media_root, "status", media_flags == EVENT_MEDIA_NONE ? "ready" : "requested");
    cJSON *media_entries = cJSON_CreateObject();
    if (media_flags & EVENT_MEDIA_IMAGE)
    {
        cJSON *image = cJSON_CreateObject();
        cJSON_AddStringToObject(image, "status", "requested");
        cJSON_AddStringToObject(image, "error", "");
        cJSON *files = cJSON_CreateObject();
        if (requirements.annotated_image)
            cJSON_AddStringToObject(files, "annotated_image", "");
        if (requirements.raw_image)
            cJSON_AddStringToObject(files, "raw_image", "");
        cJSON_AddItemToObject(image, "files", files);
        cJSON_AddItemToObject(media_entries, "image", image);
    }
    if (media_flags & EVENT_MEDIA_VIDEO)
    {
        cJSON *video = cJSON_CreateObject();
        cJSON_AddStringToObject(video, "status", "requested");
        cJSON_AddStringToObject(video, "error", "");
        cJSON *files = cJSON_CreateObject();
        cJSON_AddStringToObject(files, "video", "");
        cJSON_AddItemToObject(video, "files", files);
        cJSON_AddItemToObject(media_entries, "video", video);
    }
    cJSON_AddItemToObject(media_root, "media", media_entries);

    cJSON *delivery_root = cJSON_CreateObject();
    cJSON_AddNumberToObject(delivery_root, "schema_version", 3);
    add_delivery_statuses(policy, input.event_type, delivery_root);
    cJSON_AddItemToObject(root, "policy_snapshot", policy); /* ownership transferred */

    const bool stored = atomic_write_json(dir + "/delivery_state.json", delivery_root) &&
                        atomic_write_json(dir + "/media_state.json", media_root) &&
                        atomic_write_json(dir + "/event.json", root);
    cJSON_Delete(delivery_root);
    cJSON_Delete(media_root);
    cJSON_Delete(root);
    if (!stored)
    {
        clear_active_event(merge_key, event_id);
        remove_tree(dir);
        return result(EventReportStatus::STORAGE_ERROR, "", "event state files could not be written");
    }

    std::vector<std::string> media_errors;
    if (media_flags & EVENT_MEDIA_IMAGE)
    {
        const bool has_composite_frame =
            composite_image && std::any_of(composite_image->panes.begin(), composite_image->panes.end(),
                                           [](const ImageJob::Pane &pane) { return !pane.frame.empty(); });
        if ((!composite_image && (!ctx->frame || ctx->frame->empty())) || (composite_image && !has_composite_frame))
        {
            const char *reason = composite_image ? "all selected channel frames are empty" : "current frame is empty";
            update_media_state(dir, "image", "failed", reason);
            media_errors.push_back(std::string("image: ") + reason);
        }
        else
        {
            update_media_state(dir, "image", "generating", "");
            ImageJob job;
            job.event_dir = dir;
            job.need_annotated = requirements.annotated_image;
            job.need_raw = requirements.raw_image;
            job.overlay_mode = image_overlay;
            if (composite_image)
            {
                job.composite_width = composite_image->width;
                job.composite_height = composite_image->height;
                job.composite_rows = composite_image->rows;
                job.composite_cols = composite_image->cols;
                job.panes = composite_image->panes;
            }
            else
            {
                job.raw = ctx->frame->clone();
                /* 无叠加时 annotated/raw 共享同一不可变像素块，少一次深拷贝。 */
                if (job.need_annotated)
                    job.annotated = image_overlay == "none" ? job.raw : job.raw.clone();
                if (job.need_annotated && image_overlay != "none")
                {
                    job.render_params = ctx->render_params();
                    job.render_params.show_fps = 0;
                    /* 上报图片自动复用实时画面的完整叠加层；同时允许开发者额外声明
                     * IMAGE 专用指令。业务 logic 不需要再在原图上重复画框/文字。 */
                    job.render_params.target_mask = static_cast<uint8_t>(DrawCommand::DISPLAY | DrawCommand::IMAGE);
                    job.render_params.show_system_overlays = true;
                    job.render_params.show_custom_overlays = true;
                    if (ctx->rois)
                        job.rois = *ctx->rois;
                    if (ctx->results)
                        job.results = *ctx->results;
                    if (ctx->draw_cmds)
                        job.commands = *ctx->draw_cmds;
                    job.render_params.roi_zones = nullptr;
                    job.render_params.results = nullptr;
                    job.render_params.draw_cmds = nullptr;
                }
            }
            pthread_mutex_lock(&g_mtx);
            g_image_jobs.push(std::move(job));
            pthread_cond_signal(&g_cv);
            pthread_mutex_unlock(&g_mtx);
        }
    }

    if (media_flags & EVENT_MEDIA_VIDEO)
    {
        EventVideoRequest vr;
        vr.event_id = event_id;
        vr.channel_id = resolved_video_source_channel_id;
        vr.event_type = input.event_type;
        vr.pre_sec = video_pre_sec;
        vr.post_sec = video_post_sec;
        vr.fps = video_fps;
        vr.output_path = dir + "/clip.mp4";
        update_media_state(dir, "video", "generating", "");
        if (!event_video_recorder_trigger(vr))
        {
            update_media_state(dir, "video", "failed", "event video recorder rejected request");
            media_errors.push_back("video: recorder rejected request");
        }
    }
    if (!media_errors.empty())
    {
        clear_active_event_by_id(event_id);
        std::string detail;
        for (const std::string &error : media_errors)
        {
            if (!detail.empty())
                detail += "; ";
            detail += error;
        }
        return result(EventReportStatus::CREATED_MEDIA_FAILED, event_id, detail);
    }
    return result(EventReportStatus::CREATED, event_id, "event created");
}

EventReportResult report_event(ChannelContext *ctx, const EventRequest &request)
{
    return report_event_impl(ctx, request, nullptr);
}

EventReportResult report_event(GlobalContext *gctx, const EventRequest &request)
{
    auto invalid = [](const std::string &detail) {
        EventReportResult value;
        value.status = EventReportStatus::INVALID_REQUEST;
        value.detail = detail;
        return value;
    };

    if (!gctx || !gctx->config)
        return invalid("global ctx/config is null");

    int source_channel_id = request.source_channel_id;
    if (source_channel_id < 0)
        source_channel_id = gctx->config->media_source_channel_id;
    if (source_channel_id < 0 && gctx->connected_channel_count() > 0)
    {
        const ChannelLogicSnapshot *first_connected = gctx->connected_channel_at(0);
        if (first_connected)
            source_channel_id = first_connected->channel_id;
    }
    if (source_channel_id < 0 && gctx->channel_count() > 0)
        source_channel_id = gctx->channel_at(0)->channel_id;

    const ChannelLogicSnapshot *source_logic = gctx->channel(source_channel_id);
    if (source_channel_id < 0 || !source_logic)
        return invalid("global event source channel is not available in this application");

    auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *source_config = app_ctrl_runtime_channel_config(runtime, source_channel_id);
    if (!runtime || !source_config)
        return invalid("global event media source channel is not configured");

    cJSON *policy = parse_object_or_empty(gctx->config->report_policy_json);
    const DeliveryRequirements requirements = delivery_requirements(policy, request.event_type);
    cJSON_Delete(policy);
    const bool need_image = (requirements.media_flags & EVENT_MEDIA_IMAGE) != 0;

    CompositeImage composite;
    ChannelFrameSnapshot source_frame;
    bool source_frame_loaded = false;

    auto capture_exact = [&](int channel_id, ChannelFrameSnapshot *out) -> bool {
        const ChannelLogicSnapshot *expected = gctx->channel(channel_id);
        if (!expected || !app_ctrl_get_channel_frame_snapshot(channel_id, out))
            return false;
        return out->logic.publication_seq == expected->publication_seq;
    };

    if (need_image)
    {
        std::vector<int> image_channel_ids;
        if (gctx->connected_channel_ids && !gctx->connected_channel_ids->empty())
            image_channel_ids = *gctx->connected_channel_ids;
        else
            image_channel_ids.push_back(source_channel_id);

        composite.width = runtime->config.disp_width & ~3;
        composite.height = runtime->config.disp_height & ~1;
        composite.cols = std::max(1, std::min(runtime->config.tile_cols,
                                              static_cast<int>(image_channel_ids.size())));
        composite.rows = std::max(1, (static_cast<int>(image_channel_ids.size()) + composite.cols - 1) /
                                         composite.cols);
        composite.panes.reserve(image_channel_ids.size());

        for (int channel_id : image_channel_ids)
        {
            ChannelFrameSnapshot snapshot;
            if (!capture_exact(channel_id, &snapshot))
                return invalid("global event channel changed before its image snapshot was captured");

            ImageJob::Pane pane;
            pane.channel_id = channel_id;
            pane.frame = snapshot.frame;
            pane.results = snapshot.results;
            pane.rois = snapshot.rois;
            pane.commands = snapshot.draw_cmds;
            pane.render_params.chnId = channel_id;
            pane.render_params.inputW = pane.frame.cols;
            pane.render_params.inputH = pane.frame.rows;
            pane.render_params.disp_fps = snapshot.logic.disp_fps;
            pane.render_params.infer_fps = snapshot.logic.infer_fps;
            pane.render_params.result_frame_id = snapshot.logic.frame_seq;
            pane.render_params.result_age_ms = std::max<int64_t>(0, snapshot.logic.publication_age_ms);
            pane.render_params.show_fps = 0;
            pane.render_params.target_mask = static_cast<uint8_t>(DrawCommand::DISPLAY | DrawCommand::IMAGE);
            pane.render_params.show_system_overlays = true;
            pane.render_params.show_custom_overlays = true;
            if (channel_id == source_channel_id)
            {
                source_frame = snapshot;
                source_frame_loaded = true;
            }
            composite.panes.push_back(std::move(pane));
        }

        if (!source_frame_loaded)
        {
            if (!capture_exact(source_channel_id, &source_frame))
                return invalid("global event source channel changed before its image snapshot was captured");
            source_frame_loaded = true;
        }
    }

    /* report_policy/report_parameters 来自全局节点；来源通道可以由 C++、参数或画布决定。 */
    ChannelConfig report_config = *source_config;
    report_config.report_policy_json = gctx->config->report_policy_json;
    report_config.report_parameters_json = gctx->config->report_parameters_json;
    report_config.event_video = gctx->config->event_video;

    ChannelContext ctx{};
    ctx.chnId = source_channel_id;
    ctx.frame = source_frame_loaded ? &source_frame.frame : nullptr;
    ctx.frame_id = source_frame_loaded ? source_frame.logic.frame_seq : source_logic->frame_seq;
    ctx.timestamp_ms = source_frame_loaded && source_frame.logic.frame_steady_ms != 0
                           ? source_frame.logic.frame_steady_ms
                           : (source_logic->frame_steady_ms != 0 ? source_logic->frame_steady_ms : gctx->steady_ms);
    ctx.unix_ms = source_frame_loaded && source_frame.logic.frame_unix_ms != 0
                      ? source_frame.logic.frame_unix_ms
                      : (source_logic->frame_unix_ms != 0 ? source_logic->frame_unix_ms : gctx->unix_ms);
    ctx.results = source_frame_loaded ? &source_frame.results : nullptr;
    ctx.config = &report_config;
    ctx.rois = source_frame_loaded ? &source_frame.rois : nullptr;
    ctx.draw_cmds = source_frame_loaded ? &source_frame.draw_cmds : nullptr;
    ctx.infer_fps = source_frame_loaded ? source_frame.logic.infer_fps : source_logic->infer_fps;
    ctx.disp_fps = source_frame_loaded ? source_frame.logic.disp_fps : source_logic->disp_fps;
    return report_event_impl(&ctx, request,
                             need_image && gctx->connected_channel_count() > 0 ? &composite : nullptr,
                             gctx->config->media_source_channel_id);
}

void event_report_video_ready(const std::string &event_id, const std::string &video_path)
{
    size_t slash = video_path.find_last_of('/');
    if (slash == std::string::npos)
        return;
    const std::string event_dir = video_path.substr(0, slash);
    size_t dir_slash = event_dir.find_last_of('/');
    const std::string dir_name = dir_slash == std::string::npos ? event_dir : event_dir.substr(dir_slash + 1);
    if (dir_name != event_id)
        return;
    const std::string filename = video_path.substr(slash + 1);
    update_media_state(event_dir, "video", "ready", "", {{"video", filename.c_str()}});
    enforce_outbox_cap();
}

void event_report_video_failed(const std::string &event_id, const std::string &video_path, const std::string &reason)
{
    size_t slash = video_path.find_last_of('/');
    if (slash == std::string::npos)
        return;
    const std::string event_dir = video_path.substr(0, slash);
    size_t dir_slash = event_dir.find_last_of('/');
    const std::string dir_name = dir_slash == std::string::npos ? event_dir : event_dir.substr(dir_slash + 1);
    if (dir_name != event_id)
        return;
    update_media_state(event_dir, "video", "failed", reason.c_str());
    clear_active_event_by_id(event_id);
    enforce_outbox_cap();
}

void event_report_deinit(void)
{
    pthread_mutex_lock(&g_mtx);
    if (!g_worker_started)
    {
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    g_running = false;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    pthread_join(g_worker_tid, nullptr);
    pthread_mutex_lock(&g_mtx);
    g_worker_started = false;
    g_active.clear();
    pthread_mutex_unlock(&g_mtx);
}
