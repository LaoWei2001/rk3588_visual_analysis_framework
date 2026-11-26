#include "text_overlay.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <drawing.h>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <rendering.h>
#include <vector>

/* 唯一文字绘制出口：普通文字和重影文字共用同一个缓存渲染器。
 * FreeType 字形、加粗蒙版和重影膨胀蒙版只在缓存未命中时生成；命中时只在文字实际包围框
 * 内按蒙版着色。缓存为线程局部，因此四路显示可以并行且不需要全局字体锁。 */
static inline void put_text_auto(cv::Mat &img, const std::string &s, cv::Point org, double font_scale,
                                 const cv::Scalar &color, int thickness, bool shadow_enabled = false,
                                 const cv::Scalar &shadow_color = cv::Scalar(0, 0, 0), int shadow_width = 2)
{
    const int fh = std::max(12, (int)std::lround(font_scale * 30.0));
    if (draw_text_unicode_cached(img, s, org, fh, color, thickness, shadow_enabled, shadow_color, shadow_width))
        return;

    /* 缓存构建失败时必须保证文字仍然可见。直接路径只在异常情况下执行，不影响正常缓存性能。 */
    if (shadow_enabled)
        draw_text_unicode(img, s, org, fh, shadow_color, std::max(1, std::min(shadow_width, 8)));
    draw_text_unicode(img, s, org, fh, color, /*filled*/ -1);
    if (thickness >= 2)
        draw_text_unicode(img, s, org, fh, color, std::min(thickness - 1, 6));
}

/* YOLOv8 COCO 17点人体骨架，索引从0开始。 */
static const std::pair<int, int> kCoco17Skeleton[] = {
    {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12}, {5, 6}, {5, 7}, {6, 8},
    {7, 9},   {8, 10},  {1, 2},   {0, 1},   {0, 2},   {1, 3},  {2, 4},  {3, 5}, {4, 6}};

/* 21点手部骨架：0=腕部，其后每4点依次为拇指、食指、中指、无名指、小指。 */
static const std::pair<int, int> kHand21Skeleton[] = {
    {0, 1},   {1, 2},   {2, 3},  {3, 4},   {0, 5},   {5, 6},   {6, 7},  {7, 8},   {0, 9},   {9, 10},
    {10, 11}, {11, 12}, {0, 13}, {13, 14}, {14, 15}, {15, 16}, {0, 17}, {17, 18}, {18, 19}, {19, 20}};

struct PoseSkeletonView
{
    const std::pair<int, int> *edges;
    size_t count;
};

static PoseSkeletonView pose_skeleton_for(size_t keypoint_count)
{
    if (keypoint_count == 17)
        return {kCoco17Skeleton, sizeof(kCoco17Skeleton) / sizeof(kCoco17Skeleton[0])};
    if (keypoint_count == 21)
        return {kHand21Skeleton, sizeof(kHand21Skeleton) / sizeof(kHand21Skeleton[0])};
    return {nullptr, 0};
}

static bool pose_keypoint_visible(const AlgoResult &res, size_t index)
{
    if (index >= res.keypoints.size())
        return false;
    const cv::Point2f &point = res.keypoints[index];
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0.0f || point.y < 0.0f)
        return false;
    return res.keypoint_scores.size() != res.keypoints.size() || res.keypoint_scores[index] >= 0.25f;
}

namespace
{

struct SegmentationColorLuts
{
    cv::Mat b;
    cv::Mat g;
    cv::Mat r;

