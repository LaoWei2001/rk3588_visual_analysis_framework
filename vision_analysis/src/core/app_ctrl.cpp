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

#include "../capturer/decChannel.h"
#include "logic/core/global_logic.h"

/*======================== 全局变量 ========================*/
APP_CTRL *g_pCtrl = nullptr;

/* 配置采用原子替换写入；250ms 轮询并连续确认两次即可兼顾低延迟和文件稳定性。 */
static constexpr long CONFIG_MONITOR_INTERVAL_MS = 250;

/* ---- helpers ---- */
static uint64_t steady_now_ms(void)
{
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

static bool model_runtime_changed(const ChannelConfig &old_channel, const ChannelConfig &new_channel)
{
    return old_channel.infer_enable != new_channel.infer_enable || old_channel.models != new_channel.models ||
           old_channel.threads != new_channel.threads;
}

static void restore_model_runtime(ChannelConfig &channel, const ChannelConfig &old_channel)
{
    channel.infer_enable = old_channel.infer_enable;
    channel.models = old_channel.models;
    channel.threads = old_channel.threads;
}

static bool roi_config_equal(const std::vector<RoiZoneConfig> &lhs, const std::vector<RoiZoneConfig> &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i)
        if (lhs[i].name != rhs[i].name || lhs[i].polygon != rhs[i].polygon)
            return false;
    return true;
}

static bool logic_runtime_changed(const ChannelConfig &old_channel, const ChannelConfig &new_channel)
{
    return old_channel.logic != new_channel.logic || !roi_config_equal(old_channel.roi_zones, new_channel.roi_zones);
}

std::shared_ptr<const AppRuntimeSnapshot> app_ctrl_build_runtime_snapshot(const AppConfig &config, int input_w,
                                                                          int input_h, uint64_t generation)
{
    auto snapshot = std::make_shared<AppRuntimeSnapshot>();
    snapshot->config = config;
    snapshot->generation = generation;
    std::fill(std::begin(snapshot->channel_config_index), std::end(snapshot->channel_config_index), -1);

    for (size_t config_index = 0; config_index < config.channels.size(); ++config_index)
    {
        const ChannelConfig &channel = config.channels[config_index];
        if (channel.id < 0 || channel.id >= MAX_CHANNEL_NUM)
            continue;
        snapshot->channel_config_index[channel.id] = static_cast<int>(config_index);

        std::vector<LogicParameterError> parameter_errors;
        if (!logic_parameters_resolve(channel.logic, channel.logic_parameters_json, nullptr,
                                      &snapshot->logic_parameters[channel.id], &parameter_errors))
        {
            fprintf(stderr, "[RuntimeSnapshot] channel %d logic parameter resolution failed:\n", channel.id);
            for (const auto &error : parameter_errors)
                fprintf(stderr, "  - %s: %s\n", error.field.c_str(), error.message.c_str());
            return std::shared_ptr<const AppRuntimeSnapshot>();
        }

        /* ROI 配置统一是归一化坐标；运行快照在发布前一次性转换到模型坐标系。 */
        auto &runtime_rois = snapshot->roi_zones[channel.id];
        auto add_zone = [&](const std::string &name, const std::vector<std::pair<double, double>> &polygon) {
            size_t point_count = polygon.size();
            while (point_count > 1 && polygon.front() == polygon[point_count - 1])
                --point_count;
            if (point_count < 3 || input_w <= 0 || input_h <= 0)
                return;
            RoiZone zone;
            zone.name = name;
            zone.polygon.reserve(point_count);
            for (size_t i = 0; i < point_count; ++i)
            {
                const auto &point = polygon[i];
                zone.polygon.emplace_back(static_cast<int>(point.first * input_w + 0.5),
                                          static_cast<int>(point.second * input_h + 0.5));
            }
            runtime_rois.push_back(std::move(zone));
        };

        for (const auto &zone : channel.roi_zones)
            add_zone(zone.name, zone.polygon);
    }
    return snapshot;
}

void app_ctrl_store_runtime_snapshot(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot)
{
    APP_CTRL *ctrl = g_pCtrl;
    if (!ctrl || !snapshot)
        return;
    std::atomic_store_explicit(&ctrl->runtime_snapshot, snapshot, std::memory_order_release);
}

std::shared_ptr<const AppRuntimeSnapshot> app_ctrl_get_runtime_snapshot(void)
{
    APP_CTRL *ctrl = g_pCtrl;
    return ctrl ? std::atomic_load_explicit(&ctrl->runtime_snapshot, std::memory_order_acquire)
                : std::shared_ptr<const AppRuntimeSnapshot>();
}

