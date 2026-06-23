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
 *   8. upload_worker           — Redis 报警上传
 */

#pragma once

#include "../config/config.h"
#include "../logic/channel_logic.h"
#include "../player/display.h"
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
};

/*================================================================
 * 通道运行时状态
 *================================================================*/
struct ChannelState
{
    /* 显示统计 (display_worker 独占, 无锁) */
    cv::Mat tile_staging;
    float disp_fps = 0.0f;
    int fps_counter = 0;
    uint64_t last_fps_ts_ms = 0;

    /* 推理时间戳 (videoOutHandle 独占) */
    uint64_t last_infer_ts_ms = 0;

    /* 由 chn_mtx[chnId] 保护 */
    ChannelOnlineState online_state = CH_ONLINE; /*!< 当前在线状态 */
    uint64_t offline_ts_ms = 0;                  /*!< 最近一次离线时刻 */
    uint64_t online_ts_ms = 0;                   /*!< 最近一次上线时刻 */
    std::vector<AlgoResult> last_results;
    int64_t result_frame_seq = 0; /* last_results/last_logic_frame 对应的帧序号, 用于校验帧-结果匹配 */
    uint64_t last_result_ts_ms = 0;
    /* 多 ROI 区域 (一个通道可配置多个)。
     * roi_zones      : 各区域 名字+多边形(模型输入坐标系) —— logic(ctx->rois) 与
     * 显示 都用它。 roi_zones_raw  : 各区域原始像素顶点,
     * 仅"旧像素格式"需要(源分辨率变化时据此重算 roi_zones);
     *                  归一化加载时直接写入 roi_zones、此表留空。索引与 roi_zones
     * 一一对应。 */
    std::vector<RoiZone> roi_zones;
    std::vector<std::vector<cv::Point>> roi_zones_raw;
    bool roi_model_space = false; /* true=归一化加载(roi_zones 顶点已是模型坐标,
                                     无需按源分辨率缩放) */
    int last_src_w = 0;
    int last_src_h = 0;
    int src_w_now = 0; /* 当前解码源分辨率(frame_inlet 每帧写); 供推理通道 ROI 缩放用 */
    int src_h_now = 0;
    std::vector<DrawCommand> draw_cmds;
    std::string logic_name;
    std::shared_ptr<void> logic_state;
    cv::Mat last_frame;
    cv::Mat last_logic_frame;
    cv::Mat logic_display_frame;      /* logic 经 display_canvas() 自绘的显示底图(640×640
                                         BGR)；空=不覆盖，显示走实时采集帧 */
    uint64_t logic_display_ts_ms = 0; /* 上面那帧的产生时刻(steady ms)，显示端据此判新鲜度，过期回退实时帧 */
    int64_t logic_frame_id = 0;
    int64_t input_frame_seq = 0;
    uint64_t last_logic_ts_ms = 0;
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

    /*!< 2. 显示子系统 */
    Display_t dispDesc;   /*!< 显示窗口描述符 */
    char **pDispBuffer;   /*!< 双缓冲指针 (front buffer) */
    int disp_thread_exit; /*!< 显示线程退出标志 */

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
    volatile int isRunning;                   /*!< 全局运行标志: 0=退出 */

    /*!< 7. 线程句柄 (main 中 pthread_create 填充) */
    pthread_t config_monitor_tid; /*!< 配置热加载监控线程 */
    pthread_t fd_monitor_tid;     /*!< fd 使用量监控线程 */
    pthread_t upload_worker_tid;  /*!< Redis 报警上传线程 */

    /*!< 8. 配置热加载与线程退出标志 */
    uint64_t configLastMtime; /*!< 配置文件上次修改时间 */
    int config_monitor_exit;  /*!< 配置监控线程退出标志 */
    int fd_monitor_exit;      /*!< fd 监控线程退出标志 */
    int upload_worker_exit;   /*!< 上传线程退出标志 */
};

/*======================== 全局指针 ========================*/
extern APP_CTRL *g_pCtrl;

/*======================== 生命周期接口 ========================*/
#ifdef __cplusplus
extern "C"
{
#endif

    int app_ctrl_init(const char *cfgPath);
    void app_ctrl_deinit(void);

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

    /*======================== 动态计算属性 ========================*/
    static inline int app_ctrl_get_chn_nums(void)
    {
        return g_pCtrl ? (int)g_pCtrl->config.channels.size() : 0;
    }
    static inline int app_ctrl_get_enable_disp(void)
    {
        return g_pCtrl ? g_pCtrl->config.enable_display : 0;
    }
    static inline int app_ctrl_get_enable_rtsp(void)
    {
        return g_pCtrl ? (g_pCtrl->config.enable_rtsp ? 1 : 0) : 0;
    }
    static inline int app_ctrl_get_disp_width(void)
    {
        return g_pCtrl ? (g_pCtrl->config.disp_width & ~3) : 0;
    }
    static inline int app_ctrl_get_disp_height(void)
    {
        return g_pCtrl ? (g_pCtrl->config.disp_height & ~1) : 0;
    }
    static inline int app_ctrl_get_tile_cols(void)
    {
        return g_pCtrl ? (g_pCtrl->config.tile_cols > 0 ? g_pCtrl->config.tile_cols : 1) : 1;
    }
    static inline int app_ctrl_get_tile_rows(void)
    {
        if (!g_pCtrl)
            return 1;
        int c = app_ctrl_get_tile_cols();
        int n = app_ctrl_get_chn_nums();
        int req = (n + c - 1) / c;
        return g_pCtrl->config.tile_rows > 0 ? (g_pCtrl->config.tile_rows > req ? g_pCtrl->config.tile_rows : req)
                                             : req;
    }
    static inline int app_ctrl_get_max_fps(void)
    {
        return (g_pCtrl && g_pCtrl->config.max_fps > 0) ? g_pCtrl->config.max_fps : 30;
    }

#ifdef __cplusplus
}
#endif
