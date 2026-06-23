/**
 * @file rtsp_streamer.h
 * @brief 内置 RTSP 推流 — 把"显示屏拼接画面"编码后通过 RTSP 对外提供
 *
 * 取流点:
 *   = main 分配的拼接显示缓冲 g_disp.front (*g_pCtrl->pDispBuffer)。
 *   它是
 * RGB、disp_width×disp_height、含全部叠加框(检测框/自定义绘制/ROI/FPS)的整幅画面，
 *   与接显示器看到的内容完全一致。
 *
 * 设计要点:
 *   - 使用已链接的 gst-rtsp-server (见 src/third_party/gst_opt/api.cmake)，
 *     在板上起 rtsp://<板IP>:<port><path>，无需引入新依赖。
 *   - 服务跑在【独立 GMainContext + 独立线程】，与 GTK 显示(若开启)互不干扰；
 *     无显示器(enable_display=false)时只要 enable_rtsp=true 也能对外推流。
 *   - 编码优先硬件 mpph264enc/mpph265enc，缺失时自动回退软件 x264enc/x265enc。
 *   - 推帧用 g_signal_emit_by_name(appsrc,"push-buffer",...)，与采集端
 * pull-sample 风格一致， 不需要链接 gstreamer-app。
 *
 * 接入 (main.cpp):
 *   1. 分配 g_disp 缓冲的条件需含 enable_rtsp:
 *        if (app_ctrl_get_enable_disp() || app_ctrl_get_enable_rtsp()) { ...
 * dispBufferMap ... }
 *   2. videoOutHandle 推显示队列的条件需含 enable_rtsp (frame_inlet.cpp)。
 *   3. 线程创建区调用 rtsp_streamer_init();
 *   4. 退出序列调用 rtsp_streamer_deinit();
 */
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 按配置启动 RTSP 服务。
     *
     * enable_rtsp=false 时直接返回 0(不做任何事)。
     * 必须在 g_disp 显示缓冲已分配之后调用 (依赖 *g_pCtrl->pDispBuffer)。
     * @return 0 = 成功或未启用; -1 = 启动失败 (调用方可忽略, 继续无 RTSP 运行)。
     */
    int rtsp_streamer_init(void);

    /** @brief 停止 RTSP 服务与推流线程 (幂等; 未启用时为空操作)。 */
    void rtsp_streamer_deinit(void);

#ifdef __cplusplus
}
#endif