const ChannelConfig *app_ctrl_runtime_channel_config(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot,
                                                     int channel_id)
{
    if (!snapshot || channel_id < 0 || channel_id >= MAX_CHANNEL_NUM)
        return nullptr;
    const int config_index = snapshot->channel_config_index[channel_id];
    if (config_index < 0 || config_index >= static_cast<int>(snapshot->config.channels.size()))
        return nullptr;
    const ChannelConfig &channel = snapshot->config.channels[config_index];
    return channel.id == channel_id ? &channel : nullptr;
}

const std::vector<RoiZone> *app_ctrl_runtime_channel_rois(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot,
                                                          int channel_id)
{
    return app_ctrl_runtime_channel_config(snapshot, channel_id) ? &snapshot->roi_zones[channel_id] : nullptr;
}

const LogicParameterSet *app_ctrl_runtime_logic_parameters(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot,
                                                           int channel_id)
{
    return app_ctrl_runtime_channel_config(snapshot, channel_id) ? &snapshot->logic_parameters[channel_id] : nullptr;
}

/*======================== 配置热加载线程 (pthread 入口) ========================*/
extern "C" void *config_monitor_thread_func(void *arg)
{
    (void)arg;
    APP_CTRL *ctrl = g_pCtrl;
    if (!ctrl)
        return nullptr;

    uint64_t pendingMtime = 0;
    printf("[ConfigMonitor] Thread started, monitoring: %s\n", ctrl->config.config_path.c_str());

    while (!ctrl->config_monitor_exit.load())
    {
        /* 带超时的条件变量等待：普通参数通常在保存后 250~500ms 进入热重载。 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += CONFIG_MONITOR_INTERVAL_MS / 1000;
        ts.tv_nsec += (CONFIG_MONITOR_INTERVAL_MS % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&ctrl->cv_config_mtx);
        int rc = 0;
        while (!ctrl->config_monitor_exit.load() && rc == 0)
            rc = pthread_cond_timedwait(&ctrl->cv_config, &ctrl->cv_config_mtx, &ts);
        const bool woke_to_exit = ctrl->config_monitor_exit.load();
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
            printf("[ConfigMonitor] First detection, waiting for file to stabilize...\n");
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

        /* 本轮所有 diff 都基于同一份旧配置，避免监控线程自己在分阶段提交后
         * 又拿“半新半旧”的 ctrl->config 做后续比较。 */
        AppConfig old_cfg;
        pthread_rwlock_rdlock(&ctrl->mtx);
        old_cfg = ctrl->config;
        pthread_rwlock_unlock(&ctrl->mtx);

        /* Channel topology is created at startup (decoders, queues, threads and
         * ChannelState). Hot reload may update fields, but must not resize or
         * reorder that topology. */
        bool channel_topology_changed = new_cfg.channels.size() != old_cfg.channels.size();
        if (!channel_topology_changed)
        {
            for (size_t i = 0; i < new_cfg.channels.size(); ++i)
            {
                if (new_cfg.channels[i].id != old_cfg.channels[i].id ||
                    new_cfg.channels[i].enable != old_cfg.channels[i].enable)
                {
                    channel_topology_changed = true;
                    break;
                }
            }
        }
        const bool output_topology_changed =
            new_cfg.enable_display != old_cfg.enable_display || new_cfg.enable_rtsp != old_cfg.enable_rtsp ||
            new_cfg.disp_width != old_cfg.disp_width || new_cfg.disp_height != old_cfg.disp_height ||
            new_cfg.tile_cols != old_cfg.tile_cols || new_cfg.tile_rows != old_cfg.tile_rows ||
            new_cfg.enable_pause_key != old_cfg.enable_pause_key || new_cfg.rtsp_port != old_cfg.rtsp_port ||
            new_cfg.rtsp_path != old_cfg.rtsp_path || new_cfg.rtsp_fps != old_cfg.rtsp_fps ||
            new_cfg.rtsp_bitrate != old_cfg.rtsp_bitrate || new_cfg.rtsp_codec != old_cfg.rtsp_codec ||
            new_cfg.rtsp_encoder != old_cfg.rtsp_encoder;
        if (channel_topology_changed || output_topology_changed)
        {
            fprintf(stderr, "[ConfigMonitor] Hot reload rejected: channel topology or display/RTSP layout/settings "
                            "cannot change; restart required\n");
            ctrl->configLastMtime = mtime;
            continue;
        }

        /* 模块参数的重载影响由各自 logic.json Schema 声明，不再把新参数名
         * 硬编码进本文件。restart_required 在任何模型/流副作用发生前整轮拒绝。 */
        const size_t parameter_compare_count = std::min(old_cfg.channels.size(), new_cfg.channels.size());
        std::vector<LogicReloadImpact> logic_parameter_impacts(parameter_compare_count, LogicReloadImpact::NONE);
        std::vector<std::vector<std::string>> logic_parameter_changed_keys(parameter_compare_count);
        bool logic_parameter_restart_required = false;
        for (size_t i = 0; i < parameter_compare_count; ++i)
        {
            const ChannelConfig &old_channel = old_cfg.channels[i];
            const ChannelConfig &new_channel = new_cfg.channels[i];
            if (old_channel.logic != new_channel.logic)
                continue;
            logic_parameter_impacts[i] =
                logic_parameters_reload_impact(new_channel.logic, old_channel.logic_parameters_json,
                                               new_channel.logic_parameters_json, &logic_parameter_changed_keys[i]);
            if (logic_parameter_impacts[i] == LogicReloadImpact::RESTART_REQUIRED)
            {
                logic_parameter_restart_required = true;
                fprintf(stderr, "[ConfigMonitor] channel %d logic %s parameter change requires restart", new_channel.id,
                        new_channel.logic.c_str());
                for (const auto &key : logic_parameter_changed_keys[i])
                    fprintf(stderr, " %s", key.c_str());
                fprintf(stderr, "\n");
            }
        }

        bool global_parameter_restart_required = false;
        for (const auto &next_global : new_cfg.global_logics)
        {
            const auto previous = std::find_if(old_cfg.global_logics.begin(), old_cfg.global_logics.end(),
                                               [&](const GlobalLogicConfig &item) {
                                                   return item.instance_id == next_global.instance_id;
                                               });
            if (previous == old_cfg.global_logics.end() || !previous->enable || !next_global.enable ||
                previous->logic != next_global.logic)
                continue;
            std::vector<std::string> changed_keys;
            const LogicReloadImpact impact = logic_parameters_reload_impact(
                next_global.logic, previous->logic_parameters_json, next_global.logic_parameters_json, &changed_keys);
            if (impact == LogicReloadImpact::RESTART_REQUIRED)
            {
                global_parameter_restart_required = true;
                fprintf(stderr, "[ConfigMonitor] global instance %s logic %s parameter change requires restart",
                        next_global.instance_id.c_str(), next_global.logic.c_str());
                for (const auto &key : changed_keys)
                    fprintf(stderr, " %s", key.c_str());
                fprintf(stderr, "\n");
            }
        }

        if (logic_parameter_restart_required || global_parameter_restart_required)
        {
            fprintf(stderr, "[ConfigMonitor] Hot reload rejected: module parameter marked restart_required\n");
            ctrl->configLastMtime = mtime;
            continue;
        }

        printf("[ConfigReload] Config file changed, reloading...\n");

        /* 更新运行时参数 */
        size_t update_cnt =
            std::min(new_cfg.channels.size(), sizeof(ctrl->channels_state) / sizeof(ctrl->channels_state[0]));

        /* 锁外检测哪些通道需要模型热重载 */
        std::vector<size_t> model_reload_indices;
        for (size_t i = 0; i < update_cnt; ++i)
        {
            if (model_runtime_changed(old_cfg.channels[i], new_cfg.channels[i]))
            {
                model_reload_indices.push_back(i);
            }
        }

        /* 执行模型热重载 (耗时操作，不持配置锁)。失败的通道只回退模型相关字段，
         * 其它 logic/ROI/上报参数仍可正常热更新。 */
        std::vector<unsigned char> model_reload_applied(update_cnt, 0);
        for (size_t config_index : model_reload_indices)
        {
            if (algorithm_reload_channel_model(new_cfg.channels[config_index].id, new_cfg.channels[config_index]))
            {
                model_reload_applied[config_index] = 1;
            }
            else
            {
                fprintf(stderr, "[ConfigMonitor] ch%d model change rejected; keeping previous active model config\n",
                        new_cfg.channels[config_index].id);
                restore_model_runtime(new_cfg.channels[config_index], old_cfg.channels[config_index]);
            }
        }

        /* 阶段 A: 锁外比对 global_logics */
        bool global_logics_changed = (old_cfg.global_logics.size() != new_cfg.global_logics.size());
        if (!global_logics_changed)
        {
            for (const auto &next_global : new_cfg.global_logics)
            {
                const auto previous = std::find_if(old_cfg.global_logics.begin(), old_cfg.global_logics.end(),
                                                   [&](const GlobalLogicConfig &item) {
                                                       return item.instance_id == next_global.instance_id;
                                                   });
                if (previous == old_cfg.global_logics.end() || *previous != next_global)
                {
                    global_logics_changed = true;
                    break;
                }
            }
        }

        /* 阶段 A: 锁外收集 per-channel logic 切换名单 */
        struct LogicSwitch
        {
            int channel_id;
            std::string new_logic;
            bool reset_state;
            bool logic_name_changed;
            LogicReloadImpact parameter_impact;
            std::vector<std::string> parameter_keys;
        };
        std::vector<LogicSwitch> logic_switches;
        logic_switches.reserve(update_cnt);
        for (size_t i = 0; i < update_cnt; ++i)
        {
            const std::string &old_logic = old_cfg.channels[i].logic;
            const std::string &new_logic = new_cfg.channels[i].logic;
            const LogicReloadImpact parameter_impact =
                i < logic_parameter_impacts.size() ? logic_parameter_impacts[i] : LogicReloadImpact::NONE;
            logic_switches.push_back({new_cfg.channels[i].id, new_logic,
                                      logic_runtime_changed(old_cfg.channels[i], new_cfg.channels[i]) ||
                                          parameter_impact == LogicReloadImpact::RESET_STATE,
                                      old_logic != new_logic, parameter_impact,
                                      i < logic_parameter_changed_keys.size() ? logic_parameter_changed_keys[i]
                                                                              : std::vector<std::string>()});
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
                       "will reconcile changed instances\n",
                       ctrl->config.global_logics.size(), new_cfg.global_logics.size());
                ctrl->config.global_logics = new_cfg.global_logics;
            }

            /* ROI、模型及 report_policy 派生字段不在注册表内，须显式复制。 */
            for (size_t i = 0; i < update_cnt; ++i)
            {
                ctrl->config.channels[i].roi_zones = new_cfg.channels[i].roi_zones;
                ctrl->config.channels[i].models = new_cfg.channels[i].models;
                ctrl->config.channels[i].event_video = new_cfg.channels[i].event_video;
            }

            pthread_rwlock_unlock(&ctrl->mtx);
        }

        /* 模型/logic/ROI 参数先发布，避免后面的流重建耗时期间出现“新模型已经
         * 工作、上层 logic 仍拿旧配置”的长窗口。stream 尚未写入 ctrl->config，
         * 因而这一代快照仍准确描述当前旧采集源。 */
        std::vector<int> logic_changed_channels;
        for (const auto &sw : logic_switches)
            if (sw.reset_state)
                logic_changed_channels.push_back(sw.channel_id);

        std::vector<int> tracker_reset_channels;
        for (size_t config_index : model_reload_indices)
            if (model_reload_applied[config_index])
                tracker_reset_channels.push_back(new_cfg.channels[config_index].id);

        AppConfig base_applied_cfg;
        uint64_t base_generation = 0;
        pthread_rwlock_wrlock(&ctrl->mtx);
        base_applied_cfg = ctrl->config;
        base_generation = ++ctrl->config_generation;
        pthread_rwlock_unlock(&ctrl->mtx);

        for (const auto &channel : base_applied_cfg.channels)
        {
            printf("[ConfigMonitor] Updating channel %d model filters (%zu configured)\n", channel.id,
                   channel.models.size());
            algorithm_update_thresh(channel.id, channel);
            algorithm_update_detect_classes(channel.id, channel);
        }
        algorithm_update_queue_size(base_applied_cfg.queue_size);

        if (!analyzer_publish_runtime_snapshot(base_applied_cfg, base_generation, logic_changed_channels,
                                               tracker_reset_channels))
        {
            fprintf(stderr, "[ConfigMonitor] ERROR: base runtime snapshot publish failed\n");
            continue;
        }

        for (const auto &sw : logic_switches)
        {
            if (sw.parameter_impact != LogicReloadImpact::NONE)
            {
                printf("[ConfigMonitor] Channel %d module parameters updated (%s):", sw.channel_id,
                       logic_reload_impact_name(sw.parameter_impact));
                for (const auto &key : sw.parameter_keys)
                    printf(" %s", key.c_str());
                printf("\n");
            }
            if (sw.reset_state)
                printf("[ConfigMonitor] Channel %d logic %s: '%s', state reset\n", sw.channel_id,
                       sw.logic_name_changed ? "switched" : "configuration changed", sw.new_logic.c_str());
        }

        if (global_logics_changed)
        {
            const int running = global_logic_reload_all(base_applied_cfg.global_logics);
            printf("[ConfigMonitor] global_logic instances reconciled (%d running, %zu configured)\n", running,
                   base_applied_cfg.global_logics.size());
        }

        bool stream_runtime_changed = false;
        /* ======================== 阶段 D: 流地址热切换 ========================
         * stream 字段不在注册表内 (sync_fields 不触碰), 须在此单独比对。
         * 检测到某通道 stream 变化时: 停旧采集器 → 为剩余共享通道重建 → 为切换通道新建。
         * 复用 DecChannel::init() + busListen 的已有基础设施, 与断流重连共用重连逻辑。 */
        {
            struct StreamSwitch
            {
                size_t config_index;
                int channel_id;
                std::string old_loc, new_loc, old_type, new_type;
                SrcCfg_t old_src, new_src;
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
                if (!changed && (os.loop != ns.loop || os.usb_width != ns.usb_width || os.usb_height != ns.usb_height))
                    changed = true;
                if (!changed && old_type == "usb" &&
                    (old_cfg.channels[i].playback_fps != new_cfg.channels[i].playback_fps ||
                     old_cfg.channels[i].max_fps != new_cfg.channels[i].max_fps))
                    changed = true;
                if (!changed)
                    continue;

                SrcCfg_t src;
                src.srcType = new_type;
                src.location = new_loc;
                src.videoEncType = ns.video_enc.empty() ? "h264" : config_utils::to_lower_copy(ns.video_enc);
                src.loop = ns.loop;
                src.usb_width = ns.usb_width;
                src.usb_height = ns.usb_height;
                src.usb_fps = new_cfg.channels[i].playback_fps > 0 ? new_cfg.channels[i].playback_fps
                                                                   : new_cfg.channels[i].max_fps;
                SrcCfg_t old_src;
                old_src.srcType = old_type;
                old_src.location = old_loc;
                old_src.videoEncType = os.video_enc.empty() ? "h264" : config_utils::to_lower_copy(os.video_enc);
                old_src.loop = os.loop;
                old_src.usb_width = os.usb_width;
                old_src.usb_height = os.usb_height;
                old_src.usb_fps = old_cfg.channels[i].playback_fps > 0 ? old_cfg.channels[i].playback_fps
                                                                       : old_cfg.channels[i].max_fps;
                stream_switches.push_back(
                    {i, new_cfg.channels[i].id, old_loc, new_loc, old_type, new_type, old_src, src});
            }

            for (auto &sw : stream_switches)
            {
                const int chnId = sw.channel_id;
                printf("[ConfigMonitor] Channel %d stream changed: %s -> %s\n", chnId, sw.old_loc.c_str(),
                       sw.new_loc.c_str());

                /* D1. 定位当前服务此通道的采集器（可能是自有, 也可能是共享别人的） */
                DecChannel *old_cap = ctrl->capturers[chnId];
                int old_owner_id = chnId;
                if (!old_cap)
                {
                    for (int j = 0; j < APP_CTRL_MAX_CAPTURERS; ++j)
                    {
                        if (ctrl->capturers[j] && ctrl->capturers[j]->hasChannel(chnId))
                        {
                            old_cap = ctrl->capturers[j];
                            old_owner_id = j;
                            break;
                        }
                    }
                }

                /* D2. 停旧采集器, 取出共享通道列表
                 * 必须整体 stop: chnIds 由 GStreamer 线程并发读取, 不能在运行中修改。
                 * stop() 会等 bus 线程退出(已加 mStopRequested 加速), 之后安全读取 chnIds。 */
                std::vector<int> remaining; /* 同一采集器上未切换的其他通道 */
                if (old_cap)
                {
                    std::vector<int> all_ids = old_cap->mGstChn.chnIds; /* stop 前拷贝 */

                    for (int cid : all_ids)
                        analyzer_channel_offline(cid);

                    old_cap->stop();
                    delete old_cap;
                    ctrl->capturers[old_owner_id] = nullptr;
                    ctrl->capturer_count--;

                    /* 筛选：仍需旧地址的通道（排除也在切换列表里的） */
                    for (int id : all_ids)
                    {
                        if (id == chnId)
                            continue;
                        bool also_switching = false;
                        for (const auto &s2 : stream_switches)
                            if (s2.channel_id == id)
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

                /* D3. 为 remaining 通道重建旧地址采集器（共享场景, 通常 remaining 为空） */
                if (!remaining.empty())
                {
                    int primary = remaining[0];
                    const ChannelConfig *primary_config = nullptr;
                    for (const auto &channel : ctrl->config.channels)
                        if (channel.id == primary)
                        {
                            primary_config = &channel;
                            break;
                        }
                    if (!primary_config)
                        continue;
                    const auto &ps = primary_config->stream;
                    SrcCfg_t old_src;
                    old_src.srcType = config_utils::normalize_src_type(ps);
                    old_src.location = config_utils::resolve_stream_location(ps, old_src.srcType);
                    old_src.videoEncType = ps.video_enc.empty() ? "h264" : config_utils::to_lower_copy(ps.video_enc);
                    old_src.loop = ps.loop;
                    old_src.usb_width = ps.usb_width;
                    old_src.usb_height = ps.usb_height;
                    old_src.usb_fps =
                        primary_config->playback_fps > 0 ? primary_config->playback_fps : primary_config->max_fps;

                    DecChannel *rebuild = new DecChannel(primary, old_src);
                    for (size_t k = 1; k < remaining.size(); ++k)
                        rebuild->addTargetChannel(remaining[k]);

                    if (rebuild->init() == 0)
                    {
                        ctrl->capturers[primary] = rebuild;
                        ctrl->capturer_count++;
                        for (int rid : remaining)
                            analyzer_channel_online(rid);
                        printf("[ConfigMonitor] Rebuilt shared capturer for ch%d (+%zu shared)\n", primary,
                               remaining.size() - 1);
                    }
                    else
                    {
                        fprintf(stderr, "[ConfigMonitor] WARN: rebuild capturer failed for ch%d\n", primary);
                        delete rebuild;
                    }
                }

                /* D4. 以新地址创建采集器 */
                bool stream_applied = false;
                if (!sw.new_src.location.empty())
                {
                    DecChannel *nc = new DecChannel(chnId, sw.new_src);
                    if (nc->init() == 0)
                    {
                        ctrl->capturers[chnId] = nc;
                        ctrl->capturer_count++;
                        analyzer_channel_online(chnId);
                        stream_applied = true;
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
                    stream_applied = true;
                    printf("[ConfigMonitor] Channel %d new stream location empty, channel stays offline\n", chnId);
                }

                /* 新源启动失败时尽力恢复旧源，且绝不把失败的新 stream 发布给上层。 */
                if (!stream_applied && !sw.old_src.location.empty())
                {
                    DecChannel *rollback = new DecChannel(chnId, sw.old_src);
                    if (rollback->init() == 0)
                    {
                        ctrl->capturers[chnId] = rollback;
                        ctrl->capturer_count++;
                        analyzer_channel_online(chnId);
                        printf("[ConfigMonitor] Channel %d rolled back to previous stream: %s\n", chnId,
                               sw.old_loc.c_str());
                    }
                    else
                    {
                        fprintf(stderr, "[ConfigMonitor] CRITICAL: Channel %d old stream rollback failed: %s\n", chnId,
                                sw.old_loc.c_str());
                        delete rollback;
                    }
                }

                if (stream_applied)
                {
                    pthread_rwlock_wrlock(&ctrl->mtx);
                    ctrl->config.channels[sw.config_index].stream = new_cfg.channels[sw.config_index].stream;
                    pthread_rwlock_unlock(&ctrl->mtx);
                    stream_runtime_changed = true;
                }
            }

            if (!stream_switches.empty())
                printf("[ConfigMonitor] Stream hot-switch done: %zu channel(s) rebuilt\n", stream_switches.size());
        }

        /* 阶段 E: 流切换完成后再发布一代快照，仅更新最终成功应用的 stream 字段。
         * 失败的新源未写入 ctrl->config，因此上层仍会看到旧源配置。 */
        if (stream_runtime_changed)
        {
            AppConfig applied_cfg;
            uint64_t generation = 0;
            pthread_rwlock_wrlock(&ctrl->mtx);
            applied_cfg = ctrl->config;
            generation = ++ctrl->config_generation;
            pthread_rwlock_unlock(&ctrl->mtx);

            if (!analyzer_publish_runtime_snapshot(applied_cfg, generation))
            {
                fprintf(stderr, "[ConfigMonitor] ERROR: stream runtime snapshot publish failed\n");
                continue;
            }
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
        g_pCtrl->channels_state[i].last_logic_ts_ms = now_ms;
    }

    /* analyzer 初始化模型前 input 尺寸尚未知，先发布无 ROI 的启动快照；
     * analyzer_init 随后会以真实 inputW/inputH 原子替换它。 */
    app_ctrl_store_runtime_snapshot(app_ctrl_build_runtime_snapshot(g_pCtrl->config, 0, 0, g_pCtrl->config_generation));

    g_pCtrl->b_init = 1;
    g_pCtrl->isRunning.store(true);
    g_pCtrl->pDispBuffer = nullptr;
    g_pCtrl->capturer_count = 0;
    for (int i = 0; i < APP_CTRL_MAX_CAPTURERS; ++i)
        g_pCtrl->capturers[i] = nullptr;

    g_pCtrl->configLastMtime = config_get_mtime(path);

    /* 线程退出标志: 0=运行中, 1=请求退出 (由 main 管理) */
    g_pCtrl->config_monitor_exit.store(false);
    g_pCtrl->fd_monitor_exit.store(false);
    g_pCtrl->disp_thread_exit.store(false);

    return 0;
}

/*======================== 反初始化 ========================*/
void app_ctrl_request_stop(void)
{
    if (!g_pCtrl)
        return;

    g_pCtrl->isRunning.store(false);
    g_pCtrl->config_monitor_exit.store(true);
    g_pCtrl->fd_monitor_exit.store(true);
    g_pCtrl->disp_thread_exit.store(true);
    pthread_mutex_lock(&g_pCtrl->cv_config_mtx);
    pthread_cond_broadcast(&g_pCtrl->cv_config);
    pthread_mutex_unlock(&g_pCtrl->cv_config_mtx);
}

void app_ctrl_deinit(void)
{
    if (!g_pCtrl)
        return;

    /* deinit 不负责 join。main 必须先 request_stop 并回收所有使用控制块的线程。 */
    app_ctrl_request_stop();

    /* 销毁同步原语 */
    pthread_rwlock_destroy(&g_pCtrl->mtx);
    pthread_mutex_destroy(&g_pCtrl->cv_config_mtx);
    pthread_cond_destroy(&g_pCtrl->cv_config);
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_mutex_destroy(&g_pCtrl->chn_mtx[i]);

    delete g_pCtrl;
    g_pCtrl = nullptr;
}

int app_ctrl_has_channel(int channel_id)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return app_ctrl_runtime_channel_config(snapshot, channel_id) ? 1 : 0;
}

int app_ctrl_get_channel_display_order(int channel_id)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    if (!snapshot || channel_id < 0 || channel_id >= MAX_CHANNEL_NUM)
        return -1;
    return snapshot->channel_config_index[channel_id];
}

