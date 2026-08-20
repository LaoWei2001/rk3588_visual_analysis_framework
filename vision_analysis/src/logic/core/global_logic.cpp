/**
 * @file global_logic.cpp
 * @brief 全局逻辑模块 — C/pthread 风格
 *
 * 每个 GlobalLogicConfig 实例对应一个独立 pthread。启动阶段统一创建；热重载按稳定
 * instance_id 只替换发生变化的实例，避免一个节点改参数导致其它全局状态被清空。
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

    printf("[GlobalLogic] Thread started: id=%s logic=%s poll=%dms connected=",
           t->config.instance_id.c_str(), t->config.logic.c_str(), t->config.poll_interval_ms);
    if (t->config.channels.empty())
        printf("NONE\n");
    else
    {
        for (size_t i = 0; i < t->config.channels.size(); ++i)
            printf("%d%s", t->config.channels[i], i + 1 < t->config.channels.size() ? "," : "\n");
    }

    APP_CTRL *ctrl = g_pCtrl;
    t->channel_ids.clear();
    if (ctrl)
    {
        auto runtime = app_ctrl_get_runtime_snapshot();
        if (runtime)
            for (const auto &channel : runtime->config.channels)
                t->channel_ids.push_back(channel.id);
    }

    int ch_count = (int)t->channel_ids.size();
    t->channel_observed.assign(ch_count, 0);
    t->last_publication_seq.assign(ch_count, 0);
    t->channel_snapshots.reserve(ch_count);
    t->updated_channels.reserve(ch_count);

    int poll_ms = std::max(10, t->config.poll_interval_ms);

    while (t->running.load())
    {
        pause_ctrl::wait_if_paused(&t->running);
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
        t->gctx.connected_channel_ids = &t->config.channels;
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

    printf("[GlobalLogic] Thread exited: id=%s logic=%s\n", t->config.instance_id.c_str(),
           t->config.logic.c_str());
    return nullptr;
}

/*======================== 公开接口 ========================*/
static GlobalLogicThread *start_one(const GlobalLogicConfig &cfg, const std::shared_ptr<void> &preserved_state = {})
{
    GlobalLogicFunc fn = global_logic_get(cfg.logic.c_str());
    if (!fn)
    {
        fprintf(stderr, "[GlobalLogic][%s] logic '%s' not found\n", cfg.instance_id.c_str(), cfg.logic.c_str());
        return nullptr;
    }

    GlobalLogicThread *t = new GlobalLogicThread();
    t->config = cfg;
    t->state = preserved_state;
    t->running.store(true);
    t->stop_requested.store(false);
    t->func = fn;
    t->tick_id = 0;
    t->last_tick_steady_ms = 0;
    std::vector<LogicParameterError> parameter_errors;
    if (!logic_parameters_resolve(cfg.logic, cfg.logic_parameters_json, nullptr, &t->logic_parameters,
                                  &parameter_errors))
    {
        fprintf(stderr, "[GlobalLogic][%s] parameters for '%s' are invalid:\n", cfg.instance_id.c_str(),
                cfg.logic.c_str());
        for (const auto &error : parameter_errors)
            fprintf(stderr, "  - %s: %s\n", error.field.c_str(), error.message.c_str());
        delete t;
        return nullptr;
    }

    int ret = pthread_create(&t->tid, nullptr, global_logic_thread_func, t);
    if (ret != 0)
    {
        fprintf(stderr, "[GlobalLogic][%s] pthread_create failed for %s: %s\n", cfg.instance_id.c_str(),
                cfg.logic.c_str(), strerror(ret));
        delete t;
        return nullptr;
    }
    return t;
}

static void stop_one(GlobalLogicThread *t)
{
    if (!t)
        return;
    t->stop_requested.store(true);
    t->running.store(false);
    pause_ctrl::notify_waiters();
    pthread_join(t->tid, nullptr);
}

static const GlobalLogicConfig *find_config(const std::vector<GlobalLogicConfig> &cfgs,
                                            const std::string &instance_id)
{
    for (const auto &cfg : cfgs)
        if (cfg.instance_id == instance_id)
            return &cfg;
    return nullptr;
}

int global_logic_start_all(const std::vector<GlobalLogicConfig> &cfgs)
{
    global_logic_stop_all();

    pthread_mutex_lock(&g_threads_mtx);
    int started = 0;
    for (const auto &cfg : cfgs)
    {
        if (!cfg.enable)
            continue;
        GlobalLogicThread *t = start_one(cfg);
        if (t)
        {
            g_threads.push_back(t);
            ++started;
        }
    }
    pthread_mutex_unlock(&g_threads_mtx);

    printf("[GlobalLogic] Started %d/%zu instance(s)\n", started, cfgs.size());
    return started;
}

int global_logic_reload_all(const std::vector<GlobalLogicConfig> &cfgs)
{
    pthread_mutex_lock(&g_threads_mtx);

    for (auto it = g_threads.begin(); it != g_threads.end();)
    {
        GlobalLogicThread *current = *it;
        const GlobalLogicConfig *next = find_config(cfgs, current->config.instance_id);
        if (next && next->enable && current->config == *next)
        {
            ++it;
            continue;
        }

        stop_one(current);
        std::shared_ptr<void> preserved_state;
        bool preserve_state = false;
        if (next && next->enable && current->config.logic == next->logic &&
            current->config.channels == next->channels)
        {
            const LogicReloadImpact impact = logic_parameters_reload_impact(
                next->logic, current->config.logic_parameters_json, next->logic_parameters_json);
            if (impact == LogicReloadImpact::NONE || impact == LogicReloadImpact::PRESERVE_STATE)
            {
                preserve_state = true;
                preserved_state = current->state;
            }
        }

        const std::string instance_id = current->config.instance_id;
        delete current;
        it = g_threads.erase(it);

        if (next && next->enable)
        {
            GlobalLogicThread *replacement = start_one(*next, preserved_state);
            if (replacement)
            {
                it = g_threads.insert(it, replacement);
                ++it;
                printf("[GlobalLogic] Reloaded instance %s (%s)\n", instance_id.c_str(),
                       preserve_state ? "state preserved" : "state reset");
            }
        }
    }

    for (const auto &cfg : cfgs)
    {
        if (!cfg.enable)
            continue;
        const bool exists = std::any_of(g_threads.begin(), g_threads.end(), [&](const GlobalLogicThread *thread) {
            return thread && thread->config.instance_id == cfg.instance_id;
        });
        if (!exists)
        {
            GlobalLogicThread *t = start_one(cfg);
            if (t)
                g_threads.push_back(t);
        }
    }

    const int running = static_cast<int>(g_threads.size());
    pthread_mutex_unlock(&g_threads_mtx);
    return running;
}

void global_logic_stop_all(void)
{
    pthread_mutex_lock(&g_threads_mtx);

    for (GlobalLogicThread *t : g_threads)
    {
        if (!t)
            continue;
        stop_one(t);
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
