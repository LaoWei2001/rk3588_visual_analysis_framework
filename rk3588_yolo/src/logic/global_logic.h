/**
 * @file global_logic.h
 * @brief 全局逻辑模块 — 跨通道策略 + 轮询调度
 *
 * ============================================================
 * 通道 logic vs 全局 logic — 选哪个？
 * ============================================================
 *
 * │ 维度             │ 通道 logic (channel_logic)          │ 全局 logic
 * (global_logic)              │
 * │------------------│-------------------------------------│----------------------------------------│
 * │ 触发时机         │ 每帧推理完成后立即调用              │ 独立线程，按
 * poll_interval_ms 轮询      │ │ 访问的帧数据     │ ctx->frame /
 * ctx->results（当帧）   │ get_channel_snapshot（快照，可能旧几帧）│ │
 * 跨通道协同       │ 需主动 get_channel_snapshot(other)  │
 * 天然访问所有通道；for_each_channel 遍历 │ │ 状态管理         │
 * ctx->state（本通道专属）             │ gctx->state（本实例专属） │ │ 适用场景
 * │ 本通道的目标过滤/计时/告警          │ 多路联动、全局计数、汇总上报 │
 *
 * 经验法则：
 *   - 只看自己通道的结果 → 用通道 logic
 *   - 需要同时看多路结果或做跨路关联 → 用全局 logic
 *   - 需要每帧都跑的高频逻辑 → 用通道 logic（poll_interval_ms
 * 不能低于推理延迟）
 *   - 需要后台定期巡检（不关心每帧）→ 用全局 logic
 *
 * ============================================================
 * 注册全局 logic 的方法
 * ============================================================
 *
 * 1. 在 global_logic.cpp 中实现函数（见下方"最小 logic 示例"）：
 *       static void global_my_logic(GlobalContext *gctx) { ... }
 *
 * 2. 在 global_logic.cpp 的 global_logic_register() 中注册：
 *       register_global_logic("global_my_logic", global_my_logic);
 *
 * 3. 在 config.json 中配置（支持多实例）：
 *       "global_logics": [
 *           {
 *               "enable": true,
 *               "logic":  "global_my_logic",
 *               "channels": [0, 1, 2],      // 空数组 = 监控所有通道
 *               "poll_interval_ms": 200
 *           }
 *       ]
 *
 * ============================================================
 * 最小 logic 示例
 * ============================================================
 *
 * @code
 * // 持久化状态（跨 tick 保留，用 shared_ptr<void> 惰性初始化）
 * struct MyGlobalState {
 *     int total_person_count = 0;
 *     uint64_t last_alarm_ts = 0;
 * };
 *
 * static void global_my_logic(GlobalContext *gctx)
 * {
 *     // ① 惰性初始化跨 tick 状态
 *     if (!*gctx->state)
 *         *gctx->state = std::make_shared<MyGlobalState>();
 *     auto &st = *std::static_pointer_cast<MyGlobalState>(*gctx->state);
 *
 *     // ② 可选：仅在有新推理结果时处理（避免重复计算旧帧）
 *     if (!gctx->has_new_infer) return;
 *
 *     // ③ 遍历本实例监控的通道
 *     int total = 0;
 *     gctx->for_each_channel([&](int ch, int idx) {
 *         ChannelSnapshot s = gctx->get_channel_snapshot(ch);
 *
 *         // 新鲜度检查（避免使用太旧的结果）
 *         if (s.frame.empty() || s.result_age_ms > 500) return;
 *
 *         // s.frame 与 s.results 严格同帧（s.frame_seq 即帧序号）
 *         for (const auto &r : s.results) {
 *             if (r.label == "person") ++total;
 *         }
 *
 *         // 若需要该通道的专有变量（由通道 logic 写入 logic_state）：
 *         // auto cst = std::static_pointer_cast<ChState>(s.logic_state);
 *         // if (cst) { ... cst->my_field ... }
 *     });
 *
 *     st.total_person_count = total;
 *
 *     // ④ 触发告警（限频：每 5 秒一次）
 *     if (total >= 3) {
 *         const uint64_t now = gctx->timestamp_ms;
 *         if (now - st.last_alarm_ts > 5000) {
 *             st.last_alarm_ts = now;
 *             printf("[MyGlobal] ALARM: %d persons across all channels\n",
 * total);
 *             // alarm_uploader_enqueue(...)  ← 按需接入上报
 *         }
 *     }
 * }
 * @endcode
 *
 * ============================================================
 * 状态管理说明
 * ============================================================
 *
 * gctx->state 是 std::shared_ptr<void>*，指向本实例的跨 tick 持久状态。
 * 惰性初始化范式（推荐）：
 *
 *   if (!*gctx->state)
 *       *gctx->state = std::make_shared<MyState>();
 *   auto &st = *std::static_pointer_cast<MyState>(*gctx->state);
 *
 * 注意事项：
 *   - gctx->state 的所有权由框架持有；logic 函数只写 *gctx->state（不 reset
 * 指针本身）。
 *   - 框架在 global_logic_stop_all() 时自动释放状态，无需 logic 手动清理。
 *   - 多个全局 logic 实例各自独立 state，互不影响。
 *
 * ============================================================
 * has_new_infer / latest_infer_channel 使用说明
 * ============================================================
 *
 * has_new_infer = 1  表示在上一 tick 到本 tick 之间，至少有一个受监控通道
 * 产生了新的推理结果（result_frame_seq 更新了）。
 *
 * 用途：
 *   if (!gctx->has_new_infer) return;  // 跳过没有新结果的 tick，减少无效计算
 *
 * latest_infer_channel 是本 tick 内最后完成推理的通道号（-1=无）。
 * 适合"谁最新就用谁"的场景：
 *   int ch = gctx->latest_infer_channel;
 *   if (ch >= 0) {
 *       ChannelSnapshot s = gctx->get_channel_snapshot(ch);
 *       ...
 *   }
 */

