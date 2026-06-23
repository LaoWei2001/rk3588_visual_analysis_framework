/**
 * @file text_overlay.cpp
 * @brief 画面 UTF-8/中文 文本叠加实现 (见 text_overlay.h)
 *
 * 画面文字统一用 OpenCV freetype 模块渲染(中英文)。freetype 为必需(CMake
 * 已强制), 不再回退 Hershey。字体加载失败会打印明显错误, 文字将不绘制。
 */
#include "text_overlay.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <opencv2/freetype.hpp>

namespace
{

std::mutex g_ft_mtx;                    /* 保护 g_ft2 的加载与 putText(freetype face 非线程安全) */
cv::Ptr<cv::freetype::FreeType2> g_ft2; /* 空 = 字体未加载成功 */
bool g_ft_tried = false;

/* 需在持有 g_ft_mtx 时调用。返回可用的 FreeType2(或空)。 */
cv::Ptr<cv::freetype::FreeType2> ensure_ft2_locked()
{
    if (g_ft_tried)
        return g_ft2;
    g_ft_tried = true;

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
            g_ft2 = ft;
            fprintf(stderr, "[text_overlay] font loaded: %s\n", p);
            break;
        }
        catch (...)
        {
            /* 加载失败, 尝试下一个候选 */
        }
    }
    if (!g_ft2)
        fprintf(stderr, "[text_overlay][ERROR] 未能加载任何字体! 画面文字将无法显示。\n"
                        "  请放置字体到 ./assets/fonts/overlay.ttf, 或设置环境变量 "
                        "RK_OVERLAY_FONT=/path/to/font,\n"
                        "  或确认 /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc 存在。\n");
    return g_ft2;
}

} // namespace

bool text_overlay_available()
{
    std::lock_guard<std::mutex> lk(g_ft_mtx);
    return (bool)ensure_ft2_locked();
}

bool draw_text_unicode(cv::InputOutputArray img, const std::string &utf8, cv::Point org, int font_height_px,
                       const cv::Scalar &color, int thickness)
{
    if (utf8.empty() || img.empty())
        return false;
    std::lock_guard<std::mutex> lk(g_ft_mtx);
    cv::Ptr<cv::freetype::FreeType2> ft = ensure_ft2_locked();
    if (!ft)
        return false;
    if (font_height_px < 1)
        font_height_px = 1;
    try
    {
        ft->putText(img, utf8, org, font_height_px, color, thickness, cv::LINE_AA,
                    /*bottomLeftOrigin=*/true);
    }
    catch (...)
    {
        return false;
    }
    return true;
}
