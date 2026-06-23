/**
 * @file app_ctrl.cpp
 * @brief 应用全局控制块实现 — C/pthread 风格
 *
 * 所有同步使用 pthread 原语, 线程创建由 main 统一管理.
 */
#include "app_ctrl.h"

#include "../analyzer/analyzer.h"
#include "../config/config_registry.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

/* feed_stats_reset 定义在 frame_inlet.cpp, 声明在 analyzer_internal.h (C++
 * linkage). analyzer.h 被 extern "C" 包裹不能放进去, 此处单独前置声明. */
void feed_stats_reset(int chnId);
#include "../capturer/decChannel.h"
#include "../logic/global_logic.h"

/*======================== 全局变量 ========================*/
APP_CTRL *g_pCtrl = nullptr;

/* ---- helpers ---- */
static uint64_t steady_now_ms(void)
{
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

/*======================== 配置热加载线程 (pthread 入口)
 * ========================*/
extern "C" void *config_monitor_thread_func(void *arg)
{
    (void)arg;
    APP_CTRL *ctrl = g_pCtrl;
    if (!ctrl)
        return nullptr;

    uint64_t pendingMtime = 0;
    printf("[ConfigMonitor] Thread started, monitoring: %s\n", ctrl->config.config_path.c_str());

    while (!ctrl->config_monitor_exit)
    {
        /* 带超时的条件变量等待 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;

        pthread_mutex_lock(&ctrl->cv_config_mtx);
        int rc = 0;
        while (!ctrl->config_monitor_exit && rc == 0)
            rc = pthread_cond_timedwait(&ctrl->cv_config, &ctrl->cv_config_mtx, &ts);
        int woke_to_exit = ctrl->config_monitor_exit;
        pthread_mutex_unlock(&ctrl->cv_config_mtx);

        if (woke_to_exit)
            break;

        uint64_t mtime = config_get_mtime(ctrl->config.config_path);
        if (mtime == 0 || mtime == ctrl->configLastMtime)
            continue;

        printf("[ConfigMonitor] File changed: mtime=%lu, last=%lu\n", (unsigned long)mtime,
               (unsigned long)ctrl->configLastMtime);

        if (pendingMtime != mtime)
        {
            printf("[ConfigMonitor] First detection, waiting for file to "
                   "stabilize...\n");
            pendingMtime = mtime;
            continue;
        }
        printf("[ConfigMonitor] File stabilized, reloading config...\n");
        pendingMtime = 0;

        AppConfig new_cfg;
        new_cfg.config_path = "HOTRELOAD";
        if (!load_config(ctrl->config.config_path, new_cfg))
        {
            printf("[ConfigMonitor] ERROR: Failed to load config!\n");
            continue;
        }

        printf("[ConfigReload] Config file changed, reloading...\n");

        /* 更新运行时参数 */
        size_t update_cnt =
            std::min(new_cfg.channels.size(), sizeof(ctrl->channels_state) / sizeof(ctrl->channels_state[0]));

        /* 锁外检测哪些通道需要模型热重载 */
        std::vector<size_t> model_reload_chns;
        for (size_t i = 0; i < update_cnt; ++i)
        {
            if (ctrl->config.channels[i].model_path != new_cfg.channels[i].model_path ||
                ctrl->config.channels[i].model_type != new_cfg.channels[i].model_type ||
                ctrl->config.channels[i].label_path != new_cfg.channels[i].label_path)
            {
                model_reload_chns.push_back(i);
            }
        }

        /* 执行模型热重载 (耗时操作，不持锁) */
        for (size_t idx : model_reload_chns)
            algorithm_reload_channel_model(static_cast<int>(idx), new_cfg.channels[idx]);

        /* 阶段 A: 锁外比对 global_logics */
        bool global_logics_changed = (ctrl->config.global_logics.size() != new_cfg.global_logics.size());
        if (!global_logics_changed)
        {
            for (size_t i = 0; i < new_cfg.global_logics.size(); ++i)
            {
                if (ctrl->config.global_logics[i] != new_cfg.global_logics[i])
                {
                    global_logics_changed = true;
                    break;
                }
            }
        }

        /* 阶段 A: 锁外收集 per-channel logic 切换名单 */
        struct LogicSwitch
        {
            size_t idx;
            std::string new_logic;
            int changed;
        };
        std::vector<LogicSwitch> logic_switches;
        logic_switches.reserve(update_cnt);
        for (size_t i = 0; i < update_cnt; ++i)
        {
            const std::string &old_logic = ctrl->config.channels[i].logic;
            const std::string &new_logic = new_cfg.channels[i].logic;
            logic_switches.push_back({i, new_logic, old_logic != new_logic});
        }

        /* 阶段 B: 写锁内仅做 sync_fields */
        {
            pthread_rwlock_wrlock(&ctrl->mtx);

            g_cfg_reg.sync_fields(&ctrl->config, &new_cfg, true);
            for (size_t i = 0; i < update_cnt; ++i)
                g_cfg_reg.sync_fields(&ctrl->config.channels[i], &new_cfg.channels[i], false);

            if (global_logics_changed)
            {
                printf("[ConfigMonitor] global_logics changed (%zu -> %zu instance(s)), "
                       "will restart global logic threads\n",
                       ctrl->config.global_logics.size(), new_cfg.global_logics.size());
                ctrl->config.global_logics = new_cfg.global_logics;
            }

            /* ROI 区域同步: 不在注册表内, 须显式复制 */
            for (size_t i = 0; i < update_cnt; ++i)
            {
                ctrl->config.channels[i].roi_zones = new_cfg.channels[i].roi_zones;
                ctrl->config.channels[i].roi_polygon = new_cfg.channels[i].roi_polygon;
            }

            pthread_rwlock_unlock(&ctrl->mtx);
        }

        /* 阶段 B2: 重建各通道 ChannelState.roi_zones (需要 inputW/inputH, 可在锁外)
         */
        load_roi_zones_from_config();

        /* 阶段 C: 更新各通道 logic_name / logic_state */
        for (const auto &sw : logic_switches)
        {
            pthread_mutex_lock(&ctrl->chn_mtx[sw.idx]);
            auto &ch_state = ctrl->channels_state[sw.idx];
            ch_state.logic_name = sw.new_logic;
            if (sw.changed)
            {
                /* 切换 logic 时全面清零：自定义变量 + 检测框 + 绘制指令 + 帧计数 */
                ch_state.logic_state.reset();
                ch_state.last_results.clear();
                ch_state.draw_cmds.clear();
                ch_state.logic_frame_id = 0;
                ch_state.result_frame_seq = 0;
                ch_state.last_logic_ts_ms = steady_now_ms();
                printf("[ConfigMonitor] Channel %zu logic switched: -> '%s', all state "
                       "reset\n",
                       sw.idx, sw.new_logic.c_str());
            }
            pthread_mutex_unlock(&ctrl->chn_mtx[sw.idx]);

            /* 锁外重置 tracker 和 FPS 节流（与 offline/online 对齐） */
            if (sw.changed)
            {
                feed_stats_reset(static_cast<int>(sw.idx));
                analyzer_reset_tracker_ids(static_cast<int>(sw.idx));
            }
        }

        /* 锁外重启 global_logic 线程 */
        if (global_logics_changed)
        {
            global_logic_start_all(ctrl->config.global_logics);
            printf("[ConfigMonitor] global_logic threads restarted (%zu instance(s) "
                   "configured)\n",
                   ctrl->config.global_logics.size());
        }

        for (size_t i = 0; i < update_cnt; ++i)
        {
            int runtime_chn = static_cast<int>(i);
            float obj = ctrl->config.channels[i].obj_thresh;
            float nms = ctrl->config.channels[i].nms_thresh;

            printf("[ConfigMonitor] Updating channel %zu: obj=%.2f, nms=%.2f\n", i, obj, nms);

            algorithm_update_thresh(runtime_chn, obj, nms);
            algorithm_update_detect_classes(runtime_chn, ctrl->config.channels[i].detect_classes);
            analyzer_update_tracker(runtime_chn, &ctrl->config.channels[i]);
        }

        for (size_t idx : model_reload_chns)
            analyzer_reset_tracker_ids(static_cast<int>(idx));

        /* ======================== 阶段 D: 流地址热切换 ========================
         * stream 字段不在注册表内 (sync_fields 不触碰), 须在此单独比对。
         * 检测到某通道 stream 变化时: 停旧采集器 → 为剩余共享通道重建 →
         * 为切换通道新建。 复用 DecChannel::init() + busListen 的已有基础设施,
         * 与断流重连共用重连逻辑。 */
        {
            struct StreamSwitch
            {
                int chnId;
                std::string old_loc, new_loc, new_type;
                SrcCfg_t new_src;
            };
            std::vector<StreamSwitch> stream_switches;

            for (size_t i = 0; i < update_cnt; ++i)
            {
                const auto &os = ctrl->config.channels[i].stream;
                const auto &ns = new_cfg.channels[i].stream;
                std::string old_type = config_utils::normalize_src_type(os);
                std::string new_type = config_utils::normalize_src_type(ns);
                std::string old_loc = config_utils::resolve_stream_location(os, old_type);
                std::string new_loc = config_utils::resolve_stream_location(ns, new_type);

                bool changed = (old_loc != new_loc) || (old_type != new_type);
                if (!changed)
                {
                    std::string old_enc = os.video_enc.empty() ? "h264" : config_utils::to_lower_copy(os.video_enc);
                    std::string new_enc = ns.video_enc.empty() ? "h264" : config_utils::to_lower_copy(ns.video_enc);
                    if (old_enc != new_enc)
                        changed = true;
                }
                if (!changed)
                    continue;

                SrcCfg_t src;
                src.srcType = new_type;
                src.location = new_loc;
                src.videoEncType = ns.video_enc.empty() ? "h264" : config_utils::to_lower_copy(ns.video_enc);
                src.loop = ns.loop;
                stream_switches.push_back({(int)i, old_loc, new_loc, new_type, src});
            }

            for (auto &sw : stream_switches)
            {
                int chnId = sw.chnId;
                printf("[ConfigMonitor] Channel %d stream changed: %s -> %s\n", chnId, sw.old_loc.c_str(),
                       sw.new_loc.c_str());

                /* D1. 定位当前服务此通道的采集器（可能是自有, 也可能是共享别人的） */
                DecChannel *old_cap = ctrl->capturers[chnId];
                int old_slot = chnId;
                if (!old_cap)
                {
                    for (int j = 0; j < APP_CTRL_MAX_CAPTURERS; ++j)
                    {
                        if (ctrl->capturers[j] && ctrl->capturers[j]->hasChannel(chnId))
                        {
                            old_cap = ctrl->capturers[j];
                            old_slot = j;
                            break;
                        }
                    }
                }

                /* D2. 停旧采集器, 取出共享通道列表
                 * 必须整体 stop: chnIds 由 GStreamer 线程并发读取, 不能在运行中修改。
                 * stop() 会等 bus 线程退出(已加 mStopRequested 加速), 之后安全读取
                 * chnIds。 */
                std::vector<int> remaining; /* 同一采集器上未切换的其他通道 */
                if (old_cap)
                {
                    std::vector<int> all_ids = old_cap->mGstChn.chnIds; /* stop 前拷贝 */

                    for (int cid : all_ids)
                        analyzer_channel_offline(cid);

                    old_cap->stop();
                    delete old_cap;
                    ctrl->capturers[old_slot] = nullptr;
                    ctrl->capturer_count--;

                    /* 筛选：仍需旧地址的通道（排除也在切换列表里的） */
                    for (int id : all_ids)
                    {
                        if (id == chnId)
                            continue;
                        bool also_switching = false;
                        for (const auto &s2 : stream_switches)
                            if (s2.chnId == id)
                            {
                                also_switching = true;
                                break;
                            }
                        if (!also_switching)
                            remaining.push_back(id);
                    }
                }
                else
                {
                    /* 该通道原来没有采集器（location 为空等），仅标记离线 */
                    analyzer_channel_offline(chnId);
                }

                /* D3. 为 remaining 通道重建旧地址采集器（共享场景, 通常 remaining
                 * 为空） */
                if (!remaining.empty())
                {
                    int primary = remaining[0];
                    const auto &ps = ctrl->config.channels[primary].stream;
                    SrcCfg_t old_src;
                    old_src.srcType = config_utils::normalize_src_type(ps);
                    old_src.location = config_utils::resolve_stream_location(ps, old_src.srcType);
                    old_src.videoEncType = ps.video_enc.empty() ? "h264" : config_utils::to_lower_copy(ps.video_enc);
                    old_src.loop = ps.loop;

                    DecChannel *rebuild = new DecChannel(primary, old_src);
                    for (size_t k = 1; k < remaining.size(); ++k)
                        rebuild->addTargetChannel(remaining[k]);

                    if (rebuild->init() == 0)
                    {
                        ctrl->capturers[primary] = rebuild;
                        ctrl->capturer_count++;
                        for (int rid : remaining)
                            analyzer_channel_online(rid);
                        printf("[ConfigMonitor] Rebuilt shared capturer for ch%d (+%zu "
                               "shared)\n",
                               primary, remaining.size() - 1);
                    }
                    else
                    {
                        fprintf(stderr, "[ConfigMonitor] WARN: rebuild capturer failed for ch%d\n", primary);
                        delete rebuild;
                    }
                }

                /* D4. 以新地址创建采集器 */
                if (!sw.new_src.location.empty())
                {
                    DecChannel *nc = new DecChannel(chnId, sw.new_src);
                    if (nc->init() == 0)
                    {
                        ctrl->capturers[chnId] = nc;
                        ctrl->capturer_count++;
                        analyzer_channel_online(chnId);
                        printf("[ConfigMonitor] Channel %d new capturer: %s (%s)\n", chnId, sw.new_loc.c_str(),
                               sw.new_type.c_str());
                    }
                    else
                    {
                        fprintf(stderr, "[ConfigMonitor] Channel %d capturer init failed: %s\n", chnId,
                                sw.new_loc.c_str());
                        delete nc;
                    }
                }
                else
                {
                    printf("[ConfigMonitor] Channel %d new stream location empty, "
                           "channel stays offline\n",
                           chnId);
                }

                /* D5. 更新 config 中的 stream 字段 (sync_fields 不覆盖此区域) */
                pthread_rwlock_wrlock(&ctrl->mtx);
                ctrl->config.channels[chnId].stream = new_cfg.channels[chnId].stream;
                pthread_rwlock_unlock(&ctrl->mtx);
            }

            if (!stream_switches.empty())
                printf("[ConfigMonitor] Stream hot-switch done: %zu channel(s) rebuilt\n", stream_switches.size());
        }

        ctrl->configLastMtime = mtime;
    }
    return nullptr;
}

/*======================== 初始化 ========================*/
int app_ctrl_init(const char *cfgPath)
{
    if (g_pCtrl)
        return 0;

    g_pCtrl = new APP_CTRL();
    if (!g_pCtrl)
        return -1;

    g_pCtrl->magic = APP_CTRL_MAGIC;

    std::string path(cfgPath);
    if (!load_config(path, g_pCtrl->config))
    {
        delete g_pCtrl;
        g_pCtrl = nullptr;
        return -1;
    }

    /* 初始化 pthread 同步原语 */
    pthread_rwlock_init(&g_pCtrl->mtx, nullptr);
    pthread_mutex_init(&g_pCtrl->cv_config_mtx, nullptr);
    pthread_cond_init(&g_pCtrl->cv_config, nullptr);
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_mutex_init(&g_pCtrl->chn_mtx[i], nullptr);

    /* 初始化通道状态时间戳 */
    uint64_t now_ms = steady_now_ms();
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        g_pCtrl->channels_state[i].last_fps_ts_ms = now_ms;
        g_pCtrl->channels_state[i].last_infer_ts_ms = now_ms;
        g_pCtrl->channels_state[i].last_result_ts_ms = now_ms;
        g_pCtrl->channels_state[i].last_logic_ts_ms = now_ms;
    }

    g_pCtrl->b_init = 1;
    g_pCtrl->isRunning = 1;
    g_pCtrl->pDispBuffer = nullptr;
    g_pCtrl->capturer_count = 0;
    for (int i = 0; i < APP_CTRL_MAX_CAPTURERS; ++i)
        g_pCtrl->capturers[i] = nullptr;

    g_pCtrl->configLastMtime = config_get_mtime(path);

    /* 线程退出标志: 0=运行中, 1=请求退出 (由 main 管理) */
    g_pCtrl->config_monitor_exit = 0;
    g_pCtrl->fd_monitor_exit = 0;
    g_pCtrl->upload_worker_exit = 0;
    g_pCtrl->disp_thread_exit = 0;

    return 0;
}

