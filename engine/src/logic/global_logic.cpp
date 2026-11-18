#include "events/event_report.h"
/**
 * @file global_logic.cpp
 * @brief 全局逻辑模块 — C/pthread 风格
 *
 * 每个 GlobalLogicConfig 实例对应一个独立 pthread。启动阶段统一创建；热重载按稳定
 * instance_id 只替换发生变化的实例，避免一个节点改参数导致其它全局状态被清空。
 */

#include "application/control/logic_control.h"
#include "application/runtime/app_ctrl.h"
#include "application/runtime/publication_signal.h"
#include "application/runtime/runtime_status.h"
#include "application/runtime/service_watchdog.h"
#include "context_access.h"
#include "global_logic.h"
#include "logic/logic_registry.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <parameters.h>
#include <pthread.h>
#include <thread>
#include <utility>

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
    uint64_t pause_since_ms = 0;
    uint64_t paused_ms = 0;
    std::map<int, uint64_t> file_timelines;
    std::map<int, uint64_t> paused_publications;

    std::vector<int> channel_ids;
    std::vector<ChannelLogicSnapshot> channel_snapshots;
    std::vector<ChannelInput> ready_inputs;

    GlobalContext gctx;
    GlobalLogicFunc func;
};

static std::vector<GlobalLogicThread *> g_threads;
static std::mutex g_threads_mtx;
static constexpr int GLOBAL_LOGIC_MIN_DISPATCH_INTERVAL_MS = 5;

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

