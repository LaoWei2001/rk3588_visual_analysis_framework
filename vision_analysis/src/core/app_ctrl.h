/**
 * @file app_ctrl.h
 * @brief 应用全局控制块 — 仿海康 H9 HIKFLOW_DEMO_CTRL 风格设计
 *
 * 设计参照: hikflow_demo_priv.h 中 _HIKFLOW_DEMO_CTRL_ 的分区段组织方式
 *
 * 线程一览 (全部在 main 中 pthread_create / pthread_join):
 *   1. config_monitor_thread   — 配置文件热加载监控
 *   2. fd_monitor_thread       — fd 使用量监控
 *   3. capture_bus_thread[N]   — GStreamer bus 监听 + 重连 (DecChannel 内部)
 *   4. display_worker[N]       — 异步显示 RGA + framebuffer
 *   5. dispatch_worker[N]      — NPU 结果分发 + channel_logic
 *   6. infer_worker[N]         — NPU 推理 (algoProcess 内部)
 *   7. global_logic[N]         — 跨通道全局逻辑轮询
 *   8. alarm/event workers     — 告警落盘与事件录像（各模块内部管理）
 */

#pragma once

#include "../config/config.h"
#include "../player/display.h"
#include "logic/core/channel_logic.h"
#include "logic/core/logic_parameters.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <string>
#include <vector>

class DecChannel; /* 前置声明, 底层 C++ 类型 */

/*======================== 魔数 ========================*/
#define APP_CTRL_MAGIC 0x524B3358U /* "RK3X" */

/*================================================================
 * 通道在线状态枚举
 *================================================================*/
/**
 * 状态转移:
 *   捕获线程检测到断流 → CH_OFFLINE
 *   捕获线程发起重连   → CH_RECONNECTING
 *   重连成功（首帧到达）→ CH_ONLINE
 *
 * 写操作由 analyzer_channel_offline / analyzer_channel_online 持 chn_mtx 完成；
 * 读操作由 analyzer_is_channel_online / get_channel_snapshot 持同一把锁完成。
 */
enum ChannelOnlineState
{
    CH_ONLINE = 0,       /*!< 流正常到达 */
    CH_OFFLINE = 1,      /*!< 流已断开    */
    CH_RECONNECTING = 2, /*!< 重连进行中  */
};

/*================================================================
 * 通道原始帧 — 仅供 analyzer 内部传递
 *================================================================*/
struct ChannelRawFrame
{
    int width = 0;
    int height = 0;
    cv::Mat model_input_mat;

    /* 解码源帧的借用视图：仅在 videoOutHandle 当前回调及其同步调用链内有效。
     * 传统 CV logic 可通过 ChannelContext::source_frame() 按需转成原分辨率 BGR；
     * 不得把 source_data 保存到本次 logic 调用之外。异步推理结果路径不设置这些字段。 */
    const void *source_data = nullptr;
    int source_format = 0;
    int source_hstride = 0;
    int source_vstride = 0;
};

/*================================================================
 * 不可变运行配置快照
 *================================================================*/
/**
 * 配置热更新只在框架层构造并整体发布此快照。逐帧处理拿到 shared_ptr 后，
 * 本帧看到的 ChannelConfig / ROI / 全局参数始终来自同一 generation，
 * 不会在业务 logic 执行过程中被原地修改。
 *
 * ChannelContext 的公开字段保持不变：框架仍向上层提供
 * ctx->config / ctx->rois / ctx->roi，上层 logic 无需感知此类型。
 */
struct AppRuntimeSnapshot
{
    AppConfig config;
    std::vector<RoiZone> roi_zones[MAX_CHANNEL_NUM];
    LogicParameterSet logic_parameters[MAX_CHANNEL_NUM];
    int channel_config_index[MAX_CHANNEL_NUM]{};
    uint64_t generation = 0;
};

/*================================================================
 * 通道运行时状态
 *================================================================*/
struct ChannelState
{
    /* fps_counter/last_fps_ts_ms 由 display_worker 独占；disp_fps 跨线程读写走 chn_mtx */
    cv::Mat tile_staging;
    float disp_fps = 0.0f;
    int fps_counter = 0;
    uint64_t last_fps_ts_ms = 0;