#pragma once

#include <cstdint>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

struct AlgoResult;
#include "../config/config.h"
#include "../core/app_ctrl.h"

/*======================== 全局逻辑上下文 ========================*/

/**
 * @brief 全局 logic 函数的上下文（每次 tick 构造、栈上传递）。
 *
 * 取数安全规则（一图胜千言）：
 *
 *   ┌─────────────────────────────────────────────────────┐
 *   │  gctx->get_channel_snapshot(ch)                     │
 *   │    ↓ 一把 chn_mtx，原子读出                         │
 *   │  snapshot.frame      — BGR 640 图（模型输入尺寸）    │
 *   │  snapshot.results    — 与 frame 严格同帧的检测结果   │
 *   │  snapshot.frame_seq  — 帧序号（frame == results 的证明）│
 *   │  snapshot.logic_state— 该通道 logic 的专有状态       │
 *   │  snapshot.result_age_ms — 距现在的毫秒数（新鲜度）   │
 *   └─────────────────────────────────────────────────────┘
 *
 * 重要：不要拆成多次调用 — get_channel_snapshot 一次性原子取出，
 * 分次调用（分别取 frame 和 results）无法保证同帧。
 */
struct GlobalContext
{
    /**
     * @brief 本实例配置（只读）。
     *
     * 包含 logic 名称、channels 列表、poll_interval_ms 等。
     * 热重载时框架会重启受影响实例，此指针始终有效。
     */
    const GlobalLogicConfig *config;

    /**
     * @brief 本次 tick 的时间戳（ms，单调递增）。
     *
     * 适合做限频判断：
     *   if (gctx->timestamp_ms - last_alarm_ts > 5000) { 触发告警; }
     */
    uint64_t timestamp_ms;

    /**
     * @brief 本实例的累计 tick 计数（从 0 开始，每 poll_interval_ms 递增 1）。
     *
     * 适合做周期性任务（如每 10 tick 打印一次统计）：
     *   if (gctx->tick_id % 10 == 0) { 打印; }
     */
    int64_t tick_id;

    /**
     * @brief 本实例监控的通道 ID 列表指针（只读）。
     *
     * 请通过 for_each_channel() 遍历，不要直接解引用（框架保证非空）。
     * 若配置中 channels 为空数组，框架会自动填充所有活跃通道。
     */
    const std::vector<int> *channel_ids;

    /**
     * @brief 本 tick 内是否有受监控通道产生新推理结果。
     *
     * 为 0 表示所有受监控通道的 result_frame_seq 与上 tick 相同，
     * 通常意味着推理线程还没跑完或输入帧率很低。
     *
     * 常见用法：
     *   if (!gctx->has_new_infer) return;  // 无新结果就跳过
     */
    int has_new_infer;

    /**
     * @brief 本 tick 内最近完成推理的通道号；无则为 -1。
     *
     * 仅当 has_new_infer == 1 时有意义。
     * 适合"抢先处理最新通道"的场景：
     *   int ch = gctx->latest_infer_channel;
     *   if (ch >= 0) { ... }
     */
    int latest_infer_channel;

    /** @brief latest_infer_channel 通道完成推理的时刻（ms）。*/
    uint64_t latest_infer_ts_ms;

    /**
     * @brief 本实例的跨 tick 持久状态（框架持有所有权）。
     *
     * 惰性初始化范式（见文件头示例）。
     * 框架在 global_logic_stop_all() 时自动释放，无需 logic 手动清理。
     */
    std::shared_ptr<void> *state;

    /* ---- 便捷方法 ---- */

