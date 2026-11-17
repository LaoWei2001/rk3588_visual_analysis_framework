/** @file rendering.h
 * @brief Shared BGR image overlays for video, still-image inference and offline logic replay.
 */
#pragma once
#include <config_types.h>
#include <opencv2/core.hpp>
#include <types.h>

namespace vision
{
/** Borrowed input lists must remain alive for the duration of render_overlays().
 * Coordinates are expressed in inputW/inputH and scaled to the destination image.
 * inputW/inputH must be positive; bgr must be a nonempty CV_8UC3 image.
 * Rendering modifies only the supplied Mat; it does not read a runtime session.
 */
struct OverlayOptions
{
    int chnId = 0;
    int inputW = 0, inputH = 0;
    float disp_fps = 0.0f;
    float infer_fps = 0.0f;
    /* >0: 同一版本的结果矩阵必须保持不可变，可跨显示帧复用分割/深度缓存。
     * <=0: 不缓存可变结果；独立调用方可以原位修改同一 Mat 后重新绘制。 */
    int64_t result_frame_id = 0;
    int show_fps = 1;
    uint8_t target_mask = DrawCommand::DISPLAY;
    bool show_system_overlays = true; /* ROI、检测框、姿态、分割 */
    bool show_custom_overlays = true; /* logic 的 draw_* 指令 */
    /* 深度通道使用纯热力图底图；首个深度结果到达前显示黑色。
     * 底图选择独立于系统/业务绘图开关。 */
    bool depth_background = false;

    /* 推理 ROI 使用完整视频归一化坐标，仅供最终显示层绘制；绝不参与模型输入构造。 */
    const InferenceRoiConfig *inference_roi = nullptr;
    /* 本通道全部 ROI 区域(顶点均为业务坐标系(640×640)); render_overlays 按 inputW/inputH 缩放后逐个绘制。 */
    const std::vector<RoiZone> *roi_zones = nullptr;
    const std::vector<AlgoResult> *results = nullptr;
    const std::vector<DrawCommand> *draw_cmds = nullptr;

    /* 帧率文字的像素字号：<=0 随画面自动缩放（默认），>0 固定字号。
     * 例如 options.fps_font_height = 18; 边距也会固定，不随窗口变化。 */
    int fps_font_height = 0;
    bool show_network_rtt = false; ///< 在帧率旁显示到摄像头的网络往返延迟。
    float network_rtt_ms = -1.0f;  ///< 填入测得的毫秒数；负数显示 --。绘图函数不会联网。
};

/* Reuse per output image when updating the performance strip between inference results. */
struct OverlayCache
{
    cv::Mat background;
    float render_fps = 0.0f;
    float infer_fps = 0.0f;
    bool visible = false;
    bool show_network_rtt = false;
    float network_rtt_ms = -1.0f;
};

/** @brief 把检测、ROI 和业务指令画到 bgr；坐标从 options.inputW/H 缩放到图像尺寸。
 * @note 要求非空 CV_8UC3 与正 inputW/H；列表只借用到返回，cache 可为空或由出口独立持有。
 * 示例：
 * @code
 * vision::OverlayOptions options;
 * options.inputW = options.inputH = 640;
 * options.show_fps = 0;
 * cv::Mat image(640, 640, CV_8UC3, cv::Scalar(0));
 * vision::render_overlays(image, options);
 * @endcode
 */
void render_overlays(cv::Mat &bgr, const OverlayOptions &options, OverlayCache *cache = nullptr);
/** @brief 返回底部帧率文字的刷新区域，坐标属于输入 view。
 * @note 使用固定字号时传入与绘制相同的 options，确保刷新区域能覆盖文字。
 * 示例：
 * @code
 * cv::Mat image(640, 640, CV_8UC3);
 * cv::Rect strip = vision::performance_overlay_bounds(image);
 * @endcode
 */
cv::Rect performance_overlay_bounds(const cv::Mat &view, const OverlayOptions &options = OverlayOptions{});
/** @brief 在右下角绘制帧率并更新可选缓存；bgr 应为非空 CV_8UC3。
 * @note 默认随画面缩放；options.fps_font_height > 0 时使用固定像素字号和边距。
 * @note 重复刷新同一底图前先恢复 cache.background，否则文字会叠加；每个出口独立缓存。
 * 示例：
 * @code
 * vision::OverlayOptions options;
 * cv::Mat image(640, 640, CV_8UC3, cv::Scalar(0));
 * vision::render_performance_overlay(image, options);
 * @endcode
 */
void render_performance_overlay(cv::Mat &bgr, const OverlayOptions &options, OverlayCache *cache = nullptr);
} // namespace vision