    /* 推理时间戳由 videoOutHandle 更新，跨线程读写走 chn_mtx */
    uint64_t last_infer_ts_ms = 0;

    /* 由 chn_mtx[chnId] 保护 */
    ChannelOnlineState online_state = CH_ONLINE; /*!< 当前在线状态 */
    uint64_t offline_ts_ms = 0;                  /*!< 最近一次离线时刻 */
    uint64_t online_ts_ms = 0;                   /*!< 最近一次上线时刻 */
    std::vector<AlgoResult> last_results;
    int64_t result_frame_seq = 0; /* last_results/last_logic_frame 对应的帧序号, 用于校验帧-结果匹配 */
    uint64_t last_result_ts_ms = 0;
    int src_w_now = 0; /* 当前解码源分辨率(frame_inlet 写，ChannelContext 元信息读取) */
    int src_h_now = 0;
    std::vector<DrawCommand> draw_cmds;
    std::shared_ptr<void> logic_state;
    cv::Mat last_frame;
    cv::Mat last_logic_frame;
    cv::Mat
        logic_display_frame; /* logic 经 display_canvas() 自绘的显示底图(640×640 BGR)；空=不覆盖，显示走实时采集帧 */
    uint64_t logic_display_ts_ms = 0; /* 上面那帧的产生时刻(steady ms)，显示端据此判新鲜度，过期回退实时帧 */
    int64_t logic_frame_id = 0;
    int64_t input_frame_seq = 0;
    uint64_t last_logic_ts_ms = 0;

    /* 推理运行时开关: 1=按 config 正常推理; 0=本通道强制跳过NPU推理(画面正常显示)。
     * 由系统级动作 infer_toggle 控制, channel_control 线程写, frame_inlet 线程读。 */
    int infer_runtime_enable = 1;
};

/*================================================================
 * 通道快照 — 一次持锁原子读出 frame + results + logic_state
 *================================================================*/
struct ChannelSnapshot
{
    cv::Mat frame;
    std::vector<AlgoResult> results;
    float infer_fps = 0.0f;
    float disp_fps = 0.0f;
    int64_t logic_frame_id = 0;
    int64_t frame_seq = 0; /* frame 与 results 共同对应的帧序号 (二者保证来自同一帧) */
    int64_t result_age_ms = -1;
    bool has_results = false;
    std::shared_ptr<void> logic_state;
    ChannelOnlineState online_state = CH_ONLINE; /*!< 快照时刻的在线状态 */
};

/*================================================================
 * 全局控制块 — 仿 H9 HIKFLOW_DEMO_CTRL 分区段设计
 *================================================================*/
#define APP_CTRL_MAX_CAPTURERS MAX_CHANNEL_NUM

struct APP_CTRL
{
    /*!< 0. 有效性校验 */
    unsigned int magic; /*!< 魔数 APP_CTRL_MAGIC, 运行时校验 */

    /*!< 1. 全局参数 */
    int b_init;       /*!< 初始化完成标志 */
    AppConfig config; /*!< 全局配置 (JSON 解析结果) */
    /* 逐帧线程只读此不可变快照；使用 atomic_load/store(shared_ptr) 发布。 */
    std::shared_ptr<const AppRuntimeSnapshot> runtime_snapshot;
    uint64_t config_generation = 1;

    /*!< 2. 显示子系统 */
    Display_t dispDesc;                        /*!< 显示窗口描述符 */
    char **pDispBuffer;                        /*!< 双缓冲指针 (front buffer) */
    std::atomic<bool> disp_thread_exit{false}; /*!< 显示线程退出标志 */

    /*!< 3. 采集子系统 */
    DecChannel *capturers[APP_CTRL_MAX_CAPTURERS]; /*!< 采集器句柄 */
    int capturer_count;                            /*!< 有效采集器数量 */

    /*!< 4. 推理子系统 */
    int inputW; /*!< 模型输入宽度 */
    int inputH; /*!< 模型输入高度 */

    /*!< 5. 通道运行时状态 (索引=通道号) */
    ChannelState channels_state[MAX_CHANNEL_NUM]; /*!< 每通道独立状态 */