    SegmentationColorLuts()
    {
        static const unsigned char class_colors[][3] = {
            {255, 56, 56},  {255, 157, 151}, {255, 112, 31}, {255, 178, 29},  {207, 210, 49},
            {72, 249, 10},  {146, 204, 23},  {61, 219, 134}, {26, 147, 52},   {0, 212, 187},
            {44, 153, 168}, {0, 194, 255},   {52, 69, 147},  {100, 115, 255}, {0, 24, 236},
            {132, 56, 255}, {82, 0, 133},    {203, 56, 255}, {255, 149, 200}, {255, 55, 199}};

        b = cv::Mat::zeros(1, 256, CV_8UC1);
        g = cv::Mat::zeros(1, 256, CV_8UC1);
        r = cv::Mat::zeros(1, 256, CV_8UC1);
        for (int class_id_plus_1 = 1; class_id_plus_1 < 256; ++class_id_plus_1)
        {
            const int color_idx = (class_id_plus_1 - 1) % 20;
            /* class_colors 是 RGB；显示渲染阶段统一使用 BGR。 */
            b.at<unsigned char>(0, class_id_plus_1) = class_colors[color_idx][2];
            g.at<unsigned char>(0, class_id_plus_1) = class_colors[color_idx][1];
            r.at<unsigned char>(0, class_id_plus_1) = class_colors[color_idx][0];
        }
    }
};

static const SegmentationColorLuts &segmentation_color_luts()
{
    static const SegmentationColorLuts luts;
    return luts;
}

struct SegmentationOverlayCache
{
    int64_t result_frame_id = -1;
    const unsigned char *source_data = nullptr;
    int source_rows = 0;
    int source_cols = 0;
    int source_type = -1;
    cv::Size output_size;
    cv::Rect active_rect;
    cv::Mat active_mask;
    cv::Mat active_color_bgr;
    cv::Mat blend_scratch;
    bool prepared = false;

    bool matches(const cv::Mat &source, const cv::Size &size, int64_t frame_id) const
    {
        /* frame_id<=0 的调用方没有稳定的结果版本号，宁可重建也不能误用旧掩码。 */
        return frame_id > 0 && prepared && result_frame_id == frame_id && source_data == source.data &&
               source_rows == source.rows && source_cols == source.cols && source_type == source.type() &&
               output_size == size;
    }