int app_ctrl_get_chn_nums(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? static_cast<int>(snapshot->config.channels.size()) : 0;
}

int app_ctrl_get_enable_disp(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? (snapshot->config.enable_display ? 1 : 0) : 0;
}

int app_ctrl_get_enable_rtsp(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? (snapshot->config.enable_rtsp ? 1 : 0) : 0;
}

int app_ctrl_get_disp_width(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? (snapshot->config.disp_width & ~3) : 0;
}

int app_ctrl_get_disp_height(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? (snapshot->config.disp_height & ~1) : 0;
}

int app_ctrl_get_tile_cols(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot && snapshot->config.tile_cols > 0 ? snapshot->config.tile_cols : 1;
}

int app_ctrl_get_tile_rows(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    if (!snapshot)
        return 1;
    const int cols = snapshot->config.tile_cols > 0 ? snapshot->config.tile_cols : 1;
    const int count = static_cast<int>(snapshot->config.channels.size());
    const int required = std::max(1, (count + cols - 1) / cols);
    return snapshot->config.tile_rows > required ? snapshot->config.tile_rows : required;
}

int app_ctrl_get_max_fps(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot && snapshot->config.max_fps > 0 ? snapshot->config.max_fps : 30;
}

int app_ctrl_get_local_default_fps(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? snapshot->config.local_default_fps : 25;
}

