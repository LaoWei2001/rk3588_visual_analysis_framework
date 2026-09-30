/**
 * @file pipeline_runtime.h
 * @brief 视频分析管线的生命周期、帧入口和结果分发接口
 *
 * 线程模型:
 *   pipeline_dispatch_worker — 每推理通道一个, NPU 结果分发 + channel_logic
 *   pipeline_logic_worker    — 每启用通道一个, 异步执行非 NPU 视觉逻辑
 *   显示线程接口由 display/display_pipeline.h 提供。
 */
#pragma once

#include "config/config.h"
#include <cstdint>
#include <vector>

#ifdef __cplusplus
extern "C"
{
#endif

    /*======================== 图像描述 (C-compatible) ========================*/
    typedef struct
    {
        char fmt[32];
        int chnId;
        int width;
        int height;
        int horStride;
        int verStride;
        int dataSize;
        int fd;
    } FrameInputDesc;

    /** @brief 结果分发线程 — 等 NPU 完成 → process_channel_results → channel_logic */
    void *pipeline_dispatch_worker(void *arg);

    /** @brief 传统视觉线程 — 只消费最新帧，不阻塞解码回调 */
    void *pipeline_logic_worker(void *arg);

    /*======================== 生命周期接口 ========================*/

    /** @brief 初始化分析管线数据结构 (不创建线程).
     *  线程创建由 main 负责. */
    int pipeline_init(void);

    /** 只请求停止并唤醒推理/分发/显示等待者，不销毁任何同步对象。 */
    void pipeline_request_stop(void);

    /** @brief 反初始化分析管线；必须在 main 已 join display/dispatch 后调用。 */
    void pipeline_deinit(void);

    /** @brief 唤醒所有 display 线程 (让它们检查 isRunning 退出). */
    void pipeline_wake_display_threads(void);

    /** @brief 销毁显示/传统逻辑队列同步原语 (必须在相关线程 join 后调用). */
    void pipeline_destroy_display_queues(void);

    /** @brief 视频帧处理入口，由采集模块的 appsink 回调调用。 */
    int pipeline_submit_frame(char *imgData, FrameInputDesc imgDesc);

    /** @brief 获取 display/dispatch/logic 线程数量 (供 main 创建线程) */
    int pipeline_get_display_thread_count(void);
    int pipeline_get_dispatch_thread_count(void);
    int pipeline_get_logic_thread_count(void);

    /** @brief 获取指定索引的 display/dispatch/logic 线程的通道号 */
    int pipeline_get_display_chn_id(int idx);
    int pipeline_get_dispatch_chn_id(int idx);
    int pipeline_get_logic_chn_id(int idx);

    /*======================== 跟踪器接口 ========================*/
    void pipeline_reset_tracker_ids(int chnId);

    /*======================== 通道热插拔 / 断流重连 ========================*/

    /**
     * @brief 标记通道离线（捕获线程检测到断流时调用）。
     *
     * 效果：
     *   - 更新 online_state → CH_OFFLINE，记录 offline_ts_ms
     *   - 重置 FPS 节流计时（feed_stats_reset），避免重连后节流偏移错乱
     *   - 重置跟踪器轨迹（pipeline_reset_tracker_ids），避免旧 track_id 污染
     *
     * 典型调用点：GStreamer bus EOS / error 回调（capture_bus_thread）。
     */
    void pipeline_channel_offline(int chnId);

    /**
     * @brief 标记通道恢复在线（捕获线程重连成功后调用）。
     *
     * 效果：
     *   - 更新 online_state → CH_ONLINE，记录 online_ts_ms
     *   - 清空 last_results / draw_cmds，避免旧框冻结在新画面上
     *   - 重置 FPS 节流计时与跟踪器（同 pipeline_channel_offline）
     *
     * 典型调用点：重连后 GStreamer pipeline 首帧到达前。
     */
    void pipeline_channel_online(int chnId);

#ifdef __cplusplus
}

/**
 * @brief 原子发布一代不可变运行配置。
 *
 * 框架会先构造完整快照，再与各通道逐帧处理互斥地一次发布；业务 logic 的
 * ChannelContext 接口保持不变。logic_changed_channels 会额外清空对应 logic_state，
 * tracker_reset_channels 会重置轨迹编号。
 */
bool pipeline_publish_runtime_snapshot(const AppConfig &config, uint64_t generation,
                                       const std::vector<int> &logic_changed_channels = {},
                                       const std::vector<int> &tracker_reset_channels = {});

#endif