/*======================== 反初始化 ========================*/
void app_ctrl_deinit(void)
{
    if (!g_pCtrl)
        return;

    g_pCtrl->isRunning = 0;

    /* 唤醒 config_monitor 使其退出 */
    g_pCtrl->config_monitor_exit = 1;
    pthread_mutex_lock(&g_pCtrl->cv_config_mtx);
    pthread_cond_broadcast(&g_pCtrl->cv_config);
    pthread_mutex_unlock(&g_pCtrl->cv_config_mtx);

    /* 销毁同步原语 */
    pthread_rwlock_destroy(&g_pCtrl->mtx);
    pthread_mutex_destroy(&g_pCtrl->cv_config_mtx);
    pthread_cond_destroy(&g_pCtrl->cv_config);
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_mutex_destroy(&g_pCtrl->chn_mtx[i]);

    delete g_pCtrl;
    g_pCtrl = nullptr;
}

/*======================== 通道数据查询 ========================*/
std::vector<AlgoResult> app_ctrl_get_results(int chnId)
{
    if (!g_pCtrl || chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return {};
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    std::vector<AlgoResult> out = g_pCtrl->channels_state[chnId].last_results;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    return out;
}

std::vector<AlgoResult> app_ctrl_get_results_fresh(int chnId, int max_age_ms)
{
    if (!g_pCtrl || chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return {};
    if (max_age_ms <= 0)
        return app_ctrl_get_results(chnId);

    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    auto &cs = g_pCtrl->channels_state[chnId];
    uint64_t now = steady_now_ms();
    int64_t age_ms = (int64_t)(now - cs.last_result_ts_ms);
    if (age_ms > max_age_ms)
    {
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        return {};
    }
    std::vector<AlgoResult> out = cs.last_results;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    return out;
}

float app_ctrl_get_disp_fps(int chnId)
{
    if (!g_pCtrl || chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return 0.0f;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    float v = g_pCtrl->channels_state[chnId].disp_fps;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    return v;
}

float app_ctrl_get_infer_fps(int chnId)
{
    return algorithm_get_infer_fps(chnId);
}

int app_ctrl_get_target_count(int chnId, const char *label, int max_age_ms)
{
    auto results = app_ctrl_get_results_fresh(chnId, max_age_ms);
    std::string s(label);
    int n = 0;
    for (const auto &r : results)
        if (r.label == s)
            ++n;
    return n;
}

int app_ctrl_has_target(int chnId, const char *label, int max_age_ms)
{
    auto results = app_ctrl_get_results_fresh(chnId, max_age_ms);
    std::string s(label);
    for (const auto &r : results)
        if (r.label == s)
            return 1;
    return 0;
}

uint64_t app_ctrl_get_last_infer_ts_ms(int chnId)
{
    if (!g_pCtrl || chnId < 0 || chnId >= app_ctrl_get_chn_nums())
        return 0;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    uint64_t v = g_pCtrl->channels_state[chnId].last_infer_ts_ms;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    return v;
}

std::string app_ctrl_get_logic_name(int chnId)
{
    if (!g_pCtrl || chnId < 0 || chnId >= app_ctrl_get_chn_nums())
        return {};
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    std::string v = g_pCtrl->channels_state[chnId].logic_name;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    return v;
}

int app_ctrl_get_channel_snapshot(int chnId, ChannelSnapshot *out)
{
    if (!g_pCtrl || !out || chnId < 0 || chnId >= app_ctrl_get_chn_nums())
        return 0;

    out->infer_fps = algorithm_get_infer_fps(chnId);

    cv::Mat frame_shallow;
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        const auto &cs = g_pCtrl->channels_state[chnId];
        frame_shallow = cs.last_logic_frame;
        out->results = cs.last_results;
        out->logic_state = cs.logic_state;
        out->disp_fps = cs.disp_fps;
        out->logic_frame_id = cs.logic_frame_id;
        out->frame_seq = cs.result_frame_seq; /* frame 与 results 同帧序号 */
        out->has_results = !cs.last_results.empty();
        out->result_age_ms = (int64_t)(steady_now_ms() - cs.last_result_ts_ms);
        out->online_state = cs.online_state;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }
    out->frame = frame_shallow.clone();
    return 1;
}
