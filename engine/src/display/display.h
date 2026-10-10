#pragma once
#include <config_types.h>
#include <opencv2/opencv.hpp>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

/*======================== render_overlays 参数包 ========================*/
struct RenderParams
{
    int chnId = 0;
    int inputW = 0, inputH = 0;
    float disp_fps = 0.0f;
    float infer_fps = 0.0f;
    int64_t result_frame_id = 0; /* 分割叠加缓存版本；同一推理结果可跨多个显示帧复用 */
    int show_fps = 1;
    uint8_t target_mask = DrawCommand::DISPLAY;
    bool show_system_overlays = true; /* ROI、检测框、姿态、分割 */
    bool show_custom_overlays = true; /* logic 的 draw_* 指令 */

    /* 推理 ROI 使用完整视频归一化坐标，仅供最终显示层绘制；绝不参与模型输入构造。 */
    const InferenceRoiConfig *inference_roi = nullptr;
    /* 本通道全部 ROI 区域(顶点均为业务坐标系(640×640)); render_overlays 按 inputW/inputH 缩放后逐个绘制。 */
    const std::vector<RoiZone> *roi_zones = nullptr;
    const std::vector<AlgoResult> *results = nullptr;
    const std::vector<DrawCommand> *draw_cmds = nullptr;
};

struct ChannelContext;
RenderParams channel_render_params(const ChannelContext &ctx);

/* 仅缓存 FPS 文字下方的一条背景，关闭性能显示时不保存图像。 */
struct PerformanceOverlayCache
{
    cv::Mat background;
    float render_fps = 0.0f;
    float infer_fps = 0.0f;
    bool visible = false;
};

cv::Rect performance_overlay_bounds(const cv::Mat &view);
void render_performance_overlay(cv::Mat &view, const RenderParams &params, PerformanceOverlayCache *cache = nullptr);

typedef struct
{
    const char *winTitle;
    int x;
    int y;
    int width;
    int height;
} Display_t;

char **dispBufferMap(Display_t *dispDesc);
void dispBufferUnmap(void);
int display(Display_t *dispDesc);

// Display buffer lock to avoid tearing when GTK reads while pipeline writes.
void display_lock();
void display_unlock();
bool display_try_lock();

/**
 * @brief 将所有 overlay（ROI、检测框、draw_cmds、FPS 文字）画到 screen_roi 上。
 *
 * screen_roi 必须是 BGR 格式的 cv::Mat（与 OpenCV draw 函数约定一致）。
 * display 路径在写入 front buffer 前会统一做 BGR→RGB 转换。
 *
 * RenderParams 是显示/媒体出口内部参数，由引擎构造。
 */
void render_overlays(cv::Mat &screen_roi, const RenderParams &p, PerformanceOverlayCache *performance_cache = nullptr);

/* Render one channel with the same rules as the live view. The caller owns the
 * BGR image and target size; this function never reads or writes framebuffer. */
void render_channel_view(cv::Mat &bgr, int chn_id, uint64_t frame_timestamp_ms = 0,
                         PerformanceOverlayCache *performance_cache = nullptr);

/**
 * @brief 高性能统一文字绘制出口。
 *
 * 用 FreeType 生成灰度字形蒙版；重复出现的稳定整行文本会晋升到线程内有界 LRU，
 * 包含实时数字、每帧变化的文本由 Unicode 字形缓存拼装，数值变化不再触发整行
 * FreeType 重新栅格化。命中时只在文字
 * 实际包围框内按二值蒙版着色。缓存阶段将不同
 * OpenCV/FreeType 实现输出的字形值统一归一化为 0/255；开启重影时，膨胀后的重影蒙版也只生成
 * 一次。缓存不包含颜色和坐标，因此同一段文字可在不同位置、颜色间复用。
 *
 * @param thickness       逻辑粗细：<=1 为填充字；>=2 为填充字加同色笔画。
 * @param shadow_enabled  true=绘制重影/外描边；false=只绘制前景文字。
 * @param shadow_color    重影 BGR 颜色。
 * @param shadow_width    重影膨胀宽度，内部限制为 1~8 像素。
 */
bool draw_text_unicode_cached(cv::Mat &img, const std::string &utf8, cv::Point org, int font_height_px,
                              const cv::Scalar &color, int thickness, bool shadow_enabled,
                              const cv::Scalar &shadow_color, int shadow_width);