    void prepare(const cv::Mat &source, const cv::Size &size, int64_t frame_id)
    {
        result_frame_id = frame_id;
        source_data = source.data;
        source_rows = source.rows;
        source_cols = source.cols;
        source_type = source.type();
        output_size = size;
        active_rect = cv::Rect();
        active_mask.release();
        active_color_bgr.release();
        prepared = true;

        if (source.empty() || source.type() != CV_8UC1 || size.width <= 0 || size.height <= 0)
            return;

        cv::Mat resized_mask;
        cv::resize(source, resized_mask, size, 0, 0, cv::INTER_NEAREST);

        cv::Mat foreground_mask;
        cv::compare(resized_mask, 0, foreground_mask, cv::CMP_NE);
        active_rect = cv::boundingRect(foreground_mask);
        if (active_rect.empty())
            return;

        active_mask = foreground_mask(active_rect).clone();
        const cv::Mat class_ids = resized_mask(active_rect);
        const SegmentationColorLuts &luts = segmentation_color_luts();
        std::array<cv::Mat, 3> bgr_channels;
        cv::LUT(class_ids, luts.b, bgr_channels[0]);
        cv::LUT(class_ids, luts.g, bgr_channels[1]);
        cv::LUT(class_ids, luts.r, bgr_channels[2]);
        cv::merge(bgr_channels.data(), static_cast<int>(bgr_channels.size()), active_color_bgr);
    }
};

/* 每个显示/录像工作线程独享缓存，无跨通道锁。实时显示是一通道一线程，
 * 因而同一推理结果被多个解码帧复用时不会重复 resize、着色和扫描掩码。 */
thread_local std::array<std::vector<SegmentationOverlayCache>, MAX_CHANNEL_NUM + 1> g_segmentation_caches;

static void draw_segmentation_overlay(cv::Mat &screen_roi, const std::vector<AlgoResult> &results, int channel_id,
                                      int64_t result_frame_id)
{
    auto &caches =
        g_segmentation_caches[channel_id >= 0 && channel_id < MAX_CHANNEL_NUM ? channel_id : MAX_CHANNEL_NUM];
    const size_t mask_count = static_cast<size_t>(std::count_if(
        results.begin(), results.end(), [](const AlgoResult &result) { return !result.boxMask.empty(); }));
    caches.resize(mask_count);
    size_t index = 0;
    for (const auto &result : results)
    {
        if (result.boxMask.empty())
            continue;
        auto &cache = caches[index++];
        if (!cache.matches(result.boxMask, screen_roi.size(), result_frame_id))
            cache.prepare(result.boxMask, screen_roi.size(), result_frame_id);
        if (cache.active_rect.empty())
            continue;

        cv::Mat dst = screen_roi(cache.active_rect);
        cache.blend_scratch.create(cache.active_rect.size(), screen_roi.type());
        cv::addWeighted(dst, 0.5, cache.active_color_bgr, 0.5, 0.0, cache.blend_scratch);
        cache.blend_scratch.copyTo(dst, cache.active_mask);
    }
}

// Per-render-thread cache: colorization is paid once per new inference result,
// rather than once per display tick. Holding the source Mat pins its pool slot.
static bool draw_depth_background(cv::Mat &screen, const std::vector<AlgoResult> &results, int channel,
                                  int64_t frame_id)
{
    const AlgoResult *depth = nullptr;
    for (const auto &result : results)
        if (result.is_depth())
        {
            depth = &result;
            break;
        }
    if (!depth || depth->depth_map.type() != CV_32FC1)
        return false;
    struct Cache
    {
        cv::Mat source, color;
        cv::Size size;
        int64_t frame = -1;
        DepthKind kind = DepthKind::Metric;
    };
    static thread_local std::array<Cache, MAX_CHANNEL_NUM + 1> caches;
    Cache &cache = caches[channel >= 0 && channel < MAX_CHANNEL_NUM ? channel : MAX_CHANNEL_NUM];
    if (frame_id <= 0 || cache.source.data != depth->depth_map.data || cache.source.size() != depth->depth_map.size() ||
        cache.size != screen.size() || cache.frame != frame_id || cache.kind != depth->depth_kind)
    {
        cache.source = depth->depth_map;
        cache.frame = frame_id;
        cache.kind = depth->depth_kind;
        const auto proximity = [&](float value) {
            return cache.kind == DepthKind::RelativeInverse ? value : 1.0f / std::max(value, 1e-6f);
        };
        cache.size = screen.size();
        std::vector<float> samples;
        samples.reserve((cache.source.rows / 4 + 1) * (cache.source.cols / 4 + 1));
        for (int y = 0; y < cache.source.rows; y += 4)
            for (int x = 0; x < cache.source.cols; x += 4)
            {
                const float d = cache.source.at<float>(y, x);
                if (depth->valid_depth_value(d))
                    samples.push_back(proximity(d));
            }
        float lo = 0.0f, hi = 1.0f;
        if (!samples.empty())
        {
            std::sort(samples.begin(), samples.end());
            lo = samples[static_cast<size_t>((samples.size() - 1) * 0.02)];
            hi = samples[static_cast<size_t>((samples.size() - 1) * 0.98)];
        }
        const float scale = 255.0f / std::max(hi - lo, 1e-6f);
        cv::Mat gray = cv::Mat::zeros(cache.source.size(), CV_8UC1);
        cv::Mat valid = cv::Mat::zeros(cache.source.size(), CV_8UC1);
        for (int y = 0; y < cache.source.rows; ++y)
        {
            const float *input = cache.source.ptr<float>(y);
            auto *output = gray.ptr<uint8_t>(y);
            auto *mask = valid.ptr<uint8_t>(y);
            for (int x = 0; x < cache.source.cols; ++x)
                if (depth->valid_depth_value(input[x]))
                {
                    output[x] = cv::saturate_cast<uint8_t>((proximity(input[x]) - lo) * scale);
                    mask[x] = 255;
                }
        }
        cv::Mat color;
        cv::applyColorMap(gray, color, cv::COLORMAP_JET);
        cv::resize(color, cache.color, screen.size(), 0, 0, cv::INTER_LINEAR);
        cv::Mat resized_valid;
        cv::resize(valid, resized_valid, screen.size(), 0, 0, cv::INTER_NEAREST);
        cache.color.setTo(cv::Scalar(0, 0, 0), resized_valid == 0);
    }
    cache.color.copyTo(screen);
    return true;
}

static cv::Rect clipped_draw_bounds(const cv::Mat &image, int64_t left, int64_t top, int64_t right, int64_t bottom)
{
    left = std::max<int64_t>(0, std::min<int64_t>(image.cols, left));
    top = std::max<int64_t>(0, std::min<int64_t>(image.rows, top));
    right = std::max<int64_t>(0, std::min<int64_t>(image.cols, right));
    bottom = std::max<int64_t>(0, std::min<int64_t>(image.rows, bottom));
    if (right <= left || bottom <= top)
        return cv::Rect();
    return cv::Rect(static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left),
                    static_cast<int>(bottom - top));
}