int app_ctrl_get_performance_display(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? (snapshot->config.performance_display ? 1 : 0) : 0;
}

int app_ctrl_get_debug_display(void)
{
    auto snapshot = app_ctrl_get_runtime_snapshot();
    return snapshot ? (snapshot->config.debug_display ? 1 : 0) : 0;
}

/*======================== 通道数据查询 ========================*/
std::vector<AlgoResult> app_ctrl_get_results(int chnId)
{
    if (!app_ctrl_has_channel(chnId))
        return {};
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    std::vector<AlgoResult> out = g_pCtrl->channels_state[chnId].last_results;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    return out;
}

std::vector<AlgoResult> app_ctrl_get_results_fresh(int chnId, int max_age_ms)
{
    if (!app_ctrl_has_channel(chnId))
        return {};
    if (max_age_ms <= 0)
        return app_ctrl_get_results(chnId);

    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    auto &cs = g_pCtrl->channels_state[chnId];
    uint64_t now = steady_now_ms();
    if (cs.published_steady_ms == 0)
    {
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        return {};
    }
    int64_t age_ms = now >= cs.published_steady_ms ? static_cast<int64_t>(now - cs.published_steady_ms) : 0;
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
    if (!app_ctrl_has_channel(chnId))
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

std::string app_ctrl_get_logic_name(int chnId)
{
    auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, chnId);
    return channel ? channel->logic : std::string();
}

