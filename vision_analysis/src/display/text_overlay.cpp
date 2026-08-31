/**
 * @file text_overlay.cpp
 * @brief 画面 UTF-8/中文 文本叠加实现 (见 text_overlay.h)
 *
 * 画面文字统一用 OpenCV freetype 模块渲染(中英文)。freetype 为必需(CMake 已强制),
 * 不再回退 Hershey。字体加载失败会打印明显错误, 文字将不绘制。
 */
#include "text_overlay.h"

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <list>
#include <opencv2/freetype.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

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

struct TextMaskKey
{
    std::string text;
    int font_height_px = 0;
    int stroke_width = 0;
    int shadow_width = 0;

    TextMaskKey() = default;
    TextMaskKey(std::string value, int height, int stroke, int shadow)
        : text(std::move(value)), font_height_px(height), stroke_width(stroke), shadow_width(shadow)
    {
    }

    bool operator==(const TextMaskKey &other) const
    {
        return font_height_px == other.font_height_px && stroke_width == other.stroke_width &&
               shadow_width == other.shadow_width && text == other.text;
    }
};

struct TextMaskKeyHash
{
    size_t operator()(const TextMaskKey &key) const
    {
        size_t hash = std::hash<std::string>{}(key.text);
        hash ^= static_cast<size_t>(key.font_height_px) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<size_t>(key.stroke_width) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<size_t>(key.shadow_width) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

struct CachedTextMask
{
    cv::Point offset_from_origin;
    cv::Mat glyph_mask;
    cv::Mat shadow_mask;
    size_t bytes = 0;
};

struct ThreadTextMaskCache
{
    using CacheEntry = std::pair<TextMaskKey, CachedTextMask>;
    using CacheList = std::list<CacheEntry>;

    CacheList lru;
    std::unordered_map<TextMaskKey, CacheList::iterator, TextMaskKeyHash> lookup;
    std::unordered_map<TextMaskKey, unsigned char, TextMaskKeyHash> admissions;
    size_t bytes = 0;
};

struct CachedGlyphMask
{
    CachedTextMask paint;
    int advance = 0;
};

struct TextMeasureKey
{
    std::string text;
    int font_height_px = 0;
    int thickness = 0;

    TextMeasureKey() = default;
    TextMeasureKey(std::string value, int height, int line_thickness)
        : text(std::move(value)), font_height_px(height), thickness(line_thickness)
    {
    }

    bool operator==(const TextMeasureKey &other) const
    {
        return font_height_px == other.font_height_px && thickness == other.thickness && text == other.text;
    }
};

struct TextMeasureKeyHash
{
    size_t operator()(const TextMeasureKey &key) const
    {
        size_t hash = std::hash<std::string>{}(key.text);
        hash ^= static_cast<size_t>(key.font_height_px) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<size_t>(key.thickness) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

struct TextMeasureValue
{
    cv::Size size;
    int baseline = 0;

    TextMeasureValue() = default;
    TextMeasureValue(cv::Size measured_size, int measured_baseline)
        : size(measured_size), baseline(measured_baseline)
    {
    }
};

thread_local ThreadFreeTypeState g_thread_ft;
thread_local ThreadTextMaskCache g_text_mask_cache;
thread_local std::unordered_map<TextMaskKey, CachedGlyphMask, TextMaskKeyHash> g_glyph_mask_cache;
thread_local std::unordered_map<TextMeasureKey, TextMeasureValue, TextMeasureKeyHash> g_text_measure_cache;
std::atomic<bool> g_font_loaded_logged{false};
std::atomic<bool> g_font_error_logged{false};

constexpr size_t kMaxCachedTextMasks = 96;
constexpr size_t kMaxCachedTextBytes = 8U * 1024U * 1024U;
constexpr size_t kMaxTextAdmissions = 256;
constexpr unsigned char kAdmissionHits = 3;
constexpr size_t kMaxTextMeasurements = 256;
constexpr size_t kMaxCachedGlyphMasks = 1024;

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

size_t mat_bytes(const cv::Mat &mat)
{
    return mat.empty() ? 0U : mat.total() * mat.elemSize();
}

bool build_text_mask(const TextMaskKey &key, CachedTextMask &entry)
{
    cv::Size text_size;
    int baseline = 0;
    if (!measure_text_unicode(key.text, key.font_height_px, /*filled*/ -1, text_size, baseline))
        return false;

    const int guard = key.font_height_px + key.stroke_width + key.shadow_width + 4;
    const int vertical_span = text_size.height + std::abs(baseline) + guard;
    const int mask_width = std::max(1, text_size.width + guard * 2);
    const int mask_height = std::max(1, vertical_span * 2);
    cv::Mat full_glyph(mask_height, mask_width, CV_8UC1, cv::Scalar(0));
    const cv::Point local_origin(guard, mask_height / 2);
    if (!draw_text_unicode(full_glyph, key.text, local_origin, key.font_height_px, cv::Scalar(255), /*filled*/ -1))
        return false;
    if (key.stroke_width > 0 &&
        !draw_text_unicode(full_glyph, key.text, local_origin, key.font_height_px, cv::Scalar(255), key.stroke_width))
        return false;

    /* 不同板端 OpenCV/FreeType 组合在单通道 Mat 上可能输出 1 或 255 作为字形覆盖值。
     * 缓存阶段统一归一化为 0/255，后续 setTo 只依赖“是否为字形像素”，避免把值1
     * 误当作 1/255 透明度而导致中文、英文和数字全部接近不可见。 */
    cv::threshold(full_glyph, full_glyph, 0, 255, cv::THRESH_BINARY);

    const cv::Rect glyph_bounds = cv::boundingRect(full_glyph);
    if (glyph_bounds.empty())
        return false;
    const cv::Rect full_bounds(0, 0, mask_width, mask_height);
    const cv::Rect paint_bounds(glyph_bounds.x - key.shadow_width, glyph_bounds.y - key.shadow_width,
                                glyph_bounds.width + key.shadow_width * 2,
                                glyph_bounds.height + key.shadow_width * 2);
    const cv::Rect paint_roi = paint_bounds & full_bounds;
    if (paint_roi.empty())
        return false;

    entry.offset_from_origin = paint_roi.tl() - local_origin;
    entry.glyph_mask = full_glyph(paint_roi).clone();
    if (key.shadow_width > 0)
    {
        cv::Mat full_shadow;
        const cv::Mat kernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE, cv::Size(key.shadow_width * 2 + 1, key.shadow_width * 2 + 1));
        cv::dilate(full_glyph, full_shadow, kernel);
        entry.shadow_mask = full_shadow(paint_roi).clone();
    }
    entry.bytes = mat_bytes(entry.glyph_mask) + mat_bytes(entry.shadow_mask) + key.text.capacity();
    return true;
}

void evict_oldest_text_mask()
{
    ThreadTextMaskCache &cache = g_text_mask_cache;
    if (cache.lru.empty())
        return;
    auto oldest = std::prev(cache.lru.end());
    cache.bytes -= std::min(cache.bytes, oldest->second.bytes);
    cache.lookup.erase(oldest->first);
    cache.lru.erase(oldest);
}

CachedTextMask *cached_text_mask(const std::string &text, int font_height_px, int stroke_width, int shadow_width,
                                 bool &render_as_glyphs)
{
    render_as_glyphs = false;
    ThreadTextMaskCache &cache = g_text_mask_cache;
    TextMaskKey key(text, font_height_px, stroke_width, shadow_width);
    auto found = cache.lookup.find(key);
    if (found != cache.lookup.end())
    {
        cache.lru.splice(cache.lru.begin(), cache.lru, found->second);
        return &found->second->second;
    }

    /* 整行中带实时数字时，文本往往每帧变化。首次出现就放入 LRU 会使
     * 96 个槽位持续失效和淘汰。只有同一内容在本线程观测到多次后才晋升为
     * 整行缓存；其余帧转交 Unicode 字形缓存拼装，不污染整行 LRU。 */
    if (cache.admissions.size() >= kMaxTextAdmissions && cache.admissions.find(key) == cache.admissions.end())
        cache.admissions.clear();
    unsigned char &hits = cache.admissions[key];
    if (hits < kAdmissionHits)
        ++hits;
    const bool admit_to_cache = hits >= kAdmissionHits;

    if (!admit_to_cache)
    {
        render_as_glyphs = true;
        return nullptr;
    }

    CachedTextMask created;
    if (!build_text_mask(key, created))
    {
        render_as_glyphs = true;
        return nullptr;
    }

    if (created.bytes > kMaxCachedTextBytes)
    {
        render_as_glyphs = true;
        return nullptr;
    }

    cache.admissions.erase(key);
    while (!cache.lru.empty() &&
           (cache.lru.size() >= kMaxCachedTextMasks || cache.bytes + created.bytes > kMaxCachedTextBytes))
        evict_oldest_text_mask();
    cache.bytes += created.bytes;
    cache.lru.emplace_front(std::move(key), std::move(created));
    cache.lookup.emplace(cache.lru.front().first, cache.lru.begin());
    return &cache.lru.front().second;
}

size_t utf8_codepoint_width(const std::string &text, size_t offset)
{
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    size_t width = 1;
    if ((lead & 0xE0U) == 0xC0U)
        width = 2;
    else if ((lead & 0xF0U) == 0xE0U)
        width = 3;
    else if ((lead & 0xF8U) == 0xF0U)
        width = 4;
    if (offset + width > text.size())
        return 1;
    for (size_t index = 1; index < width; ++index)
        if ((static_cast<unsigned char>(text[offset + index]) & 0xC0U) != 0x80U)
            return 1;
    return width;
}

CachedGlyphMask *cached_glyph_mask(const TextMaskKey &key)
{
    auto found = g_glyph_mask_cache.find(key);
    if (found != g_glyph_mask_cache.end())
        return &found->second;

    /* 字形缓存只包含单个 Unicode 码点，实时数字再怎么变化也只会使用
     * 0~9、少量标点和固定汉字。上限只是对异常输入的防护；超限后不再接纳
     * 新字形，不清空已有缓存，避免运行中反复抖动。 */
    if (g_glyph_mask_cache.size() >= kMaxCachedGlyphMasks)
        return nullptr;

    CachedGlyphMask created;
    cv::Ptr<cv::freetype::FreeType2> ft = ensure_thread_ft2();
    if (!ft)
        return nullptr;
    cv::Size glyph_size;
    int baseline = 0;
    try
    {
        glyph_size = ft->getTextSize(key.text, key.font_height_px, /*filled*/ -1, &baseline);
    }
    catch (...)
    {
        return nullptr;
    }
    created.advance = std::max(0, glyph_size.width);
    if (key.text == "\t")
        created.advance = std::max(created.advance, key.font_height_px * 2);
    else if (key.text == " " && created.advance <= 0)
        created.advance = std::max(1, key.font_height_px / 3);

    if (glyph_size.width > 0 && glyph_size.height > 0 && !build_text_mask(key, created.paint))
        return nullptr;
    auto inserted = g_glyph_mask_cache.emplace(key, std::move(created));
    return &inserted.first->second;
}

bool paint_text_mask(cv::Mat &img, const CachedTextMask &entry, cv::Point origin, const cv::Scalar &color,
                     bool paint_shadow, const cv::Scalar &shadow_color)
{
    const cv::Rect wanted(origin + entry.offset_from_origin, entry.glyph_mask.size());
    const cv::Rect clipped = wanted & cv::Rect(0, 0, img.cols, img.rows);
    if (clipped.empty())
        return true;
    const cv::Rect mask_roi(clipped.x - wanted.x, clipped.y - wanted.y, clipped.width, clipped.height);
    cv::Mat destination = img(clipped);
    if (paint_shadow && !entry.shadow_mask.empty())
        destination.setTo(shadow_color, entry.shadow_mask(mask_roi));
    else if (!paint_shadow && !entry.glyph_mask.empty())
        destination.setTo(color, entry.glyph_mask(mask_roi));
    return true;
}

bool draw_text_from_cached_glyphs(cv::Mat &img, const std::string &text, cv::Point origin, int font_height_px,
                                  int stroke_width, int shadow_width, const cv::Scalar &color,
                                  bool shadow_enabled, const cv::Scalar &shadow_color)
{
    struct PositionedGlyph
    {
        CachedGlyphMask *glyph = nullptr;
        cv::Point origin;

        PositionedGlyph(CachedGlyphMask *value, cv::Point value_origin) : glyph(value), origin(value_origin) {}
    };
    std::vector<PositionedGlyph> layout;
    layout.reserve(text.size());
    int pen_x = origin.x;
    for (size_t offset = 0; offset < text.size();)
    {
        const size_t width = utf8_codepoint_width(text, offset);
        const std::string codepoint = text.substr(offset, width);
        offset += width;
        const TextMaskKey key(codepoint, font_height_px, stroke_width, shadow_width);
        CachedGlyphMask *glyph = cached_glyph_mask(key);
        if (!glyph)
            return false;
        layout.emplace_back(glyph, cv::Point(pen_x, origin.y));
        pen_x += glyph->advance;
    }

    /* 先统一画所有阴影，再画前景。如果按字符交替画，后一字的阴影可能
     * 覆盖前一字的前景边缘。 */
    if (shadow_enabled)
        for (const PositionedGlyph &positioned : layout)
            paint_text_mask(img, positioned.glyph->paint, positioned.origin, color, true, shadow_color);
    for (const PositionedGlyph &positioned : layout)
        paint_text_mask(img, positioned.glyph->paint, positioned.origin, color, false, shadow_color);
    return true;
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
    if (font_height_px < 1)
        font_height_px = 1;

    const TextMeasureKey key(utf8, font_height_px, thickness);
    const auto cached = g_text_measure_cache.find(key);
    if (cached != g_text_measure_cache.end())
    {
        size = cached->second.size;
        baseline = cached->second.baseline;
        return size.width > 0 && size.height > 0;
    }

    cv::Ptr<cv::freetype::FreeType2> ft = ensure_thread_ft2();
    if (!ft)
        return false;
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
    if (size.width <= 0 || size.height <= 0)
        return false;
    if (g_text_measure_cache.size() >= kMaxTextMeasurements)
        g_text_measure_cache.clear();
    g_text_measure_cache.emplace(key, TextMeasureValue(size, baseline));
    return true;
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

bool draw_text_unicode_cached(cv::Mat &img, const std::string &utf8, cv::Point org, int font_height_px,
                              const cv::Scalar &color, int thickness, bool shadow_enabled,
                              const cv::Scalar &shadow_color, int shadow_width)
{
    if (img.empty() || utf8.empty())
        return false;
    if (img.type() != CV_8UC3)
    {
        const int direct_shadow_width = std::max(1, std::min(shadow_width, 8));
        if (shadow_enabled &&
            !draw_text_unicode(img, utf8, org, font_height_px, shadow_color, direct_shadow_width))
            return false;
        if (!draw_text_unicode(img, utf8, org, font_height_px, color, /*filled*/ -1))
            return false;
        if (thickness >= 2)
            return draw_text_unicode(img, utf8, org, font_height_px, color, std::min(thickness - 1, 6));
        return true;
    }
    font_height_px = std::max(1, font_height_px);
    const int stroke_width = thickness >= 2 ? std::min(thickness - 1, 6) : 0;
    const int normalized_shadow_width = shadow_enabled ? std::max(1, std::min(shadow_width, 8)) : 0;
    bool render_as_glyphs = false;
    CachedTextMask *entry =
        cached_text_mask(utf8, font_height_px, stroke_width, normalized_shadow_width, render_as_glyphs);
    if (render_as_glyphs)
        return draw_text_from_cached_glyphs(img, utf8, org, font_height_px, stroke_width,
                                            normalized_shadow_width, color, shadow_enabled, shadow_color);
    if (!entry || entry->glyph_mask.empty())
        return false;

    const cv::Rect wanted(org + entry->offset_from_origin, entry->glyph_mask.size());
    const cv::Rect clipped = wanted & cv::Rect(0, 0, img.cols, img.rows);
    if (clipped.empty())
    {
        /* 基线点位于画面内、缓存包围框却完全在画面外，说明当前板端的 FreeType
         * 基线方向与缓存测量不一致。返回 false，让上层走直接绘制兜底。 */
        const bool origin_near_image = org.x >= 0 && org.x < img.cols && org.y >= 0 && org.y < img.rows;
        return !origin_near_image;
    }
    if (shadow_enabled)
        paint_text_mask(img, *entry, org, color, true, shadow_color);
    paint_text_mask(img, *entry, org, color, false, shadow_color);
    return true;
}
