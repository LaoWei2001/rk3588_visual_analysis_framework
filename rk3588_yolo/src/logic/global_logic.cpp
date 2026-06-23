/**
 * @file global_logic.cpp
 * @brief 全局逻辑模块 — C/pthread 风格
 *
 * 每个 GlobalLogicConfig 实例对应一个独立 pthread.
 * 线程由 global_logic_start_all (初始/热重载) 创建和管理.
 */

#include "global_logic.h"
#include "../core/app_ctrl.h"
#include "../core/pause_ctrl.h"
#include "logic_tools.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>

/*======================== 单个实例的线程上下文 ========================*/
struct GlobalLogicThread
{
    GlobalLogicConfig config;
    pthread_t tid;
    volatile int running;
    volatile int stop_requested;
    std::shared_ptr<void> state;
    int64_t tick_id;

    std::vector<std::vector<AlgoResult>> results_cache;
    std::vector<int> channel_ids;
    std::vector<uint64_t> last_infer_ts_ms;

    GlobalContext gctx;
    GlobalLogicFunc func;
};

static std::vector<GlobalLogicThread *> g_threads;
static pthread_mutex_t g_threads_mtx = PTHREAD_MUTEX_INITIALIZER;

/*======================== 全局逻辑分发表 ========================*/
#define MAX_GLOBAL_LOGICS 16
static struct
{
    const char *name;
    GlobalLogicFunc func;
} g_logic_map[MAX_GLOBAL_LOGICS];
static int g_logic_map_count = 0;