template <typename DrawFn>
static void draw_alpha_region(cv::Mat &image, const cv::Rect &bounds, double alpha, DrawFn draw)
{
    const double a = std::max(0.0, std::min(1.0, alpha));
    if (a <= 0.0 || bounds.empty())
        return;

    cv::Mat dst = image(bounds);
    cv::Mat overlay = dst.clone();
    draw(overlay, bounds.tl());
    cv::addWeighted(overlay, a, dst, 1.0 - a, 0.0, dst);
}

static int draw_padding(int thickness, bool antialiased)
{
    const int stroke_padding = thickness > 0 ? (thickness + 1) / 2 : 0;
    return stroke_padding + (antialiased ? 2 : 1);
}

} // namespace

static void draw_pose_overlay(cv::Mat &screen_roi, const std::vector<AlgoResult> &results, float scale_x, float scale_y)
{
    for (const auto &res : results)
    {
        if (res.keypoints.empty())
            continue;

        const PoseSkeletonView skeleton = pose_skeleton_for(res.keypoints.size());
        for (size_t j = 0; j < skeleton.count; ++j)
        {
            const size_t idx1 = static_cast<size_t>(skeleton.edges[j].first);
            const size_t idx2 = static_cast<size_t>(skeleton.edges[j].second);
            if (!pose_keypoint_visible(res, idx1) || !pose_keypoint_visible(res, idx2))
                continue;

            const cv::Point2f &raw_p1 = res.keypoints[idx1];
            const cv::Point2f &raw_p2 = res.keypoints[idx2];
            cv::Point p1(static_cast<int>(raw_p1.x * scale_x), static_cast<int>(raw_p1.y * scale_y));
            cv::Point p2(static_cast<int>(raw_p2.x * scale_x), static_cast<int>(raw_p2.y * scale_y));
            cv::line(screen_roi, p1, p2, cv::Scalar(0, 165, 255), 2);
        }

        for (size_t j = 0; j < res.keypoints.size(); ++j)
        {
            if (!pose_keypoint_visible(res, j))
                continue;
            const cv::Point2f &raw_p = res.keypoints[j];
            cv::Point p(static_cast<int>(raw_p.x * scale_x), static_cast<int>(raw_p.y * scale_y));
            cv::circle(screen_roi, p, 3, cv::Scalar(0, 255, 255), -1);
        }
    }
}

