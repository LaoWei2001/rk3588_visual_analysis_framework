/**
 * @file text_overlay.cpp
 * @brief 画面 UTF-8/中文 文本叠加实现 (见 text_overlay.h)
 *
 * 画面文字统一用 OpenCV freetype 模块渲染(中英文)。freetype 为必需(CMake 已强制),
 * 不再回退 Hershey。字体加载失败会打印明显错误, 文字将不绘制。
 */
#include "text_overlay.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <opencv2/freetype.hpp>

namespace
{

/* FreeType face 不能被多线程并发调用。原实现用一个全局 face + 全局锁，
 * 四路显示的所有中英文都被强制串行；状态文字较多时每路只剩约 4~5 FPS。
 * 改为每个渲染线程独立持有 face：同一 face 仍只在本线程调用，但不同通道
 * 可以并行栅格化文字，无需跨通道互斥。 */
struct ThreadFreeTypeState
{
    cv::Ptr<cv::freetype::FreeType2> ft2; /* 空 = 本线程字体未加载成功 */
    bool tried = false;
};

thread_local ThreadFreeTypeState g_thread_ft;
std::atomic<bool> g_font_loaded_logged{false};
std::atomic<bool> g_font_error_logged{false};

/* 返回本线程专用的 FreeType2(或空)。每个线程最多加载一次字体。 */
cv::Ptr<cv::freetype::FreeType2> ensure_thread_ft2()
{
    if (g_thread_ft.tried)
        return g_thread_ft.ft2;
    g_thread_ft.tried = true;

    const char *env = std::getenv("RK_OVERLAY_FONT");
    const char *candidates[] = {
        env,
        "./assets/fonts/overlay.ttf",
        "./assets/fonts/overlay.ttc",
        "./assets/fonts/overlay.otf",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        nullptr,
    };

    for (const char *p : candidates)
    {
        if (!p || !*p)
            continue;
        try
        {
            cv::Ptr<cv::freetype::FreeType2> ft = cv::freetype::createFreeType2();
            ft->loadFontData(std::string(p), 0); /* TTC: 取第 0 个 face */
            g_thread_ft.ft2 = ft;
            if (!g_font_loaded_logged.exchange(true, std::memory_order_relaxed))
                fprintf(stderr, "[text_overlay] font loaded: %s (thread-local renderer)\n", p);
            break;
        }
        catch (...)
        {
            /* 加载失败, 尝试下一个候选 */
        }
    }
    if (!g_thread_ft.ft2 && !g_font_error_logged.exchange(true, std::memory_order_relaxed))
        fprintf(stderr, "[text_overlay][ERROR] 未能加载任何字体! 画面文字将无法显示。\n"
                        "  请放置字体到 ./assets/fonts/overlay.ttf, 或设置环境变量 RK_OVERLAY_FONT=/path/to/font,\n"
                        "  或确认 /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc 存在。\n");
    return g_thread_ft.ft2;
}

} // namespace

bool text_overlay_available()
{
    return (bool)ensure_thread_ft2();
}

bool measure_text_unicode(const std::string &utf8, int font_height_px, int thickness, cv::Size &size, int &baseline)
{
    size = cv::Size();
    baseline = 0;
    if (utf8.empty())
        return false;
    cv::Ptr<cv::freetype::FreeType2> ft = ensure_thread_ft2();
    if (!ft)
        return false;
    if (font_height_px < 1)
        font_height_px = 1;
    try
    {
        size = ft->getTextSize(utf8, font_height_px, thickness, &baseline);
    }
    catch (...)
    {
        size = cv::Size();
        baseline = 0;
        return false;
    }
    return size.width > 0 && size.height > 0;
}

bool draw_text_unicode(cv::InputOutputArray img, const std::string &utf8, cv::Point org, int font_height_px,
                       const cv::Scalar &color, int thickness)
{
    if (utf8.empty() || img.empty())
        return false;
    cv::Ptr<cv::freetype::FreeType2> ft = ensure_thread_ft2();
    if (!ft)
        return false;
    if (font_height_px < 1)
        font_height_px = 1;
    try
    {
        ft->putText(img, utf8, org, font_height_px, color, thickness, cv::LINE_AA, /*bottomLeftOrigin=*/true);
    }
    catch (...)
    {
        return false;
    }
    return true;
}
