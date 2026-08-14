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
#include "logic_parameters.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <pthread.h>
#include <unistd.h>

/*======================== 单个实例的线程上下文 ========================*/
struct GlobalLogicThread
{
    GlobalLogicConfig config;
    pthread_t tid;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::shared_ptr<void> state;
    LogicParameterSet logic_parameters;
    int64_t tick_id;
    uint64_t last_tick_steady_ms;

    std::vector<int> channel_ids;
    std::vector<unsigned char> channel_observed;
    std::vector<uint64_t> last_publication_seq;
    std::vector<ChannelLogicSnapshot> channel_snapshots;
    std::vector<ChannelUpdate> updated_channels;

    GlobalContext gctx;
    GlobalLogicFunc func;
};

static std::vector<GlobalLogicThread *> g_threads;
static pthread_mutex_t g_threads_mtx = PTHREAD_MUTEX_INITIALIZER;

/*======================== 全局逻辑分发表 ========================*/
struct GlobalLogicEntry
{
    const char *name = nullptr;
    GlobalLogicFunc func = nullptr;
};
static GlobalLogicEntry g_logic_registry[MAX_GLOBAL_LOGIC_FUNCS];
static int g_logic_count = 0;

void register_global_logic(const char *name, GlobalLogicFunc func)
{
    if (!name || !func)
        return;
    for (int i = 0; i < g_logic_count; ++i)
    {
        if (g_logic_registry[i].name && strcmp(g_logic_registry[i].name, name) == 0)
        {
            g_logic_registry[i].func = func;
            return;
        }
    }
    if (g_logic_count < MAX_GLOBAL_LOGIC_FUNCS)
    {
        g_logic_registry[g_logic_count].name = name;
        g_logic_registry[g_logic_count].func = func;
        ++g_logic_count;
    }
}

GlobalLogicFunc global_logic_get(const char *name)
{
    if (name)
        for (int i = 0; i < g_logic_count; ++i)
            if (g_logic_registry[i].name && strcmp(g_logic_registry[i].name, name) == 0)
                return g_logic_registry[i].func;
    return nullptr;
}

std::vector<std::string> global_logic_names()
{
    std::vector<std::string> names;
    names.reserve(g_logic_count);
    for (int i = 0; i < g_logic_count; ++i)
        if (g_logic_registry[i].name)
            names.emplace_back(g_logic_registry[i].name);
    std::sort(names.begin(), names.end());
    return names;
}

/*======================== GlobalContext 参数访问 ========================*/
bool GlobalContext::has_param(const char *key) const
{
    return logic_parameters && logic_parameters->has(key);
}

float GlobalContext::param_float(const char *key) const
{
    return logic_parameters ? logic_parameters->get_float(key) : 0.0f;
}

int64_t GlobalContext::param_int(const char *key) const
{
    return logic_parameters ? logic_parameters->get_int(key) : 0;
}

bool GlobalContext::param_bool(const char *key) const
{
    return logic_parameters ? logic_parameters->get_bool(key) : false;
}

std::string GlobalContext::param_string(const char *key) const
{
    return logic_parameters ? logic_parameters->get_string(key) : std::string();
}

std::string GlobalContext::param_json(const char *key) const
{
    return logic_parameters ? logic_parameters->get_json(key) : std::string();
}