void GlobalContext::set_channel_draw_commands(int channel_id, const std::vector<DrawCommand> &commands) const
{
    if (config)
    {
        image_draw_commands[channel_id] = commands;
        app_ctrl_set_global_draw_commands(channel_id, config->instance_id, commands);
    }
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

/* 通道 Logic 由视频帧驱动，全局 Logic 由新发布立即唤醒并按配置周期兜底。底层统一淘汰
 * 长期没有更新的“最后一次值”，避免每个业务模块各自计算数据年龄。至少容忍 2 秒；
 * 兜底周期较长时则容忍 3 个兜底周期。该策略属于运行时健康机制，不是业务参数。 */
static int64_t automatic_input_timeout_ms(int poll_ms)
{
    constexpr int64_t minimum_timeout_ms = 2000;
    constexpr int64_t tolerated_poll_intervals = 3;
    return std::max<int64_t>(minimum_timeout_ms, static_cast<int64_t>(poll_ms) * tolerated_poll_intervals);
}

static const ChannelLogicSnapshot *find_snapshot(const std::vector<ChannelLogicSnapshot> &snapshots, int channel_id)
{
    for (const auto &snapshot : snapshots)
        if (snapshot.channel_id == channel_id)
            return &snapshot;
    return nullptr;
}

static void prepare_ready_inputs(GlobalLogicThread *thread, int64_t timeout_ms)
{
    thread->ready_inputs.clear();
    auto append_if_ready = [&](const ChannelLogicSnapshot *snapshot) {
        if (snapshot && snapshot->readable(timeout_ms))
            thread->ready_inputs.emplace_back(snapshot);
    };

    if (!thread->config.channels.empty())
    {
        for (int channel_id : thread->config.channels)
            append_if_ready(find_snapshot(thread->channel_snapshots, channel_id));
        return;
    }

    for (const auto &snapshot : thread->channel_snapshots)
        append_if_ready(&snapshot);
}

/*======================== 全局逻辑线程入口 (pthread) ========================*/
void *global_logic_thread_func(void *arg)
try
{
    GlobalLogicThread *t = (GlobalLogicThread *)arg;
    if (!t)
        return nullptr;

    printf("[GlobalLogic] Thread started: id=%s logic=%s poll=%dms connected=", t->config.instance_id.c_str(),
           t->config.logic.c_str(), t->config.poll_interval_ms);
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
    t->channel_snapshots.reserve(ch_count);
    t->ready_inputs.reserve(ch_count);

    int poll_ms = std::max(10, t->config.poll_interval_ms);
    const int64_t input_timeout_ms = automatic_input_timeout_ms(poll_ms);

    service_watchdog::Progress watchdog_progress("global logic");
    while (t->running.load())
    {
        if (!t->running.load())
            break;

        /* 先记住发布序号。取快照或执行 Logic 期间若又有新帧发布，下面的等待会
         * 立即返回，确保该更新不会因为错过条件变量通知而延迟到下一轮兜底周期。 */
        watchdog_progress.begin();
        const uint64_t observed_publication = publication_signal_sequence();
        const uint64_t tick_begin_ms = steady_now_ms();
        const uint64_t tick_unix_ms = system_now_ms();

        t->channel_snapshots.clear();
        VisionContextAccess::overlays(t->gctx).clear();

        for (int i = 0; i < ch_count; ++i)
        {
            const int channel_id = t->channel_ids[i];
            ChannelLogicSnapshot snapshot;
            if (!app_ctrl_get_channel_logic_snapshot(channel_id, &snapshot))
                continue;

            t->channel_snapshots.push_back(std::move(snapshot));
        }

        bool file_paused = false;
        bool timeline_changed = false;
        for (const auto &snapshot : t->channel_snapshots)
        {
            if (!t->config.channels.empty() && std::find(t->config.channels.begin(), t->config.channels.end(),
                                                         snapshot.channel_id) == t->config.channels.end())
                continue;
            file_paused = file_paused || snapshot.file_paused;
            const auto paused = t->paused_publications.find(snapshot.channel_id);
            if (snapshot.file_paused)
                t->paused_publications[snapshot.channel_id] = snapshot.publication_seq;
            else if (paused != t->paused_publications.end())
            {
                // 恢复后先等一帧新结果，避免把暂停前的旧快照误判成断流。
                if (snapshot.online_state == CH_ONLINE && snapshot.publication_seq == paused->second)
                    file_paused = true;
                else
                    t->paused_publications.erase(paused);
            }
            const auto previous = t->file_timelines.find(snapshot.channel_id);
            if (previous != t->file_timelines.end() && previous->second != snapshot.file_timeline)
                timeline_changed = true;
            t->file_timelines[snapshot.channel_id] = snapshot.file_timeline;
        }
        if (timeline_changed)
        {
            t->state.reset();
            t->last_tick_steady_ms = 0;
        }
        if (file_paused)
        {
            if (!t->pause_since_ms)
                t->pause_since_ms = tick_begin_ms;
            t->channel_snapshots.clear();
            t->ready_inputs.clear();
            watchdog_progress.end();
            publication_signal_wait(observed_publication, 50);
            continue;
        }
        if (t->pause_since_ms)
        {
            t->paused_ms += tick_begin_ms - t->pause_since_ms;
            t->pause_since_ms = 0;
        }
        const uint64_t analysis_ms = tick_begin_ms - t->paused_ms;
        const float dt_ms = t->last_tick_steady_ms && analysis_ms >= t->last_tick_steady_ms
                                ? static_cast<float>(analysis_ms - t->last_tick_steady_ms)
                                : 0.0f;
        t->last_tick_steady_ms = analysis_ms;
        prepare_ready_inputs(t, input_timeout_ms);

        const auto runtime = app_ctrl_get_runtime_snapshot();
        t->gctx.config = &t->config;
        t->gctx.state = &t->state;
        VisionContextAccess::bind_parameters(t->gctx, &t->logic_parameters);
        t->gctx.timestamp_ms = analysis_ms;
        t->gctx.real_time_ms = tick_begin_ms;
        t->gctx.unix_ms = tick_unix_ms;
        t->gctx.dt_ms = dt_ms;
        t->gctx.tick_id = t->tick_id++;
        t->gctx.effective_poll_interval_ms = poll_ms;
        t->gctx.runtime_generation = runtime ? runtime->generation : 0;
        VisionContextAccess::bind_global(t->gctx, &t->channel_snapshots, &t->config.channels, &t->ready_inputs);

        std::vector<LogicAction> pending_actions;
        logic_control_take_global(t->config.instance_id, pending_actions);
        GlobalLogicActionFunc action_fn = global_logic_action_get(t->config.logic.c_str());
        for (const LogicAction &action : pending_actions)
        {
            if (action.logic_name != t->config.logic)
            {
                printf("[GlobalAction][%s][%s] drop action=%s request=%s (queued for %s)\n",
                       t->config.instance_id.c_str(), t->config.logic.c_str(), action.name.c_str(),
                       action.request_id.c_str(), action.logic_name.c_str());
                continue;
            }
            if (!action_fn)
            {
                printf("[GlobalAction][%s][%s] no handler for action=%s request=%s\n", t->config.instance_id.c_str(),
                       t->config.logic.c_str(), action.name.c_str(), action.request_id.c_str());
                continue;
            }
            LogicActionResult result = action_fn(&t->gctx, &action);
            printf("[GlobalAction][%s][%s] action=%s request=%s handled=%d msg=%s\n", t->config.instance_id.c_str(),
                   t->config.logic.c_str(), action.name.c_str(), action.request_id.c_str(), result.handled ? 1 : 0,
                   result.message.c_str());
        }

        if (t->func)
            t->func(&t->gctx);

        /* 本 tick 已完成：释放证据帧引用，等待期间不占用旧解码缓冲区。 */
        t->ready_inputs.clear();
        t->channel_snapshots.clear();
        VisionContextAccess::overlays(t->gctx).clear();
        watchdog_progress.end();

        if (t->stop_requested.load())
            break;

        uint64_t elapsed_ms = steady_now_ms() - tick_begin_ms;
        if (elapsed_ms < static_cast<uint64_t>(GLOBAL_LOGIC_MIN_DISPATCH_INTERVAL_MS))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(GLOBAL_LOGIC_MIN_DISPATCH_INTERVAL_MS - elapsed_ms));
            elapsed_ms = steady_now_ms() - tick_begin_ms;
        }
        if (elapsed_ms < (uint64_t)poll_ms)
            publication_signal_wait(observed_publication, poll_ms - static_cast<int>(elapsed_ms));
    }

    printf("[GlobalLogic] Thread exited: id=%s logic=%s\n", t->config.instance_id.c_str(), t->config.logic.c_str());
    return nullptr;
}
catch (const std::exception &error)
{
    const auto *thread = static_cast<GlobalLogicThread *>(arg);
    app_ctrl_worker_failed(-1, thread->config.instance_id.c_str(), error.what());
    return nullptr;
}
catch (...)
{
    const auto *thread = static_cast<GlobalLogicThread *>(arg);
    app_ctrl_worker_failed(-1, thread->config.instance_id.c_str(), "未知异常");
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

    std::unique_ptr<GlobalLogicThread> t(new GlobalLogicThread());
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
        return nullptr;
    }

    VisionContextAccess::bind_events(t->gctx, &event_report_runtime_sink());
    int ret = pthread_create(&t->tid, nullptr, global_logic_thread_func, t.get());
    if (ret != 0)
    {
        fprintf(stderr, "[GlobalLogic][%s] pthread_create failed for %s: %s\n", cfg.instance_id.c_str(),
                cfg.logic.c_str(), strerror(ret));
        return nullptr;
    }
    return t.release();
}