    /*!< 6. pthread 同步原语 */
    pthread_rwlock_t mtx;                     /*!< 配置读写锁 (保护 config) */
    pthread_mutex_t cv_config_mtx;            /*!< 配合 cv_config 的互斥锁 */
    pthread_cond_t cv_config;                 /*!< 配置监控线程条件变量 */
    pthread_mutex_t chn_mtx[MAX_CHANNEL_NUM]; /*!< 通道独立锁 */
    std::atomic<bool> isRunning{false};       /*!< 全局运行标志: false=退出 */

    /*!< 7. 线程句柄 (main 中 pthread_create 填充) */
    pthread_t config_monitor_tid; /*!< 配置热加载监控线程 */
    pthread_t fd_monitor_tid;     /*!< fd 使用量监控线程 */

    /*!< 8. 配置热加载与线程退出标志 */
    uint64_t configLastMtime;                     /*!< 配置文件上次修改时间 */
    std::atomic<bool> config_monitor_exit{false}; /*!< 配置监控线程退出标志 */
    std::atomic<bool> fd_monitor_exit{false};     /*!< fd 监控线程退出标志 */
};

/*======================== 全局指针 ========================*/
extern APP_CTRL *g_pCtrl;

/*======================== 生命周期接口 ========================*/
#ifdef __cplusplus
extern "C"
{
#endif

    int app_ctrl_init(const char *cfgPath);
    /** 仅发出停止请求并唤醒等待者；所有线程仍须由创建者 join。 */
    void app_ctrl_request_stop(void);
    void app_ctrl_deinit(void);

    /* 单一通道身份查询。channel_id 始终等于 config.channels[].id。 */
    int app_ctrl_has_channel(int channel_id);
    /**
     * 兼容查询：返回值由线程局部 shared_ptr 保活，内容只读且不会被热更新原地修改。
     * 新的框架代码优先一次获取 app_ctrl_get_runtime_snapshot()，避免同帧重复查询。
     */
    const ChannelConfig *app_ctrl_get_channel_config(int channel_id);
    int app_ctrl_get_channel_display_order(int channel_id);

    std::shared_ptr<const AppRuntimeSnapshot> app_ctrl_get_runtime_snapshot(void);
    std::shared_ptr<const AppRuntimeSnapshot> app_ctrl_build_runtime_snapshot(const AppConfig &config, int input_w,
                                                                              int input_h, uint64_t generation);
    void app_ctrl_store_runtime_snapshot(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot);
    const ChannelConfig *app_ctrl_runtime_channel_config(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot,
                                                         int channel_id);
    const std::vector<RoiZone> *app_ctrl_runtime_channel_rois(const std::shared_ptr<const AppRuntimeSnapshot> &snapshot,
                                                              int channel_id);
    const LogicParameterSet *app_ctrl_runtime_logic_parameters(
        const std::shared_ptr<const AppRuntimeSnapshot> &snapshot, int channel_id);

    /*======================== 通道数据查询 (线程安全) ========================*/
    std::vector<AlgoResult> app_ctrl_get_results(int chnId);
    std::vector<AlgoResult> app_ctrl_get_results_fresh(int chnId, int max_age_ms);
    float app_ctrl_get_disp_fps(int chnId);
    float app_ctrl_get_infer_fps(int chnId);

    int app_ctrl_get_target_count(int chnId, const char *label, int max_age_ms);
    int app_ctrl_has_target(int chnId, const char *label, int max_age_ms);
    uint64_t app_ctrl_get_last_infer_ts_ms(int chnId);
    std::string app_ctrl_get_logic_name(int chnId);

    int app_ctrl_get_channel_snapshot(int chnId, ChannelSnapshot *out);

    /*======================== 不可变运行快照属性 ========================*/
    int app_ctrl_get_chn_nums(void);
    int app_ctrl_get_enable_disp(void);
    int app_ctrl_get_enable_rtsp(void);
    int app_ctrl_get_disp_width(void);
    int app_ctrl_get_disp_height(void);
    int app_ctrl_get_tile_cols(void);
    int app_ctrl_get_tile_rows(void);
    int app_ctrl_get_max_fps(void);
    int app_ctrl_get_local_default_fps(void);
    int app_ctrl_get_performance_display(void);
    int app_ctrl_get_debug_display(void);

#ifdef __cplusplus
}
#endif
