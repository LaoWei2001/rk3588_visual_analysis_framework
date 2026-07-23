/**
 * @file global_logic.cpp
 * @brief 全局逻辑模块 — C/pthread 风格
 *
 * 每个 GlobalLogicConfig 实例对应一个独立 pthread.
 * 线程由 global_logic_start_all (初始/热重载) 创建和管理.
 */

#include "global_logic.h"
#include "core/app_ctrl.h"
#include "core/pause_ctrl.h"
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <pthread.h>

/*======================== 单个实例的线程上下文 ========================*/
struct GlobalLogicThread
{
    GlobalLogicConfig config;
    pthread_t          tid;
    std::atomic<bool>   running{false};
    std::atomic<bool>   stop_requested{false};
    std::shared_ptr<void> state;
    int64_t            tick_id;

    std::vector<std::vector<AlgoResult>> results_cache;
    std::vector<int> channel_ids;
    std::vector<uint64_t> last_infer_ts_ms;

    GlobalContext gctx;
    GlobalLogicFunc func;
};

static std::vector<GlobalLogicThread*> g_threads;
static pthread_mutex_t g_threads_mtx = PTHREAD_MUTEX_INITIALIZER;

/*======================== 全局逻辑分发表 ========================*/
#define MAX_GLOBAL_LOGICS 16
static struct { const char *name; GlobalLogicFunc func; } g_logic_map[MAX_GLOBAL_LOGICS];
static int g_logic_map_count = 0;

static void global_default(GlobalContext *gctx);

static void register_global_logic(const char *name, GlobalLogicFunc func)
{
    if (g_logic_map_count < MAX_GLOBAL_LOGICS)
    {
        g_logic_map[g_logic_map_count].name = name;
        g_logic_map[g_logic_map_count].func = func;
        g_logic_map_count++;
    }
}

static void global_logic_register(void)
{
    g_logic_map_count = 0;
    register_global_logic("global_default", global_default);

    /* 新增 global logic: 在此处添加 register_global_logic("global_xxx", global_xxx); 即可 */
}

static GlobalLogicFunc global_logic_resolve(const char *name)
{
    for (int i = 0; i < g_logic_map_count; ++i)
        if (g_logic_map[i].name && strcmp(g_logic_map[i].name, name) == 0)
            return g_logic_map[i].func;
    return global_default;
}