void vision::render_overlays(cv::Mat &screen_roi, const vision::OverlayOptions &p,
                             vision::OverlayCache *performance_cache)
{
    // Select the base before ROI/boxes/text, so no later fill erases annotations.
    // A configured depth stream never exposes the source while waiting for data.
    const bool has_depth = p.results && draw_depth_background(screen_roi, *p.results, p.chnId, p.result_frame_id);
    if (p.depth_background && !has_depth)
        screen_roi.setTo(cv::Scalar(0, 0, 0));
    /* 推理 ROI 是纯输出叠加层：此函数只接收已经完成模型推理的显示画面，
     * 因而边界和文字不会进入模型，也不会污染业务逻辑读取的 source/model frame。 */
    if (p.show_system_overlays && p.inference_roi && p.inference_roi->has_roi())
    {
        const InferenceRoiConfig &roi = *p.inference_roi;
        std::vector<cv::Point> points;
        points.reserve(roi.polygon.size());
        for (const auto &point : roi.polygon)
            points.emplace_back(cvRound(point.first * screen_roi.cols), cvRound(point.second * screen_roi.rows));
        if (points.size() >= 3)
        {
            const double scale = (static_cast<double>(screen_roi.cols) + screen_roi.rows) / (640.0 + 640.0);
            const int thickness = std::max(2, cvRound(2.0 * scale));
            const cv::Scalar color(255, 255, 0); /* BGR cyan，与黄色业务 ROI 区分。 */
            cv::polylines(screen_roi, points, true, color, thickness, cv::LINE_AA);
            const cv::Point label_at(std::max(4, points.front().x + 5), std::max(18, points.front().y - 6));
            put_text_auto(screen_roi, "推理 ROI", label_at, std::max(0.45, 0.55 * scale), color, 1);
        }
    }

    // ROI 区域(可多个)：顶点均为业务坐标系(640×640)(同检测框)，统一按 inputW/inputH 缩放到当前窗口后逐个画。
    if (p.show_system_overlays && p.roi_zones && !p.roi_zones->empty() && p.inputW > 0 && p.inputH > 0)
    {
        const float sx = static_cast<float>(screen_roi.cols) / static_cast<float>(p.inputW);
        const float sy = static_cast<float>(screen_roi.rows) / static_cast<float>(p.inputH);
        const int thick = std::max(1, cvRound(2.0 * (sx + sy) * 0.5));
        for (const auto &zone : *p.roi_zones)
        {
            if (zone.polygon.size() < 3)
                continue;
            std::vector<cv::Point> roi;
            roi.reserve(zone.polygon.size());
            for (const auto &pt : zone.polygon)
                roi.emplace_back(static_cast<int>(pt.x * sx), static_cast<int>(pt.y * sy));
            cv::polylines(screen_roi, roi, true, cv::Scalar(0, 255, 255), thick);
        }
    }

    // 检测框 / draw_cmds 使用业务坐标系(640×640)（inputW×inputH）
    const float scale_x = static_cast<float>(screen_roi.cols) / static_cast<float>(p.inputW);
    const float scale_y = static_cast<float>(screen_roi.rows) / static_cast<float>(p.inputH);

    /* 线宽/字号/半径等"权重"必须随窗口一起缩放：否则窗口缩小时线相对变粗(填满)、
     * 窗口放大时线相对变细(露空隙)——因为之前只缩放了坐标、没缩放粗细。
     * 这里按位置同一比例缩放粗细，效果等同"在模型分辨率上绘制再整体缩放"，
     * 又不打乱 RGA 把原始帧直缩到 tile 的高效路径。 */
    const double draw_scale = (static_cast<double>(scale_x) + static_cast<double>(scale_y)) * 0.5;
    auto thk = [draw_scale](int t) { return t < 0 ? t : std::max(1, cvRound(t * draw_scale)); };

    // Segmentation mask
    if (p.show_system_overlays && p.results && !p.results->empty())
        draw_segmentation_overlay(screen_roi, *p.results, p.chnId, p.result_frame_id);

    // Detections
    if (p.show_system_overlays && p.results && !p.results->empty())
    {
        for (const auto &res : *p.results)
        {
            if (res.is_depth() || !res.show_detection_box)
                continue;
            cv::Rect box;
            box.x = static_cast<int>(res.box.x * scale_x);
            box.y = static_cast<int>(res.box.y * scale_y);
            box.width = static_cast<int>(res.box.width * scale_x);
            box.height = static_cast<int>(res.box.height * scale_y);

            box.x = std::max(0, box.x);
            box.y = std::max(0, box.y);
            if (box.x + box.width > screen_roi.cols)
                box.width = screen_roi.cols - box.x;
            if (box.y + box.height > screen_roi.rows)
                box.height = screen_roi.rows - box.y;
            if (box.width <= 0 || box.height <= 0)
                continue;

            const cv::Scalar color = (res.box_color[0] >= 0) ? res.box_color : cv::Scalar(0, 255, 0);
            // 检测框至少保留 2 像素，较大的输出画面继续按窗口比例放大。
            const int detection_thickness = std::max(2, thk(2));
            if (res.has_obb)
            {
                for (int i = 0; i < 4; ++i)
                {
                    const auto &a = res.obb_corners[i], &b = res.obb_corners[(i + 1) % 4];
                    cv::line(screen_roi, cv::Point(cvRound(a.x * scale_x), cvRound(a.y * scale_y)),
                             cv::Point(cvRound(b.x * scale_x), cvRound(b.y * scale_y)), color, detection_thickness);
                }
            }
            else
                cv::rectangle(screen_roi, box, color, detection_thickness);
            // 默认在类别名后显示置信度，固定保留两位小数。
            std::string txt = res.label + " " + cv::format("%.2f", res.score);
            if (res.track_id >= 0)
                txt = "ID " + std::to_string(res.track_id) + " " + txt;
            put_text_auto(screen_roi, txt, cv::Point(box.x, std::max(20, box.y - 5)), std::max(0.3, 0.6 * draw_scale),
                          color, 1);
        }
        draw_pose_overlay(screen_roi, *p.results, scale_x, scale_y);
    }

    // 自定义绘制指令（坐标系：inputW×inputH）
    // 只渲染 target 与 target_mask 有交集的指令
    if (p.show_custom_overlays && p.draw_cmds)
    {
        for (const auto &cmd : *p.draw_cmds)
        {
            if (!(cmd.target & p.target_mask))
                continue;
            switch (cmd.type)
            {
            case DrawCommand::RECT: {
                cv::Rect r(static_cast<int>(cmd.rect.x * scale_x), static_cast<int>(cmd.rect.y * scale_y),
                           static_cast<int>(cmd.rect.width * scale_x), static_cast<int>(cmd.rect.height * scale_y));

                // 边界保护：防止坐标溢出或出现负数宽高等导致 OpenCV crash
                r.x = std::max(0, r.x);
                r.y = std::max(0, r.y);
                if (r.x + r.width > screen_roi.cols)
                    r.width = screen_roi.cols - r.x;
                if (r.y + r.height > screen_roi.rows)
                    r.height = screen_roi.rows - r.y;

                if (r.width > 0 && r.height > 0)
                {
                    if (cmd.alpha < 0.999)
                    {
                        const int thickness = thk(cmd.thickness);
                        const int pad = draw_padding(thickness, false);
                        const cv::Rect bounds = clipped_draw_bounds(
                            screen_roi, static_cast<int64_t>(r.x) - pad, static_cast<int64_t>(r.y) - pad,
                            static_cast<int64_t>(r.x) + r.width + pad, static_cast<int64_t>(r.y) + r.height + pad);
                        draw_alpha_region(
                            screen_roi, bounds, cmd.alpha, [&](cv::Mat &overlay, const cv::Point &origin) {
                                cv::rectangle(overlay, cv::Rect(r.tl() - origin, r.size()), cmd.color, thickness);
                            });
                    }
                    else
                        cv::rectangle(screen_roi, r, cmd.color, thk(cmd.thickness));
                }
                break;
            }
            case DrawCommand::CIRCLE: {
                /* 当 scale_x != scale_y（非均匀缩放）时，用椭圆代替正圆，
                 * 使屏幕上显示的边界与逻辑判断的欧氏距离边界完全对应。
                 * 均匀缩放时退化为正圆，行为与原来完全一致。 */
                const cv::Point ec(static_cast<int>(cmd.center.x * scale_x), static_cast<int>(cmd.center.y * scale_y));
                const cv::Size es(static_cast<int>(cmd.radius * scale_x), static_cast<int>(cmd.radius * scale_y));
                if (es.width < 0 || es.height < 0)
                    break;
                if (cmd.alpha < 0.999)
                {
                    const int thickness = thk(cmd.thickness);
                    const int pad = draw_padding(thickness, true);
                    const cv::Rect bounds = clipped_draw_bounds(screen_roi, static_cast<int64_t>(ec.x) - es.width - pad,
                                                                static_cast<int64_t>(ec.y) - es.height - pad,
                                                                static_cast<int64_t>(ec.x) + es.width + pad + 1,
                                                                static_cast<int64_t>(ec.y) + es.height + pad + 1);
                    draw_alpha_region(screen_roi, bounds, cmd.alpha, [&](cv::Mat &overlay, const cv::Point &origin) {
                        cv::ellipse(overlay, ec - origin, es, 0, 0, 360, cmd.color, thickness);
                    });
                }
                else
                    cv::ellipse(screen_roi, ec, es, 0, 0, 360, cmd.color, thk(cmd.thickness));
                break;
            }
            case DrawCommand::LINE:
                cv::line(screen_roi,
                         cv::Point(static_cast<int>(cmd.pt1.x * scale_x), static_cast<int>(cmd.pt1.y * scale_y)),
                         cv::Point(static_cast<int>(cmd.pt2.x * scale_x), static_cast<int>(cmd.pt2.y * scale_y)),
                         cmd.color, thk(cmd.thickness));
                break;
            case DrawCommand::POLYLINE: {
                if (cmd.points.size() < 2)
                    break;
                std::vector<cv::Point> pts;
                pts.reserve(cmd.points.size());
                for (const auto &q : cmd.points)
                    pts.emplace_back(static_cast<int>(q.x * scale_x), static_cast<int>(q.y * scale_y));
                if (cmd.alpha < 0.999)
                {
                    /* 半透明：折线画到副本再整体融合；自交叠处只画一次，不会越叠越暗。
                     * 只复制轨迹包围区域，避免每条轨迹都 clone + blend 整张 tile。 */
                    const int thickness = thk(cmd.thickness);
                    const int pad = draw_padding(thickness, true);
                    const cv::Rect point_bounds = cv::boundingRect(pts);
                    const cv::Rect bounds =
                        clipped_draw_bounds(screen_roi, static_cast<int64_t>(point_bounds.x) - pad,
                                            static_cast<int64_t>(point_bounds.y) - pad,
                                            static_cast<int64_t>(point_bounds.x) + point_bounds.width + pad,
                                            static_cast<int64_t>(point_bounds.y) + point_bounds.height + pad);
                    draw_alpha_region(screen_roi, bounds, cmd.alpha, [&](cv::Mat &overlay, const cv::Point &origin) {
                        std::vector<cv::Point> local_pts;
                        local_pts.reserve(pts.size());
                        for (const cv::Point &point : pts)
                            local_pts.push_back(point - origin);
                        cv::polylines(overlay, local_pts, cmd.closed, cmd.color, thickness, cv::LINE_AA);
                    });
                }
                else
                {
                    cv::polylines(screen_roi, pts, cmd.closed, cmd.color, thk(cmd.thickness), cv::LINE_AA);
                }
                break;
            }
            case DrawCommand::POLY_FILLED: {
                if (cmd.points.size() < 3)
                    break;
                std::vector<cv::Point> pts;
                pts.reserve(cmd.points.size());
                for (const auto &q : cmd.points)
                    pts.emplace_back(static_cast<int>(q.x * scale_x), static_cast<int>(q.y * scale_y));
                if (cmd.alpha < 0.999)
                {
                    const int pad = draw_padding(/*filled*/ -1, true);
                    const cv::Rect point_bounds = cv::boundingRect(pts);
                    const cv::Rect bounds =
                        clipped_draw_bounds(screen_roi, static_cast<int64_t>(point_bounds.x) - pad,
                                            static_cast<int64_t>(point_bounds.y) - pad,
                                            static_cast<int64_t>(point_bounds.x) + point_bounds.width + pad,
                                            static_cast<int64_t>(point_bounds.y) + point_bounds.height + pad);
                    draw_alpha_region(screen_roi, bounds, cmd.alpha, [&](cv::Mat &overlay, const cv::Point &origin) {
                        cv::fillPoly(overlay, std::vector<std::vector<cv::Point>>{pts}, cmd.color, cv::LINE_AA, 0,
                                     cv::Point(-origin.x, -origin.y));
                    });
                }
                else
                    cv::fillPoly(screen_roi, std::vector<std::vector<cv::Point>>{pts}, cmd.color, cv::LINE_AA);
                break;
            }
            case DrawCommand::TEXT: {
                // 自适应文字大小：基于画面缩放比例调整，同时限制一个最小可读字号
                double adapted_font_scale = cmd.font_scale * ((scale_x + scale_y) * 0.5f);
                adapted_font_scale = std::max(0.3, adapted_font_scale);

                cv::Point text_pos(static_cast<int>(cmd.text_pos.x * scale_x),
                                   static_cast<int>(cmd.text_pos.y * scale_y));
                if (cmd.text_align_right)
                {
                    const int font_height = std::max(12, static_cast<int>(std::lround(adapted_font_scale * 30.0)));
                    cv::Size text_size;
                    int baseline = 0;
                    if (measure_text_unicode(cmd.text, font_height, /*filled*/ -1, text_size, baseline))
                        text_pos.x -= text_size.width;
                }
                const int shadow_width =
                    std::max(1, static_cast<int>(std::lround(cmd.text_shadow_width * (scale_x + scale_y) * 0.5f)));
                put_text_auto(screen_roi, cmd.text, text_pos, adapted_font_scale, cmd.color, cmd.thickness,
                              cmd.text_shadow_enabled, cmd.text_shadow_color, shadow_width);
                break;
            }
            }
        }
    }

    vision::render_performance_overlay(screen_roi, p, performance_cache);
}