static void global_example(GlobalContext *gctx);
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
    register_global_logic("global_example", global_example);
    register_global_logic("global_default", global_default);

    /* 新增 global logic: 在此处添加 register_global_logic("global_xxx",
     * global_xxx); 即可 */
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
        for (int i = 0; i < app_ctrl_get_chn_nums(); ++i)
            t->channel_ids.push_back(i);
    }
    else
        t->channel_ids = t->config.channels;

    int ch_count = (int)t->channel_ids.size();
    t->results_cache.assign(ch_count, {});
    t->last_infer_ts_ms.assign(ch_count, 0);
    for (int i = 0; i < ch_count; ++i)
        t->last_infer_ts_ms[i] = app_ctrl_get_last_infer_ts_ms(t->channel_ids[i]);

    int poll_ms = std::max(10, t->config.poll_interval_ms);
    if (ctrl && !t->channel_ids.empty())
    {
        int max_infer_fps = 1;
        pthread_rwlock_rdlock(&ctrl->mtx);
        for (int ch : t->channel_ids)
        {
            if (ch < 0 || ch >= app_ctrl_get_chn_nums())
                continue;
            max_infer_fps = std::max(max_infer_fps, std::max(1, ctrl->config.channels[ch].max_fps));
        }
        pthread_rwlock_unlock(&ctrl->mtx);

        int realtime_poll_ms = std::max(10, 500 / max_infer_fps);
        if (poll_ms > realtime_poll_ms)
        {
            printf("[GlobalLogic] poll interval auto-adjust: cfg=%dms -> %dms "
                   "(max_infer_fps=%d)\n",
                   poll_ms, realtime_poll_ms, max_infer_fps);
            poll_ms = realtime_poll_ms;
        }
    }

    while (t->running)
    {
        pause_ctrl::wait_if_paused();
        if (!t->running)
            break;

        uint64_t tick_begin_ms = steady_now_ms();

        int has_new_infer = 0;
        int latest_infer_channel = -1;
        uint64_t latest_infer_ts_ms = 0;

        for (int i = 0; i < ch_count; ++i)
        {
            int ch = t->channel_ids[i];
            uint64_t infer_ts_ms = app_ctrl_get_last_infer_ts_ms(ch);
            if (infer_ts_ms > t->last_infer_ts_ms[i])
            {
                t->last_infer_ts_ms[i] = infer_ts_ms;
                has_new_infer = 1;
                t->results_cache[i] = app_ctrl_get_results_fresh(ch, t->config.poll_interval_ms * 3);
            }
            if (t->last_infer_ts_ms[i] > latest_infer_ts_ms)
            {
                latest_infer_ts_ms = t->last_infer_ts_ms[i];
                latest_infer_channel = ch;
            }
        }

        t->gctx.config = &t->config;
        t->gctx.state = &t->state;
        t->gctx.timestamp_ms = steady_now_ms();
        t->gctx.tick_id = ++t->tick_id;
        t->gctx.channel_ids = &t->channel_ids;
        t->gctx.has_new_infer = has_new_infer;
        t->gctx.latest_infer_channel = latest_infer_channel;
        t->gctx.latest_infer_ts_ms = latest_infer_ts_ms;

        if (t->func)
            t->func(&t->gctx);

        if (t->stop_requested)
            break;

        uint64_t elapsed_ms = steady_now_ms() - tick_begin_ms;
        if (elapsed_ms < (uint64_t)poll_ms)
            usleep((unsigned int)((poll_ms - (int)elapsed_ms) * 1000));
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
        if (!cfg.enable)
            continue;

        GlobalLogicFunc fn = global_logic_resolve(cfg.logic.c_str());
        if (fn == global_default && strcmp(cfg.logic.c_str(), "global_default") != 0)
        {
            printf("[GlobalLogic][%zu] WARNING: logic '%s' not found, skipping\n", i, cfg.logic.c_str());
            continue;
        }

        GlobalLogicThread *t = new GlobalLogicThread();
        t->config = cfg;
        t->running = 1;
        t->stop_requested = 0;
        t->func = fn;
        t->tick_id = 0;

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
        if (!t->running)
            continue;
        t->stop_requested = 1;
        t->running = 0;
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

struct GlobalAlarmState
{
    int multi_alarm_sent = 0;        /* 报警锁存: 冷却期内不再重复触发 */
    uint64_t last_alarm_sent_ms = 0; /* 最近一次触发时刻 (ms) */
    int alarm_cooldown_ms = 10000;   /* 报警冷却时长 (ms) */
    int64_t tick_count = 0;
};

static void global_example(GlobalContext *gctx)
{
    if (!gctx)
        return;

    if (!(*gctx->state))
        *gctx->state = std::make_shared<GlobalAlarmState>();

    auto gl_state_ptr = std::static_pointer_cast<GlobalAlarmState>(*gctx->state);
    GlobalAlarmState &gl_state = *gl_state_ptr;
    gl_state.tick_count++;

    struct ChAlarmInfo
    {
        int chnId, person_detected, person_count;
    };
    std::vector<ChAlarmInfo> alarm_infos;

    gctx->for_each_channel([&](int chnId, int) {
        ChAlarmInfo info = {chnId, 0, 0};
        if (gctx->channel_has_logic(chnId, "logic_person_alarm"))
        {
            auto snap = gctx->get_channel_snapshot(chnId);
            if (!snap.frame.empty())
            {
                auto person_state = std::static_pointer_cast<PersonAlarmState>(snap.logic_state);
                if (person_state && snap.result_age_ms >= 0 && snap.result_age_ms < 500)
                {
                    info.person_detected = person_state->person_detected ? 1 : 0;
                    info.person_count = person_state->person_count;
                }
            }
        }
        alarm_infos.push_back(info);
    });

    int alarming_channel_count = 0, total_persons = 0;
    for (const auto &info : alarm_infos)
    {
        if (info.person_detected)
        {
            alarming_channel_count++;
            total_persons += info.person_count;
        }
    }

    if (alarming_channel_count >= 2 && !gl_state.multi_alarm_sent)
    {
        char detail[512] = {0};
        int offset = 0;
        for (const auto &info : alarm_infos)
        {
            if (info.person_detected)
                offset +=
                    snprintf(detail + offset, sizeof(detail) - offset, "CH%d(%d) ", info.chnId, info.person_count);
        }

        gl_state.multi_alarm_sent = 1;
        gl_state.last_alarm_sent_ms = gctx->timestamp_ms;

        /* 示例只打印; 需要真正上报时在此接 alarm_uploader_enqueue (范式见
         * global_logic.h 头部示例) */
        printf("[GlobalLogic][global_example] MULTI-ZONE ALARM: channels=%d "
               "persons=%d | %s\n",
               alarming_channel_count, total_persons, detail);
    }

    if (gl_state.multi_alarm_sent)
    {
        if (gctx->timestamp_ms - gl_state.last_alarm_sent_ms >= (uint64_t)gl_state.alarm_cooldown_ms)
            gl_state.multi_alarm_sent = 0;
    }

    if (gl_state.tick_count % 50 == 0)
    {
        printf("\n[GlobalLogic][global_example] tick=%lld | %zu channels | "
               "alarming=%d | total_persons=%d | alarm_latched=%s\n",
               (long long)gl_state.tick_count, alarm_infos.size(), alarming_channel_count, total_persons,
               gl_state.multi_alarm_sent ? "YES" : "NO");
        for (const auto &info : alarm_infos)
            printf("  CH%d: %s (%d person)\n", info.chnId, info.person_detected ? "ALARM" : "CLEAR", info.person_count);
        fflush(stdout);
    }
}

static void global_default(GlobalContext *gctx)
{
    if (!gctx)
        return;
    if (gctx->tick_id % 30 != 0)
        return;

    printf("\n==== [global_default] tick=%lld ====\n", (long long)gctx->tick_id);

    gctx->for_each_channel([&](int chnId, int) {
        auto snap = gctx->get_channel_snapshot(chnId);
        if (snap.frame.empty())
            return;
        if (snap.result_age_ms > 500)
            return;
        if (!snap.has_results)
            return;

        std::string logic_name = gctx->get_channel_logic_name(chnId);

        if (logic_name == "logic_hook")
        {
            auto st = std::static_pointer_cast<HookState>(snap.logic_state);
            if (st)
                printf("  CH%d [logic_hook] alarm_active=%d latch=%d frame=%dx%d "
                       "age=%lldms\n",
                       chnId, st->alarm_active, st->alarm_sent_latch, snap.frame.cols, snap.frame.rows,
                       (long long)snap.result_age_ms);
        }
        else if (logic_name == "logic_person_alarm")
        {
            auto st = std::static_pointer_cast<PersonAlarmState>(snap.logic_state);
            if (st)
                printf("  CH%d [person_alarm] detected=%d count=%d frame=%dx%d "
                       "age=%lldms\n",
                       chnId, st->person_detected, st->person_count, snap.frame.cols, snap.frame.rows,
                       (long long)snap.result_age_ms);
        }
        else
        {
            printf("  CH%d [%s] results=%zu frame=%dx%d age=%lldms\n", chnId, logic_name.c_str(), snap.results.size(),
                   snap.frame.cols, snap.frame.rows, (long long)snap.result_age_ms);
        }

        for (size_t r = 0; r < snap.results.size() && r < 3; ++r)
        {
            const auto &det = snap.results[r];
            printf("    [%zu] %s  score=%.2f  box=(%d,%d %dx%d)\n", r, det.label.c_str(), det.score, det.box.x,
                   det.box.y, det.box.width, det.box.height);
        }
    });
}