static void fill_channel_logic_snapshot_locked(int chnId, const ChannelState &state, ChannelLogicSnapshot *out,
                                               std::shared_ptr<const AppRuntimeSnapshot> *published_runtime)
{
    out->has_publication = state.publication_seq != 0;
    out->has_frame = !state.last_logic_frame.empty();
    out->channel_id = chnId;
    out->publication_seq = state.publication_seq;
    out->published_steady_ms = state.published_steady_ms;
    out->frame_steady_ms = state.frame_steady_ms;
    out->frame_unix_ms = state.frame_unix_ms;
    out->config_generation = state.published_config_generation;
    if (state.published_steady_ms != 0)
    {
        const uint64_t now = steady_now_ms();
        out->publication_age_ms = now >= state.published_steady_ms
                                      ? static_cast<int64_t>(now - state.published_steady_ms)
                                      : 0;
    }
    out->logic_frame_id = state.published_logic_frame_id;
    out->frame_seq = state.published_frame_seq;
    out->src_width = state.published_src_width;
    out->src_height = state.published_src_height;
    out->infer_enabled = state.published_infer_enabled != 0;
    out->disp_fps = state.disp_fps;
    out->online_state = state.online_state;
    out->online_state_changed_steady_ms =
        state.online_state == CH_ONLINE ? state.online_ts_ms : state.offline_ts_ms;
    out->outputs = state.logic_outputs;
    if (published_runtime)
        *published_runtime = state.published_runtime;
}