static void stop_one(GlobalLogicThread *t)
{
    if (!t)
        return;
    t->stop_requested.store(true);
    t->running.store(false);
    publication_signal_notify();
    pthread_join(t->tid, nullptr);
    app_ctrl_clear_all_global_draw_commands(t->config.instance_id);
}

/* Caller owns g_threads_mtx; used for normal shutdown and failed startup rollback. */
static void stop_all_locked()
{
    for (GlobalLogicThread *t : g_threads)
    {
        stop_one(t);
        delete t;
    }
    g_threads.clear();
}

static const GlobalLogicConfig *find_config(const std::vector<GlobalLogicConfig> &cfgs, const std::string &instance_id)
{
    for (const auto &cfg : cfgs)
        if (cfg.instance_id == instance_id)
            return &cfg;
    return nullptr;
}

int global_logic_start_all(const std::vector<GlobalLogicConfig> &cfgs)
{
    global_logic_stop_all();

    int started = 0;
    {
        const std::lock_guard<std::mutex> lock(g_threads_mtx);
        g_threads.reserve(cfgs.size());
        try
        {
            for (const auto &cfg : cfgs)
            {
                if (!cfg.enable)
                    continue;
                GlobalLogicThread *t = start_one(cfg);
                if (t)
                {
                    g_threads.push_back(t); // Capacity was reserved before any thread was started.
                    ++started;
                }
            }
        }
        catch (...)
        {
            stop_all_locked();
            throw;
        }
    }

    printf("[GlobalLogic] Started %d/%zu instance(s)\n", started, cfgs.size());
    return started;
}

int global_logic_reload_all(const std::vector<GlobalLogicConfig> &cfgs)
{
    const std::lock_guard<std::mutex> lock(g_threads_mtx);
    g_threads.reserve(std::max(g_threads.size(), cfgs.size()));

    for (auto it = g_threads.begin(); it != g_threads.end();)
    {
        GlobalLogicThread *current = *it;
        const GlobalLogicConfig *next = find_config(cfgs, current->config.instance_id);
        if (next && next->enable && current->config == *next)
        {
            ++it;
            continue;
        }

        // Complete potentially allocating comparisons before stopping the current instance.
        const std::string instance_id = current->config.instance_id;
        std::shared_ptr<void> preserved_state;
        bool preserve_state = false;
        if (next && next->enable && current->config.logic == next->logic && current->config.channels == next->channels)
        {
            const LogicReloadImpact impact = logic_parameters_reload_impact(
                next->logic, current->config.logic_parameters_json, next->logic_parameters_json);
            if (impact == LogicReloadImpact::NONE || impact == LogicReloadImpact::PRESERVE_STATE)
            {
                preserve_state = true;
            }
        }

        stop_one(current);
        if (preserve_state)
            preserved_state = current->state;
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
    return running;
}

void global_logic_stop_all(void)
{
    const std::lock_guard<std::mutex> lock(g_threads_mtx);
    stop_all_locked();
}

/*======================== 查询接口 (供 main) ========================*/
int global_logic_get_instance_count(void)
{
    const std::lock_guard<std::mutex> lock(g_threads_mtx);
    int n = (int)g_threads.size();
    return n;
}