/*======================== 辅助: 时间戳 ========================*/
static uint64_t steady_now_ms(void)
{
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static uint64_t system_now_ms(void)
{
    auto now = std::chrono::system_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

/*======================== 全局逻辑线程入口 (pthread) ========================*/
void *global_logic_thread_func(void *arg)
{
    GlobalLogicThread *t = (GlobalLogicThread *)arg;
    if (!t)
        return nullptr;

    printf("[GlobalLogic] Thread started: logic=%s poll=%dms channels=", t->config.logic.c_str(),
           t->config.poll_interval_ms);
    if (t->config.channels.empty())
        printf("%s\n", t->config.channels_explicit ? "NONE" : "ALL");
    else
    {
        for (size_t i = 0; i < t->config.channels.size(); ++i)
            printf("%d%s", t->config.channels[i], i + 1 < t->config.channels.size() ? "," : "\n");
    }

    APP_CTRL *ctrl = g_pCtrl;
    t->channel_ids.clear();
    if (t->config.channels.empty() && !t->config.channels_explicit && ctrl)
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
                fprintf(stderr, "[GlobalLogic] ignore unknown channel_id=%d\n", channel_id);
                continue;
            }
            t->channel_ids.push_back(channel_id);
        }
    }

    int ch_count = (int)t->channel_ids.size();
    t->channel_observed.assign(ch_count, 0);
    t->last_publication_seq.assign(ch_count, 0);
    t->channel_snapshots.reserve(ch_count);
    t->updated_channels.reserve(ch_count);

    int poll_ms = std::max(10, t->config.poll_interval_ms);
    if (ctrl && !t->channel_ids.empty())
    {
        int max_infer_fps = 1;
        auto runtime = app_ctrl_get_runtime_snapshot();
        for (int channel_id : t->channel_ids)
        {
            const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, channel_id);
            if (!channel)
                continue;
            max_infer_fps = std::max(max_infer_fps, std::max(1, channel->max_fps));
        }

        int realtime_poll_ms = std::max(10, 500 / max_infer_fps);
        if (poll_ms > realtime_poll_ms)
        {
            printf("[GlobalLogic] poll interval auto-adjust: cfg=%dms -> %dms (max_infer_fps=%d)\n", poll_ms,
                   realtime_poll_ms, max_infer_fps);
            poll_ms = realtime_poll_ms;
        }
    }

    while (t->running.load())
    {
        pause_ctrl::wait_if_paused();
        if (!t->running.load())
            break;

        const uint64_t tick_begin_ms = steady_now_ms();
        const uint64_t tick_unix_ms = system_now_ms();
        const float dt_ms = t->last_tick_steady_ms == 0
                                ? 0.0f
                                : static_cast<float>(tick_begin_ms - t->last_tick_steady_ms);
        t->last_tick_steady_ms = tick_begin_ms;

        t->channel_snapshots.clear();
        t->updated_channels.clear();

        for (int i = 0; i < ch_count; ++i)
        {
            const int channel_id = t->channel_ids[i];
            ChannelLogicSnapshot snapshot;
            if (!app_ctrl_get_channel_logic_snapshot(channel_id, &snapshot))
                continue;

            const uint64_t previous_seq = t->last_publication_seq[i];
            const uint64_t current_seq = snapshot.publication_seq;
            if (current_seq != previous_seq)
            {
                const bool initial_snapshot = t->channel_observed[i] == 0;
                const uint64_t revision_count =
                    initial_snapshot ? 1 : (current_seq > previous_seq ? current_seq - previous_seq : 1);
                ChannelUpdate update;
                update.channel_id = channel_id;
                update.initial_snapshot = initial_snapshot;
                update.previous_publication_seq = previous_seq;
                update.publication_seq = current_seq;
                update.revision_count = revision_count;
                update.missed_revisions = revision_count > 0 ? revision_count - 1 : 0;
                update.published_steady_ms = snapshot.published_steady_ms;
                t->updated_channels.push_back(update);
                t->last_publication_seq[i] = current_seq;
            }
            t->channel_observed[i] = 1;
            t->channel_snapshots.push_back(std::move(snapshot));
        }

        const auto runtime = app_ctrl_get_runtime_snapshot();
        t->gctx.config = &t->config;
        t->gctx.state = &t->state;
        t->gctx.logic_parameters = &t->logic_parameters;
        t->gctx.steady_ms = tick_begin_ms;
        t->gctx.unix_ms = tick_unix_ms;
        t->gctx.dt_ms = dt_ms;
        t->gctx.tick_id = t->tick_id++;
        t->gctx.effective_poll_interval_ms = poll_ms;
        t->gctx.runtime_generation = runtime ? runtime->generation : 0;
        t->gctx.channel_snapshots = &t->channel_snapshots;
        t->gctx.updated_channels = &t->updated_channels;

        if (t->func)
            t->func(&t->gctx);

        if (t->stop_requested.load())
            break;

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
    global_logic_stop_all();

    pthread_mutex_lock(&g_threads_mtx);

    int started = 0;
    for (size_t i = 0; i < cfgs.size(); ++i)
    {
        const GlobalLogicConfig &cfg = cfgs[i];
        if (!cfg.enable)
            continue;

        GlobalLogicFunc fn = global_logic_get(cfg.logic.c_str());
        if (!fn)
        {
            printf("[GlobalLogic][%zu] WARNING: logic '%s' not found, skipping\n", i, cfg.logic.c_str());
            continue;
        }

        GlobalLogicThread *t = new GlobalLogicThread();
        t->config = cfg;
        t->running.store(true);
        t->stop_requested.store(false);
        t->func = fn;
        t->tick_id = 0;
        t->last_tick_steady_ms = 0;
        std::vector<LogicParameterError> parameter_errors;
        if (!logic_parameters_resolve(cfg.logic, cfg.logic_parameters_json, nullptr, &t->logic_parameters,
                                      &parameter_errors))
        {
            fprintf(stderr, "[GlobalLogic][%zu] parameters for '%s' are invalid:\n", i, cfg.logic.c_str());
            for (const auto &error : parameter_errors)
                fprintf(stderr, "  - %s: %s\n", error.field.c_str(), error.message.c_str());
            delete t;
            continue;
        }

        int ret = pthread_create(&t->tid, nullptr, global_logic_thread_func, t);
        if (ret != 0)
        {
            fprintf(stderr, "[GlobalLogic] pthread_create failed for %s: %s\n", cfg.logic.c_str(), strerror(ret));
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
        if (!t)
            continue;
        if (!t->running.load())
            continue;
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