    /**
     * @brief 原子读取指定通道的完整快照（帧 + 结果 + 状态）。
     *
     * 快照内 frame 与 results 来自同一帧（frame_seq 证明），
     * 持有快照期间无需持锁，可随便访问。
     *
     * 新鲜度检查（强烈建议）：
     *   ChannelSnapshot s = gctx->get_channel_snapshot(ch);
     *   if (s.frame.empty() || s.result_age_ms > 500) return;
     *
     * @param chnId  任意通道号（不限于本实例的 channel_ids）
     */
    ChannelSnapshot get_channel_snapshot(int chnId) const
    {
        ChannelSnapshot out;
        app_ctrl_get_channel_snapshot(chnId, &out);
        return out;
    }

    /** @brief 获取指定通道当前使用的 logic 名称。*/
    std::string get_channel_logic_name(int chnId) const
    {
        return app_ctrl_get_logic_name(chnId);
    }

    /** @brief 检查指定通道是否使用指定 logic（热重载后名称可能已变）。*/
    int channel_has_logic(int chnId, const char *logicName) const
    {
        return app_ctrl_get_logic_name(chnId) == std::string(logicName) ? 1 : 0;
    }

    /** @brief 获取指定通道的推理帧率（EMA 平滑值）。*/
    float get_channel_infer_fps(int chnId) const
    {
        return app_ctrl_get_infer_fps(chnId);
    }

    /** @brief 获取指定通道的显示帧率（EMA 平滑值）。*/
    float get_channel_disp_fps(int chnId) const
    {
        return app_ctrl_get_disp_fps(chnId);
    }

    /**
     * @brief 快速查询指定通道在 max_age_ms 内是否有指定标签的目标。
     *
     * 内部不持 chn_mtx（读时间戳+结果，存在极低概率读旧值），
     * 适合快速过滤；精确逻辑请用 get_channel_snapshot。
     */
    int channel_has_target(int chnId, const char *label, int max_age_ms = 200) const
    {
        return app_ctrl_has_target(chnId, label, max_age_ms);
    }

    /**
     * @brief 快速查询指定通道在 max_age_ms 内指定标签的目标数量。
     *
     * 同 channel_has_target，适合快速过滤。
     */
    int get_channel_target_count(int chnId, const char *label, int max_age_ms = 200) const
    {
        return app_ctrl_get_target_count(chnId, label, max_age_ms);
    }

    /**
     * @brief 遍历本实例监控的所有通道。
     *
     * @param fn  回调，签名为 void(int chnId, int idx)
     *              chnId — 通道号
     *              idx   — 在 channel_ids 中的下标（0 起始）
     *
     * 使用示例：
     * @code
     *   gctx->for_each_channel([&](int ch, int idx) {
     *       ChannelSnapshot s = gctx->get_channel_snapshot(ch);
     *       if (s.frame.empty() || s.result_age_ms > 500) return;
     *       // 处理 s.results（与 s.frame 严格同帧）
     *   });
     * @endcode
     *
     * 跨通道聚合示例（统计所有通道中 person 的总数）：
     * @code
     *   int total = 0;
     *   gctx->for_each_channel([&](int ch, int) {
     *       total += gctx->get_channel_target_count(ch, "person", 300);
     *   });
     * @endcode
     */
    template <typename Func> void for_each_channel(Func &&fn) const
    {
        if (!channel_ids)
            return;
        for (size_t i = 0; i < channel_ids->size(); ++i)
            fn((*channel_ids)[i], (int)i);
    }
};

/*======================== 全局逻辑函数类型 ========================*/

/**
 * @brief 全局逻辑函数指针类型。
 *
 * 函数在独立线程中以 poll_interval_ms 为周期轮询调用。
 * 执行时间应远小于 poll_interval_ms（避免拖慢下一 tick）。
 */
typedef void (*GlobalLogicFunc)(GlobalContext *gctx);

/*======================== 多实例管理器接口 ========================*/

/**
 * @brief 启动所有全局逻辑实例（注册 + 内部创建 pthread）。
 *
 * 由 analyzer_init 调用一次；热重载时由 config_monitor 按需重启受影响实例。
 * @return 成功启动的实例数（启动失败的实例跳过，不影响其余实例）
 */
int global_logic_start_all(const std::vector<GlobalLogicConfig> &cfgs);

/**
 * @brief 停止所有全局逻辑实例（发送退出信号并 join 线程）。
 *
 * 由 analyzer_deinit 调用；调用后 state 自动释放。
 */
void global_logic_stop_all(void);

/**
 * @brief 获取当前运行的全局逻辑实例数量（供 main 诊断打印）。*/
int global_logic_get_instance_count(void);

/** @brief 全局逻辑线程入口（由 global_logic_start_all
 * 内部调用，不需要外部直接使用）。*/
void *global_logic_thread_func(void *arg);