/*======================== 辅助: 时间戳 ========================*/
static uint64_t steady_now_ms(void)
{
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

/*======================== 全局逻辑线程入口 (pthread) ========================*/
void *global_logic_thread_func(void *arg)
{
    GlobalLogicThread *t = (GlobalLogicThread *)arg;
    if (!t) return nullptr;

    printf("[GlobalLogic] Thread started: logic=%s poll=%dms channels=",
           t->config.logic.c_str(), t->config.poll_interval_ms);
    if (t->config.channels.empty())
        printf("ALL\n");
    else
    {
        for (size_t i = 0; i < t->config.channels.size(); ++i)
            printf("%d%s", t->config.channels[i], i + 1 < t->config.channels.size() ? "," : "\n");
    }

    APP_CTRL *ctrl = g_pCtrl;
    t->channel_ids.clear();
    if (t->config.channels.empty() && ctrl)
    {
        auto runtime = app_ctrl_get_runtime_snapshot();
        if (runtime)
            for (const auto &channel : runtime->config.channels)
                t->channel_ids.push_back(channel.id);
    }
    else
    {
        for (int channel_id : t->config.channels)
        {
            if (!app_ctrl_has_channel(channel_id))
            {
                fprintf(stderr, "[GlobalLogic] ignore unknown channel_id=%d\n",
                        channel_id);
                continue;
            }
            t->channel_ids.push_back(channel_id);
        }
    }

    int ch_count = (int)t->channel_ids.size();
    t->results_cache.assign(ch_count, {});
    t->last_infer_ts_ms.assign(ch_count, 0);
    for (int i = 0; i < ch_count; ++i)
        t->last_infer_ts_ms[i] = app_ctrl_get_last_infer_ts_ms(t->channel_ids[i]);

    int poll_ms = std::max(10, t->config.poll_interval_ms);
    if (ctrl && !t->channel_ids.empty())
    {
        int max_infer_fps = 1;
        auto runtime = app_ctrl_get_runtime_snapshot();
        for (int channel_id : t->channel_ids)
        {
            const ChannelConfig *channel =
                app_ctrl_runtime_channel_config(runtime, channel_id);
            if (!channel) continue;
            max_infer_fps = std::max(max_infer_fps, std::max(1, channel->max_fps));
        }

        int realtime_poll_ms = std::max(10, 500 / max_infer_fps);
        if (poll_ms > realtime_poll_ms)
        {
            printf("[GlobalLogic] poll interval auto-adjust: cfg=%dms -> %dms (max_infer_fps=%d)\n",
                   poll_ms, realtime_poll_ms, max_infer_fps);
            poll_ms = realtime_poll_ms;
        }
    }

    while (t->running.load())
    {
        pause_ctrl::wait_if_paused();
        if (!t->running.load()) break;

        uint64_t tick_begin_ms = steady_now_ms();

        int has_new_infer = 0;
        int latest_infer_channel = -1;
        uint64_t latest_infer_ts_ms = 0;

        for (int i = 0; i < ch_count; ++i)
        {
            const int channel_id = t->channel_ids[i];
            uint64_t infer_ts_ms = app_ctrl_get_last_infer_ts_ms(channel_id);
            if (infer_ts_ms > t->last_infer_ts_ms[i])
            {
                t->last_infer_ts_ms[i] = infer_ts_ms;
                has_new_infer = 1;
                t->results_cache[i] = app_ctrl_get_results_fresh(
                    channel_id, t->config.poll_interval_ms * 3);
            }
            if (t->last_infer_ts_ms[i] > latest_infer_ts_ms)
            {
                latest_infer_ts_ms = t->last_infer_ts_ms[i];
                latest_infer_channel = t->channel_ids[i];
            }
        }

        t->gctx.config              = &t->config;
        t->gctx.state               = &t->state;
        t->gctx.timestamp_ms        = steady_now_ms();
        t->gctx.tick_id             = ++t->tick_id;
        t->gctx.channel_ids         = &t->channel_ids;
        t->gctx.has_new_infer       = has_new_infer;
        t->gctx.latest_infer_channel = latest_infer_channel;
        t->gctx.latest_infer_ts_ms  = latest_infer_ts_ms;

        if (t->func) t->func(&t->gctx);

        if (t->stop_requested.load()) break;

        uint64_t elapsed_ms = steady_now_ms() - tick_begin_ms;
        if (elapsed_ms < (uint64_t)poll_ms)
        {
            int remaining_ms = poll_ms - static_cast<int>(elapsed_ms);
            while (remaining_ms > 0 && t->running.load())
            {
                const int slice_ms = std::min(remaining_ms, 50);
                usleep(static_cast<unsigned int>(slice_ms * 1000));
                remaining_ms -= slice_ms;
            }
        }
    }

    printf("[GlobalLogic] Thread exited: logic=%s\n", t->config.logic.c_str());
    return nullptr;
}

/*======================== 公开接口 ========================*/
int global_logic_start_all(const std::vector<GlobalLogicConfig> &cfgs)
{
    global_logic_register();
    global_logic_stop_all();

    pthread_mutex_lock(&g_threads_mtx);

    int started = 0;
    for (size_t i = 0; i < cfgs.size(); ++i)
    {
        const GlobalLogicConfig &cfg = cfgs[i];
        if (!cfg.enable) continue;

        GlobalLogicFunc fn = global_logic_resolve(cfg.logic.c_str());
        if (fn == global_default && strcmp(cfg.logic.c_str(), "global_default") != 0)
        {
            printf("[GlobalLogic][%zu] WARNING: logic '%s' not found, skipping\n",
                   i, cfg.logic.c_str());
            continue;
        }

        GlobalLogicThread *t = new GlobalLogicThread();
        t->config = cfg;
        t->running.store(true);
        t->stop_requested.store(false);
        t->func = fn;
        t->tick_id = 0;

        int ret = pthread_create(&t->tid, nullptr, global_logic_thread_func, t);
        if (ret != 0)
        {
            fprintf(stderr, "[GlobalLogic] pthread_create failed for %s: %s\n",
                    cfg.logic.c_str(), strerror(ret));
            delete t;
            continue;
        }

        g_threads.push_back(t);
        started++;
    }

    pthread_mutex_unlock(&g_threads_mtx);

    printf("[GlobalLogic] Started %d/%zu instance(s)\n", started, cfgs.size());
    return started;
}

void global_logic_stop_all(void)
{
    pthread_mutex_lock(&g_threads_mtx);

    for (GlobalLogicThread *t : g_threads)
    {
        if (!t) continue;
        if (!t->running.load()) continue;
        t->stop_requested.store(true);
        t->running.store(false);
        pthread_join(t->tid, nullptr);
        delete t;
    }
    g_threads.clear();

    pthread_mutex_unlock(&g_threads_mtx);
}

/*======================== 查询接口 (供 main) ========================*/
int global_logic_get_instance_count(void)
{
    pthread_mutex_lock(&g_threads_mtx);
    int n = (int)g_threads.size();
    pthread_mutex_unlock(&g_threads_mtx);
    return n;
}

/*======================== 全局逻辑函数实现 ========================*/

static void global_default(GlobalContext *gctx)
{
    (void)gctx;
}