static double performance_overlay_scale(const cv::Mat &view, const vision::OverlayOptions &options)
{
    if (options.fps_font_height > 0)
        return options.fps_font_height / 17.4;
    // 默认四分屏的单格为 960×540；字号和边距跟随实际画面一起缩放。
    return std::min(view.cols / 960.0, view.rows / 540.0);
}

cv::Rect vision::performance_overlay_bounds(const cv::Mat &view, const vision::OverlayOptions &options)
{
    // 缓存区域与文字同步缩放，停帧刷新也能完整擦除旧字；小画面保留像素取整余量。
    const int height = static_cast<int>(std::min(
        static_cast<double>(view.rows), std::max(4.0, std::ceil(64 * performance_overlay_scale(view, options)))));
    return cv::Rect(0, view.rows - height, view.cols, height);
}

void vision::render_performance_overlay(cv::Mat &screen_roi, const vision::OverlayOptions &p,
                                        vision::OverlayCache *cache)
{
    if (p.show_fps)
    {
        char fps_text[128];
        snprintf(fps_text, sizeof(fps_text), "Ch%d render %.1f / infer %.1f FPS", p.chnId, p.disp_fps, p.infer_fps);
        if (p.show_network_rtt)
        {
            const size_t used = std::char_traits<char>::length(fps_text);
            if (std::isfinite(p.network_rtt_ms) && p.network_rtt_ms >= 0)
                snprintf(fps_text + used, sizeof(fps_text) - used, " | 板端—摄像头往返 %.1f 毫秒", p.network_rtt_ms);
            else
                snprintf(fps_text + used, sizeof(fps_text) - used, " | 板端—摄像头往返 --");
        }
        if (cache)
        {
            screen_roi(vision::performance_overlay_bounds(screen_roi, p)).copyTo(cache->background);
            cache->render_fps = p.disp_fps;
            cache->infer_fps = p.infer_fps;
            cache->visible = true;
            cache->show_network_rtt = p.show_network_rtt;
            cache->network_rtt_ms = p.network_rtt_ms;
        }
        const double scale = performance_overlay_scale(screen_roi, p);
        const int margin = std::max(1, static_cast<int>(std::lround(10 * scale)));
        const int font_height = std::max(1, static_cast<int>(std::lround(0.58 * 30.0 * scale)));
        cv::Size text_size;
        int baseline = 0;
        cv::Point text_org(std::max(margin, screen_roi.cols - margin - cvRound(240 * scale)), screen_roi.rows - margin);
        if (measure_text_unicode(fps_text, font_height, /*filled*/ -1, text_size, baseline))
        {
            /* FreeType 的 org 是文字基线点；基线下方仍有 baseline 像素。
             * 同时扣除 baseline 才能保证文字最底部与 tile 下边保持 margin。 */
            text_org.x = std::max(margin, screen_roi.cols - margin - text_size.width);
            text_org.y = std::max(text_size.height + margin, screen_roi.rows - margin - baseline);
        }
        // 帧率文字允许随小窗口继续缩小，不套用其他业务文字的 12px 最小字号。
        if (!draw_text_unicode_cached(screen_roi, fps_text, text_org, font_height, cv::Scalar(255, 0, 0), 1, false,
                                      cv::Scalar(), 0))
            draw_text_unicode(screen_roi, fps_text, text_org, font_height, cv::Scalar(255, 0, 0), -1);
    }
    else if (cache)
    {
        cache->background.release();
        cache->visible = false;
    }
}
