#include "event_report_internal.h"
#include "event_retention.h"
#include "event_storage.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace event_report_detail
{
static const long long OUTBOX_CAP_BYTES = 1024LL * 1024LL * 1024LL;
static const long long OUTBOX_MIN_FREE_BYTES = 512LL * 1024LL * 1024LL;

static pthread_mutex_t g_file_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_seq = 0;

int mkdir_p(const std::string &path)
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

std::string store_dir_path()
{
    const char *env = getenv("EVENT_STORE_DIR");
    return (env && *env) ? env : "./event_store";
}

std::string store_dir()
{
    std::string dir = store_dir_path();
    mkdir_p(dir);
    return dir;
}

long long directory_bytes(const std::string &dir)
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

void remove_tree(const std::string &dir)
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

cJSON *read_json_file(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        return nullptr;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return cJSON_Parse(text.c_str());
}

/* 容量回收只能删除投递完成且媒体不再生成的事件。状态文件无法读取/解析时采用保守策略，
 * 防止半写入文件或损坏文件被误判成可删除。新 schema 按文件划分所有者：
 * C++ 只改 event/media_state，Python 只改 delivery_state。 */
bool event_has_active_work(const std::string &event_dir)
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
    const bool undelivered = event_deliveries_need_retention(delivery);
    cJSON_Delete(event);
    cJSON_Delete(media);
    cJSON_Delete(delivery);
    return generating || undelivered;
}

void enforce_outbox_cap()
{
    static std::mutex cap_mutex;
    const std::lock_guard<std::mutex> lock(cap_mutex);
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
        fprintf(stderr, "[event_outbox] storage limit reached; reclaim completed event: %s\n", event.path.c_str());
        remove_tree(event.path);
        total -= event.bytes;
        free_bytes += event.bytes;
    }
    if (total > cap_bytes || free_bytes < min_free_bytes)
    {
        static uint64_t last_warning_ms = 0;
        const uint64_t now_ms = steady_now_ms();
        if (last_warning_ms == 0 || now_ms - last_warning_ms >= 30000)
        {
            fprintf(stderr,
                    "[event_outbox] storage pressure: retained undelivered events; "
                    "used=%lld cap=%lld free=%lld min_free=%lld\n",
                    total, cap_bytes, free_bytes, min_free_bytes);
            last_warning_ms = now_ms;
        }
    }
}

std::string event_scope_token(const std::string &scope)
{
    std::string token = scope.empty() ? "event" : scope;
    for (char &ch : token)
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') && ch != '-' &&
            ch != '_')
            ch = '_';
    return token;
}

std::string make_event_id(const std::string &scope)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    time_t sec = ts.tv_sec;
    localtime_r(&sec, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    char id[128];
    snprintf(id, sizeof(id), "%s_%s_%03ld_%lu", event_scope_token(scope).c_str(), stamp, ts.tv_nsec / 1000000L,
             ++g_seq);
    return id;
}

bool atomic_write_json(const std::string &path, cJSON *root)
{
    /* 事件 JSON 同时是运行时状态和板端排障入口。统一使用格式化 JSON，并在文件末尾
     * 保留换行；C++ 后续的媒体就绪、合并告警更新也继续走此函数。 */
    char *text = cJSON_Print(root);
    if (!text)
        return false;
    std::string content(text);
    cJSON_free(text);
    if (content.empty() || content.back() != '\n')
        content.push_back('\n');
    return event_storage::write_atomic(path, content.data(), content.size());
}

bool json_to_text(cJSON *root, std::string &out)
{
    char *text = cJSON_Print(root);
    if (!text)
        return false;
    out.assign(text);
    if (out.empty() || out.back() != '\n')
        out.push_back('\n');
    cJSON_free(text);
    return true;
}

bool atomic_write_text(const std::string &path, const std::string &text)
{
    return event_storage::write_atomic(path, text.data(), text.size());
}

cJSON *read_event_document(const std::string &event_dir)
{
    return read_json_file(event_dir + "/event.json");
}

bool record_merged_trigger(const std::string &event_id, uint64_t trigger_unix_ms, const std::string &trigger_time,
                           const EventFields &fields)
{
    bool updated = false;
    const std::string event_dir = store_dir() + "/" + event_id;
    pthread_mutex_lock(&g_file_mtx);
    cJSON *root = read_event_document(event_dir);
    if (root)
    {
        cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (!cJSON_IsObject(event) || !cJSON_IsObject(data))
        {
            cJSON_Delete(root);
            pthread_mutex_unlock(&g_file_mtx);
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
    pthread_mutex_unlock(&g_file_mtx);
    return updated;
}

void refresh_media_status(cJSON *root)
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

bool update_media_state(const std::string &event_dir, const char *kind, const char *status, const char *error,
                        std::initializer_list<std::pair<const char *, const char *>> files)
{
    bool updated = false;
    pthread_mutex_lock(&g_file_mtx);
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
    pthread_mutex_unlock(&g_file_mtx);
    if (!updated)
        record_failure("media state update failed", event_dir);
    return updated;
}

} // namespace event_report_detail

using namespace event_report_detail;

bool event_report_video_ready(const std::string &event_id, const std::string &video_path)
{
    size_t slash = video_path.find_last_of('/');
    if (slash == std::string::npos)
    {
        record_failure("invalid video output path", video_path);
        return false;
    }
    const std::string event_dir = video_path.substr(0, slash);
    size_t dir_slash = event_dir.find_last_of('/');
    const std::string dir_name = dir_slash == std::string::npos ? event_dir : event_dir.substr(dir_slash + 1);
    if (dir_name != event_id)
    {
        record_failure("video event identity mismatch", video_path);
        return false;
    }
    if (!event_storage::sync_file(video_path))
    {
        event_report_video_failed(event_id, video_path, "video flush/fsync/close failed");
        return false;
    }
    const std::string filename = video_path.substr(slash + 1);
    const bool stored = update_media_state(event_dir, "video", "ready", "", {{"video", filename.c_str()}});
    enforce_outbox_cap();
    return stored;
}

void event_report_video_failed(const std::string &event_id, const std::string &video_path, const std::string &reason)
{
    record_failure(reason.c_str(), video_path);
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