static void fill_channel_publication_config(int chnId, const std::shared_ptr<const AppRuntimeSnapshot> &runtime,
                                            ChannelLogicSnapshot *out)
{
    if (!out)
        return;
    const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, chnId);
    if (channel)
    {
        out->display_order = runtime->channel_config_index[chnId];
        out->logic_name = channel->logic;
    }
}

int app_ctrl_get_channel_logic_snapshot(int chnId, ChannelLogicSnapshot *out)
{
    if (!out || !app_ctrl_has_channel(chnId))
        return 0;

    *out = ChannelLogicSnapshot();
    const float infer_fps = algorithm_get_infer_fps(chnId);
    std::shared_ptr<const AppRuntimeSnapshot> published_runtime;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    fill_channel_logic_snapshot_locked(chnId, g_pCtrl->channels_state[chnId], out, &published_runtime);
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    fill_channel_publication_config(chnId, published_runtime, out);
    out->infer_fps = infer_fps;
    return 1;
}

int app_ctrl_get_channel_frame_snapshot(int chnId, ChannelFrameSnapshot *out)
{
    if (!out || !app_ctrl_has_channel(chnId))
        return 0;

    *out = ChannelFrameSnapshot();
    const float infer_fps = algorithm_get_infer_fps(chnId);

    cv::Mat frame_shallow;
    std::shared_ptr<const AppRuntimeSnapshot> published_runtime;
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        const auto &cs = g_pCtrl->channels_state[chnId];
        frame_shallow = cs.last_logic_frame;
        out->results = cs.last_results;
        out->draw_cmds = cs.draw_cmds;
        fill_channel_logic_snapshot_locked(chnId, cs, &out->logic, &published_runtime);
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }
    out->frame = frame_shallow.clone();
    fill_channel_publication_config(chnId, published_runtime, &out->logic);
    const std::vector<RoiZone> *rois = app_ctrl_runtime_channel_rois(published_runtime, chnId);
    if (rois)
        out->rois = *rois;
    out->logic.infer_fps = infer_fps;
    return 1;
}
