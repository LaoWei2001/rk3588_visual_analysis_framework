#include "event_report_internal.h"
#include "event_storage.h"
#include <chrono>
#include <cstdio>
#include <utility>

namespace event_report_detail
{
pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker_tid;
static bool g_worker_started = false;
bool g_running = false;
std::queue<ImageJob> g_image_jobs;
pthread_mutex_t g_persist_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_persist_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_persist_tid;
static bool g_persist_started = false;
static bool g_persist_running = false;
std::queue<PersistenceJob> g_persist_jobs;
std::map<std::string, ActiveEvent> g_active; /* channel:type/dedup → event */
static std::atomic<unsigned long> g_failures{0};

void record_failure(const char *operation, const std::string &path)
{
    g_failures.fetch_add(1, std::memory_order_relaxed);
    fprintf(stderr, "[event_outbox] incomplete evidence: %s (%s)\n", operation, path.c_str());
}

uint64_t steady_now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void clear_active_event(const std::string &merge_key, const std::string &event_id)
{
    pthread_mutex_lock(&g_mtx);
    auto found = g_active.find(merge_key);
    if (found != g_active.end() && found->second.event_id == event_id)
        g_active.erase(found);
    pthread_mutex_unlock(&g_mtx);
}

void clear_active_event_by_id(const std::string &event_id)
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

void *persistence_worker(void *)
{
    while (true)
    {
        PersistenceJob job;
        pthread_mutex_lock(&g_persist_mtx);
        while (g_persist_jobs.empty() && g_persist_running)
            pthread_cond_wait(&g_persist_cv, &g_persist_mtx);
        if (g_persist_jobs.empty() && !g_persist_running)
        {
            pthread_mutex_unlock(&g_persist_mtx);
            break;
        }
        job = std::move(g_persist_jobs.front());
        g_persist_jobs.pop();
        pthread_mutex_unlock(&g_persist_mtx);

        if (job.type == PersistenceJob::MERGE)
        {
            if (record_merged_trigger(job.event_id, job.trigger_unix_ms, job.trigger_time, job.fields))
            {
                if (job.extend_video)
                    event_video_recorder_extend(job.event_id);
            }
            else
            {
                fprintf(stderr, "[event_outbox] merge persistence failed: %s\n", job.event_id.c_str());
                record_failure("merge persistence failed", job.event_id);
                clear_active_event_by_id(job.event_id);
            }
            continue;
        }

        const bool stored = mkdir_p(job.event_dir) == 0 && event_storage::sync_parent(job.event_dir) &&
                            atomic_write_text(job.event_dir + "/delivery_state.json", job.delivery_json) &&
                            atomic_write_text(job.event_dir + "/media_state.json", job.media_json) &&
                            atomic_write_text(job.event_dir + "/event.json", job.event_json);
        if (!stored)
        {
            fprintf(stderr, "[event_outbox] event persistence failed: %s\n", job.event_id.c_str());
            record_failure("event persistence failed", job.event_dir);
            clear_active_event(job.merge_key, job.event_id);
            remove_tree(job.event_dir);
            continue;
        }
        fprintf(stdout, "[event_outbox] persisted event: %s\n", job.event_id.c_str());

        if (job.has_image_job)
        {
            pthread_mutex_lock(&g_mtx);
            g_image_jobs.push(std::move(job.image_job));
            pthread_cond_signal(&g_cv);
            pthread_mutex_unlock(&g_mtx);
        }

        if (job.has_video_job && !event_video_recorder_trigger(job.video_job))
        {
            record_failure("video recorder rejected request", job.event_dir);
            update_media_state(job.event_dir, "video", "failed", "event video recorder rejected request");
            clear_active_event_by_id(job.event_id);
            fprintf(stderr, "[event_outbox] video recorder rejected event: %s\n", job.event_id.c_str());
        }
        enforce_outbox_cap();
    }
    return nullptr;
}

bool ensure_workers_locked()
{
    if (!g_persist_started)
    {
        pthread_mutex_lock(&g_persist_mtx);
        g_persist_running = true;
        pthread_mutex_unlock(&g_persist_mtx);
        if (pthread_create(&g_persist_tid, nullptr, persistence_worker, nullptr) != 0)
        {
            pthread_mutex_lock(&g_persist_mtx);
            g_persist_running = false;
            pthread_mutex_unlock(&g_persist_mtx);
            return false;
        }
        g_persist_started = true;
    }

    if (!g_worker_started)
    {
        g_running = true;
        if (pthread_create(&g_worker_tid, nullptr, image_worker, nullptr) != 0)
        {
            g_running = false;
            return false;
        }
        g_worker_started = true;
    }
    return true;
}

} // namespace event_report_detail

using namespace event_report_detail;

void event_report_deinit(void)
{
    pthread_mutex_lock(&g_mtx);
    const bool join_persist = g_persist_started;
    pthread_mutex_unlock(&g_mtx);
    pthread_mutex_lock(&g_persist_mtx);
    g_persist_running = false;
    pthread_cond_broadcast(&g_persist_cv);
    pthread_mutex_unlock(&g_persist_mtx);
    if (join_persist)
        pthread_join(g_persist_tid, nullptr);

    pthread_mutex_lock(&g_mtx);
    const bool join_image = g_worker_started;
    g_running = false;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    if (join_image)
        pthread_join(g_worker_tid, nullptr);

    pthread_mutex_lock(&g_mtx);
    g_worker_started = false;
    g_persist_started = false;
    g_active.clear();
    pthread_mutex_unlock(&g_mtx);
}

unsigned long event_report_failure_count()
{
    return g_failures.load(std::memory_order_relaxed);
}
