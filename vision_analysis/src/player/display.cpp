#include "display.h"
#include "../analyzer/algoProcess.h"
#include "../core/app_ctrl.h"
#include "../core/pause_ctrl.h"
#include "../core/process_signals.h"
#include "system.h"
#include "text_overlay.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <gtk/gtk.h>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

/* 文本绘制出口: 中英文统一用 freetype 渲染(不再回退 Hershey)。font_scale 沿用 putText 语义
 * 按比例换算成像素高; freetype 不可用时不绘制(已在字体加载处报错)。
 * thickness = 加粗级别(对外即 draw_text 的"粗细"): <=1 普通填充字(默认外观, 与历史一致);
 *   >=2 在填充字基础上再叠一层同色描边把笔画加粗, 数值越大越粗(上限封顶, 防糊成一团)。
 * 始终先填充, 保证是实心清晰字; 不会因 thickness>0 变成空心描边字。 */
static inline void put_text_auto(cv::Mat &img, const std::string &s, cv::Point org, double font_scale,
                                 const cv::Scalar &color, int thickness)
{
    const int fh = std::max(12, (int)std::lround(font_scale * 30.0));
    draw_text_unicode(img, s, org, fh, color, /*filled*/ -1);
    if (thickness >= 2)
        draw_text_unicode(img, s, org, fh, color, std::min(thickness - 1, 6));
}

/*======================== 显示状态 (模块级单例) ========================*/
struct DisplayState
{
    std::mutex mutex;
    char *buffer{nullptr};
    char *front{nullptr}; /* 当前可见的前缓冲区 */
    Display_t *desc{nullptr};
};
static DisplayState g_disp;

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
            {255, 56, 56},  {255, 157, 151}, {255, 112, 31},  {255, 178, 29}, {207, 210, 49},
            {72, 249, 10},  {146, 204, 23}, {61, 219, 134},  {26, 147, 52},  {0, 212, 187},
            {44, 153, 168}, {0, 194, 255},  {52, 69, 147},  {100, 115, 255}, {0, 24, 236},
            {132, 56, 255}, {82, 0, 133},   {203, 56, 255}, {255, 149, 200}, {255, 55, 199}};

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

struct SegmentationInstanceOverlay
{
    cv::Rect screen_box;
    cv::Mat mask;
    cv::Mat color_bgr;
    float vx = 0.0f;
    float vy = 0.0f;
    int track_id = -1;
    int track_hits = 0;
};

struct SegmentationOverlayCache
{
    int64_t result_frame_id = -1;
    cv::Size output_size;
    std::vector<SegmentationInstanceOverlay> instances;
    cv::Mat blend_scratch;

    bool matches(const cv::Size &size, int64_t frame_id) const
    {
        return frame_id > 0 && result_frame_id == frame_id && output_size == size;
    }

    void prepare(const std::vector<AlgoResult> &results, const cv::Size &size, int64_t frame_id, float scale_x,
                 float scale_y)
    {
        result_frame_id = frame_id;
        output_size = size;
        instances.clear();
        instances.reserve(results.size());

        for (const auto &res : results)
        {
            if (res.boxMask.empty() || res.boxMask.type() != CV_8UC1 || res.box.width <= 0 || res.box.height <= 0)
                continue;

            const int dst_w = std::max(1, cvRound(res.box.width * scale_x));
            const int dst_h = std::max(1, cvRound(res.box.height * scale_y));
            SegmentationInstanceOverlay item;
            item.screen_box = cv::Rect(cvRound(res.box.x * scale_x), cvRound(res.box.y * scale_y), dst_w, dst_h);
            item.vx = res.vx;
            item.vy = res.vy;
            item.track_id = res.track_id;
            item.track_hits = res.track_hits;

            cv::Mat class_ids;
            cv::resize(res.boxMask, class_ids, cv::Size(dst_w, dst_h), 0, 0, cv::INTER_NEAREST);
            cv::compare(class_ids, 0, item.mask, cv::CMP_NE);
            if (cv::countNonZero(item.mask) == 0)
                continue;

            const SegmentationColorLuts &luts = segmentation_color_luts();
            std::array<cv::Mat, 3> bgr_channels;
            cv::LUT(class_ids, luts.b, bgr_channels[0]);
            cv::LUT(class_ids, luts.g, bgr_channels[1]);
            cv::LUT(class_ids, luts.r, bgr_channels[2]);
            cv::merge(bgr_channels.data(), static_cast<int>(bgr_channels.size()), item.color_bgr);
            instances.push_back(std::move(item));
        }
    }
};

/* 每个显示/录像工作线程独享缓存，无跨通道锁。实时显示是一通道一线程，
 * 因而同一推理结果被多个解码帧复用时不会重复 resize、着色和扫描掩码。 */
thread_local std::array<SegmentationOverlayCache, MAX_CHANNEL_NUM> g_segmentation_caches;
thread_local SegmentationOverlayCache g_fallback_segmentation_cache;

static SegmentationOverlayCache &segmentation_cache_for(int channel_id)
{
    if (channel_id >= 0 && channel_id < MAX_CHANNEL_NUM)
        return g_segmentation_caches[static_cast<size_t>(channel_id)];
    return g_fallback_segmentation_cache;
}

static void draw_segmentation_overlay(cv::Mat &screen_roi, const std::vector<AlgoResult> &results, int channel_id,
                                      int64_t result_frame_id, float scale_x, float scale_y, float frames_elapsed)
{
    SegmentationOverlayCache &cache = segmentation_cache_for(channel_id);
    if (!cache.matches(screen_roi.size(), result_frame_id))
        cache.prepare(results, screen_roi.size(), result_frame_id, scale_x, scale_y);

    const cv::Rect screen_bounds(0, 0, screen_roi.cols, screen_roi.rows);
    for (const auto &item : cache.instances)
    {
        cv::Rect predicted = item.screen_box;
        if (frames_elapsed > 0.0f && item.track_id >= 0 && item.track_hits >= 3)
        {
            predicted.x += cvRound(item.vx * scale_x * frames_elapsed);
            predicted.y += cvRound(item.vy * scale_y * frames_elapsed);
        }

        const cv::Rect clipped = predicted & screen_bounds;
        if (clipped.empty())
            continue;
        const cv::Rect source_roi(clipped.x - predicted.x, clipped.y - predicted.y, clipped.width, clipped.height);
        cv::Mat dst = screen_roi(clipped);
        cache.blend_scratch.create(clipped.size(), screen_roi.type());
        cv::addWeighted(dst, 0.5, item.color_bgr(source_roi), 0.5, 0.0, cache.blend_scratch);
        cache.blend_scratch.copyTo(dst, item.mask(source_roi));
    }
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

static void draw_pose_overlay(cv::Mat &screen_roi, const std::vector<AlgoResult> &results, float scale_x, float scale_y,
                              float frames_elapsed)
{
    for (const auto &res : results)
    {
        if (res.keypoints.empty())
            continue;

        float shift_x = 0.0f;
        float shift_y = 0.0f;
        if (frames_elapsed > 0.0f && res.track_id >= 0 && res.track_hits >= 3)
        {
            shift_x = res.vx * frames_elapsed;
            shift_y = res.vy * frames_elapsed;
        }

        const PoseSkeletonView skeleton = pose_skeleton_for(res.keypoints.size());
        for (size_t j = 0; j < skeleton.count; ++j)
        {
            const size_t idx1 = static_cast<size_t>(skeleton.edges[j].first);
            const size_t idx2 = static_cast<size_t>(skeleton.edges[j].second);
            if (!pose_keypoint_visible(res, idx1) || !pose_keypoint_visible(res, idx2))
                continue;

            const cv::Point2f &raw_p1 = res.keypoints[idx1];
            const cv::Point2f &raw_p2 = res.keypoints[idx2];
            cv::Point p1(static_cast<int>((raw_p1.x + shift_x) * scale_x),
                         static_cast<int>((raw_p1.y + shift_y) * scale_y));
            cv::Point p2(static_cast<int>((raw_p2.x + shift_x) * scale_x),
                         static_cast<int>((raw_p2.y + shift_y) * scale_y));
            cv::line(screen_roi, p1, p2, cv::Scalar(0, 165, 255), 2);
        }

        for (size_t j = 0; j < res.keypoints.size(); ++j)
        {
            if (!pose_keypoint_visible(res, j))
                continue;
            const cv::Point2f &raw_p = res.keypoints[j];
            cv::Point p(static_cast<int>((raw_p.x + shift_x) * scale_x),
                        static_cast<int>((raw_p.y + shift_y) * scale_y));
            cv::circle(screen_roi, p, 3, cv::Scalar(0, 255, 255), -1);
        }
    }
}

void render_overlays(cv::Mat &screen_roi, const RenderParams &p)
{
    // ROI 区域(可多个)：顶点均为模型输入坐标系(同检测框)，统一按 inputW/inputH 缩放到当前窗口后逐个画。
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

    // 检测框 / draw_cmds 使用模型输入坐标系（inputW×inputH）
    const float scale_x = static_cast<float>(screen_roi.cols) / static_cast<float>(p.inputW);
    const float scale_y = static_cast<float>(screen_roi.rows) / static_cast<float>(p.inputH);

    /* 线宽/字号/半径等"权重"必须随窗口一起缩放：否则窗口缩小时线相对变粗(填满)、
     * 窗口放大时线相对变细(露空隙)——因为之前只缩放了坐标、没缩放粗细。
     * 这里按位置同一比例缩放粗细，效果等同"在模型分辨率上绘制再整体缩放"，
     * 又不打乱 RGA 把原始帧直缩到 tile 的高效路径。 */
    const double draw_scale = (static_cast<double>(scale_x) + static_cast<double>(scale_y)) * 0.5;
    auto thk = [draw_scale](int t) { return t < 0 ? t : std::max(1, cvRound(t * draw_scale)); };

    /* 当前显示帧相对结果源帧经过了多少个推理周期。检测框、姿态和逐目标掩码
     * 共用同一个时间基准，避免三种结果各自表现出不同程度的视觉滞后。 */
    float frames_elapsed = 0.0f;
    if (p.result_age_ms > 0 && p.infer_fps > 0.5f)
    {
        frames_elapsed = static_cast<float>(p.result_age_ms) / 1000.0f * p.infer_fps;
        frames_elapsed = std::min(frames_elapsed, 2.0f);
    }

    // Segmentation mask
    if (p.show_system_overlays && p.results && !p.results->empty())
        draw_segmentation_overlay(screen_roi, *p.results, p.chnId, p.result_frame_id, scale_x, scale_y,
                                  frames_elapsed);

    // Detections
    if (p.show_system_overlays && p.results && !p.results->empty())
    {
        /* 速度外推参数：补偿从 NPU 推理完成到当前帧显示之间的管线延迟。
         *
         * 原理：result_age_ms = 当前帧距上次 NPU 推理结果的毫秒数（即管线延迟）。
         * infer_fps = 推理帧率，用于把 Kalman 速度（像素/推理帧）换算成 像素/秒。
         * frames_elapsed = result_age_ms × infer_fps / 1000 ≈ 经过了几个推理帧间隔。
         *
         * 安全限制：
         *  - 只对 confirmed 轨迹 (track_id >= 0, track_hits >= 3) 外推，速度可信
         *  - frames_elapsed 上限 2.0f，避免网络抖动时 result_age 突然很大导致框飞走
         *  - 外推后对框做边界裁剪
         */
        for (const auto &res : *p.results)
        {
            float fx = res.box.x * scale_x;
            float fy = res.box.y * scale_y;
            float fw = res.box.width * scale_x;
            float fh = res.box.height * scale_y;

            if (frames_elapsed > 0.0f && res.track_id >= 0 && res.track_hits >= 3)
            {
                fx += res.vx * scale_x * frames_elapsed;
                fy += res.vy * scale_y * frames_elapsed;
            }

            cv::Rect box;
            box.x = static_cast<int>(fx);
            box.y = static_cast<int>(fy);
            box.width = static_cast<int>(fw);
            box.height = static_cast<int>(fh);

            box.x = std::max(0, box.x);
            box.y = std::max(0, box.y);
            if (box.x + box.width > screen_roi.cols)
                box.width = screen_roi.cols - box.x;
            if (box.y + box.height > screen_roi.rows)
                box.height = screen_roi.rows - box.y;
            if (box.width <= 0 || box.height <= 0)
                continue;

            const cv::Scalar color = (res.box_color[0] >= 0) ? res.box_color : cv::Scalar(0, 255, 0);
            /* 检测框随后会转为 NV12 4:2:0 并进行 H264 编码。过细的高饱和度线条落在
             * 不同色度采样网格上时，某些边会只保留亮度而呈灰色；至少3像素可让四条边
             * 都覆盖完整色度样本，同时仍按窗口比例继续放大。 */
            const int detection_thickness = std::max(3, thk(2));
            cv::rectangle(screen_roi, box, color, detection_thickness);
            std::string txt = res.label;
            if (res.track_id >= 0)
                txt = "ID " + std::to_string(res.track_id) + " " + txt;
            put_text_auto(screen_roi, txt, cv::Point(box.x, std::max(20, box.y - 5)), std::max(0.3, 0.6 * draw_scale),
                          color, 1);
        }
        draw_pose_overlay(screen_roi, *p.results, scale_x, scale_y, frames_elapsed);
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
                        draw_alpha_region(screen_roi, bounds, cmd.alpha,
                                          [&](cv::Mat &overlay, const cv::Point &origin) {
                                              cv::rectangle(overlay, cv::Rect(r.tl() - origin, r.size()), cmd.color,
                                                            thickness);
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
                    const cv::Rect bounds = clipped_draw_bounds(
                        screen_roi, static_cast<int64_t>(ec.x) - es.width - pad,
                        static_cast<int64_t>(ec.y) - es.height - pad,
                        static_cast<int64_t>(ec.x) + es.width + pad + 1,
                        static_cast<int64_t>(ec.y) + es.height + pad + 1);
                    draw_alpha_region(screen_roi, bounds, cmd.alpha,
                                      [&](cv::Mat &overlay, const cv::Point &origin) {
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
                    const cv::Rect bounds = clipped_draw_bounds(
                        screen_roi, static_cast<int64_t>(point_bounds.x) - pad,
                        static_cast<int64_t>(point_bounds.y) - pad,
                        static_cast<int64_t>(point_bounds.x) + point_bounds.width + pad,
                        static_cast<int64_t>(point_bounds.y) + point_bounds.height + pad);
                    draw_alpha_region(screen_roi, bounds, cmd.alpha,
                                      [&](cv::Mat &overlay, const cv::Point &origin) {
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
                    const cv::Rect bounds = clipped_draw_bounds(
                        screen_roi, static_cast<int64_t>(point_bounds.x) - pad,
                        static_cast<int64_t>(point_bounds.y) - pad,
                        static_cast<int64_t>(point_bounds.x) + point_bounds.width + pad,
                        static_cast<int64_t>(point_bounds.y) + point_bounds.height + pad);
                    draw_alpha_region(screen_roi, bounds, cmd.alpha,
                                      [&](cv::Mat &overlay, const cv::Point &origin) {
                                          cv::fillPoly(overlay, std::vector<std::vector<cv::Point>>{pts}, cmd.color,
                                                       cv::LINE_AA, 0, cv::Point(-origin.x, -origin.y));
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

                put_text_auto(
                    screen_roi, cmd.text,
                    cv::Point(static_cast<int>(cmd.text_pos.x * scale_x), static_cast<int>(cmd.text_pos.y * scale_y)),
                    adapted_font_scale, cmd.color, cmd.thickness);
                break;
            }
            }
        }
    }

    // FPS overlay
    if (app_ctrl_get_performance_display() && p.show_fps)
    {
        char fps_text[80];
        if (p.disp_fps > 0.0f)
            snprintf(fps_text, sizeof(fps_text), "Ch%d preview %.1f / infer %.1f FPS", p.chnId, p.disp_fps,
                     p.infer_fps);
        else
            snprintf(fps_text, sizeof(fps_text), "Ch%d preview -- / infer %.1f FPS", p.chnId, p.infer_fps);
        constexpr double font_scale = 0.58;
        constexpr int margin = 10;
        const int font_height = std::max(12, static_cast<int>(std::lround(font_scale * 30.0)));
        cv::Size text_size;
        int baseline = 0;
        cv::Point text_org(std::max(margin, screen_roi.cols - margin - 240), screen_roi.rows - margin);
        if (measure_text_unicode(fps_text, font_height, /*filled*/ -1, text_size, baseline))
        {
            /* FreeType 的 org 是文字基线点；基线下方仍有 baseline 像素。
             * 同时扣除 baseline 才能保证文字最底部与 tile 下边保持 margin。 */
            text_org.x = std::max(margin, screen_roi.cols - margin - text_size.width);
            text_org.y = std::max(text_size.height + margin, screen_roi.rows - margin - baseline);
        }
        put_text_auto(screen_roi, fps_text, text_org, font_scale, cv::Scalar(255, 0, 0), 1);
    }
}

void render_channel_view(cv::Mat &bgr, int chn_id, uint64_t frame_timestamp_ms)
{
    if (bgr.empty() || !app_ctrl_has_channel(chn_id))
        return;

    auto runtime = app_ctrl_get_runtime_snapshot();
    const std::vector<RoiZone> *rois = app_ctrl_runtime_channel_rois(runtime, chn_id);
    if (!rois)
        return;
    std::vector<AlgoResult> results;
    std::vector<DrawCommand> commands;
    cv::Mat logic_frame;
    float disp_fps = 0.0f;
    int64_t result_frame_id = 0;
    uint64_t result_ts_ms = 0;
    uint64_t logic_ts_ms = 0;

    pthread_mutex_lock(&g_pCtrl->chn_mtx[chn_id]);
    const ChannelState &state = g_pCtrl->channels_state[chn_id];
    results = state.last_results;
    commands = state.draw_cmds;
    logic_frame = state.logic_display_frame;
    disp_fps = state.disp_fps;
    result_frame_id = state.published_frame_seq;
    /* 必须使用产生结果的源视频帧时间，而不是结果提交完成时间。
     * 后者会把排队、预处理和 NPU 耗时全部误算成零。 */
    result_ts_ms = state.frame_steady_ms;
    logic_ts_ms = state.logic_display_ts_ms;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chn_id]);

    uint64_t current_ms = frame_timestamp_ms != 0
                              ? frame_timestamp_ms
                              : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now().time_since_epoch())
                                                          .count());

    if (!logic_frame.empty() && current_ms >= logic_ts_ms && current_ms - logic_ts_ms < 1000)
    {
        cv::resize(logic_frame, bgr, bgr.size());
        /* Logic 显示底图来自本次业务结果帧，不再是最新解码帧；此时不应把结果
         * 再向前预测到被替换掉的实时画面时间。 */
        if (result_ts_ms != 0)
            current_ms = result_ts_ms;
    }

    RenderParams params;
    params.chnId = chn_id;
    params.inputW = g_pCtrl->inputW;
    params.inputH = g_pCtrl->inputH;
    if (params.inputW <= 0 || params.inputH <= 0)
        return;
    params.disp_fps = disp_fps;
    params.infer_fps = algorithm_get_infer_fps(chn_id);
    params.result_frame_id = result_frame_id;
    params.result_age_ms = result_ts_ms != 0 && current_ms >= result_ts_ms
                               ? static_cast<int64_t>(current_ms - result_ts_ms)
                               : 0;
    params.target_mask = DrawCommand::DISPLAY;
    params.show_system_overlays = true;
    params.show_custom_overlays = true;
    params.roi_zones = rois;
    params.results = &results;
    params.draw_cmds = &commands;
    render_overlays(bgr, params);
}

char **dispBufferMap(Display_t *dispDesc)
{
    static Display_t stDispDesc = {0};

    if (!g_disp.front || (dispDesc->width != stDispDesc.width) || (dispDesc->height != stDispDesc.height))
    {
        if (g_disp.buffer)
        {
            free(g_disp.buffer);
            g_disp.buffer = nullptr;
        }
        memcpy(&stDispDesc, dispDesc, sizeof(Display_t));
        size_t sz = 3 * dispDesc->width * dispDesc->height;
        g_disp.buffer = (char *)malloc(sz);
        if (g_disp.buffer)
            memset(g_disp.buffer, 0, sz);
        g_disp.front = g_disp.buffer;
    }

    return &g_disp.front;
}

void dispBufferUnmap(void)
{
    std::lock_guard<std::mutex> lock(g_disp.mutex);
    free(g_disp.buffer);
    g_disp.buffer = nullptr;
    g_disp.front = nullptr;
    g_disp.desc = nullptr;
}

static void display_lock_impl()
{
    g_disp.mutex.lock();
}
static void display_unlock_impl()
{
    g_disp.mutex.unlock();
}

void display_lock()
{
    display_lock_impl();
}
void display_unlock()
{
    display_unlock_impl();
}
bool display_try_lock()
{
    return g_disp.mutex.try_lock();
}

static gboolean showWidget(GtkWidget *pImage)
{
    if (g_pause_toggle_signal)
    {
        g_pause_toggle_signal = 0;
        pause_ctrl::toggle();
    }
    if (g_shutdown_signal)
        app_ctrl_request_stop();

    /* Ctrl+C / SIGTERM 退出检测 */
    if (g_pCtrl && !g_pCtrl->isRunning)
    {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    /* Render from front buffer, which always contains the latest completely written frame */
    char *pBuf = g_disp.front;
    if (NULL == pBuf)
    {
        return G_SOURCE_CONTINUE;
    }

    display_lock();
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data((guchar *)pBuf, GDK_COLORSPACE_RGB, FALSE, 8, g_disp.desc->width,
                                                 g_disp.desc->height, 3 * g_disp.desc->width, NULL, NULL);
    gtk_image_set_from_pixbuf(GTK_IMAGE(pImage), pixbuf);
    g_object_unref(pixbuf);
    display_unlock();

    return G_SOURCE_CONTINUE;
}
/*======================== 暂停键: GTK 键盘事件处理 ========================*/
/* 保存窗口指针和原始标题，用于在暂停/恢复时更新标题栏 */
static GtkWidget *g_main_window = nullptr;
static std::string g_win_title;

/**
 * GTK key-press-event 回调.
 * 仅在 pause_ctrl::g_enabled 为 true 时生效 (由 toggle() 内部判断).
 * 按下空格键切换暂停/运行状态，并同步更新窗口标题以提示用户。
 */
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void)widget;
    (void)user_data;
    if (event->keyval == GDK_KEY_space)
    {
        pause_ctrl::toggle();
        /* 更新窗口标题: 暂停时加 [PAUSED] 提示，恢复后还原 */
        if (g_main_window)
        {
            if (pause_ctrl::is_paused())
            {
                std::string t = g_win_title + " [PAUSED - SPACE to resume]";
                gtk_window_set_title(GTK_WINDOW(g_main_window), t.c_str());
            }
            else
            {
                gtk_window_set_title(GTK_WINDOW(g_main_window), g_win_title.c_str());
            }
        }
        return TRUE; /* 事件已处理，不再向下传递 */
    }
    return FALSE;
}

/* —— 无图形会话(SSH / VS Code Remote / systemd 服务)拉起时，也能连上板端 HDMI 的 X(:0) ——
 * 现象：这些场景下 shell 没有指向本地 :0 的 DISPLAY、也没有 XAUTHORITY cookie，gtk_init 连不上 X，
 *       于是“先得在桌面会话/命令行手动跑一次才显示”。下面在 gtk_init 之前自助补齐显示环境：
 *       DISPLAY 缺省 :0；XAUTHORITY 从正在运行的 Xorg 的 -auth 参数(或常见 cookie 路径)取。
 *       已设置的一律不动 —— 所以 ssh -X 转发、板端桌面会话、命令行里照常用各自继承的值。 */
static std::string find_xorg_auth()
{
    DIR *d = opendir("/proc");
    if (!d)
        return "";
    std::string result;
    for (struct dirent *e; (e = readdir(d)) != nullptr;)
    {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue; /* 只看 pid 目录 */
        std::string base = std::string("/proc/") + e->d_name;

        char comm[256] = {0};
        FILE *cf = fopen((base + "/comm").c_str(), "r");
        if (!cf)
            continue;
        bool got = fgets(comm, sizeof(comm), cf) != nullptr;
        fclose(cf);
        if (!got)
            continue;
        std::string name(comm);
        if (name.rfind("Xorg", 0) != 0 && name.rfind("X\n", 0) != 0)
            continue; /* 进程名为 Xorg / X */

        FILE *pf = fopen((base + "/cmdline").c_str(), "rb");
        if (!pf)
            continue;
        std::vector<std::string> args;
        std::string cur;
        for (int c; (c = fgetc(pf)) != EOF;)
        {
            if (c == 0)
            {
                if (!cur.empty())
                    args.push_back(cur);
                cur.clear();
            }
            else
                cur.push_back((char)c);
        }
        if (!cur.empty())
            args.push_back(cur);
        fclose(pf);

        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "-auth")
            {
                result = args[i + 1];
                break;
            } /* 取 -auth 后面的 cookie 路径 */
        if (!result.empty())
            break;
    }
    closedir(d);
    return result;
}

static void ensure_x_display_env()
{
    if (!getenv("DISPLAY"))
        setenv("DISPLAY", ":0", 0); /* 缺省连本地 HDMI :0(已设则不动，如 ssh -X 的转发显示) */

    if (getenv("XAUTHORITY"))
        return; /* 已有 cookie 就用现成的(桌面会话/命令行) */

    std::string auth = find_xorg_auth(); /* 1) 优先从 Xorg 的 -auth 取真实 cookie */
    if (auth.empty())
    { /* 2) 兜底常见路径 */
        const char *cands[] = {"/run/lightdm/root/:0", "/var/run/lightdm/root/:0", "/run/user/0/gdm/Xauthority",
                               "/root/.Xauthority", nullptr};
        for (int i = 0; cands[i]; ++i)
            if (access(cands[i], R_OK) == 0)
            {
                auth = cands[i];
                break;
            }
    }
    if (!auth.empty())
        setenv("XAUTHORITY", auth.c_str(), 1);
}

static GtkWidget *disp_init(const char *strWinTitle, int32_t width, int32_t height)
{
    ensure_x_display_env(); /* 必须在 gtk_init 之前：无图形会话时自助补 DISPLAY/XAUTHORITY */
    /* 板子没装无障碍(AT-SPI)总线，GTK 会刷一行 dbind-WARNING；关掉无障碍桥，纯净日志、无任何副作用 */
    setenv("NO_AT_BRIDGE", "1", 0);
    gtk_init(NULL, NULL);

    static GtkWidget *pWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (pWindow)
    {
        g_signal_connect(pWindow, "destroy", G_CALLBACK(gtk_main_quit), NULL);
        gtk_window_set_title(GTK_WINDOW(pWindow), strWinTitle);
        gtk_window_set_default_size(GTK_WINDOW(pWindow), width, height);

        /* 挂接键盘事件 — 仅在 pause_ctrl 已启用时有实际意义;
         * 即使功能关闭，连接信号也无害：toggle() 内部会直接返回。
         * gtk_widget_add_events 确保窗口接收键盘事件。 */
        gtk_widget_add_events(pWindow, GDK_KEY_PRESS_MASK);
        g_signal_connect(pWindow, "key-press-event", G_CALLBACK(on_key_press), NULL);

        /* 保存窗口指针和标题，供 on_key_press 更新标题栏 */
        g_main_window = pWindow;
        g_win_title = strWinTitle ? strWinTitle : "";
    }
    else
    {
        return NULL;
    }

    return pWindow;
}
static int32_t disp_set_loop(GtkWidget *pWindow, GSourceFunc pCamUpdate)
{
    if (NULL == pWindow)
    {
        return -1;
    }
    GtkWidget *image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(pWindow), image);
    GSource *gSource = g_timeout_source_new(33); // ~30fps refresh
    g_source_set_callback(gSource, pCamUpdate, image, NULL);
    g_source_attach(gSource, NULL);
    /* g_source_attach 内部已经为 GMainContext 增加了一份引用,
     * 调用方持有的初始引用必须释放, 否则该 GSource 永不销毁,
     * 进而 leak 一份 GLib wakeup pipe (display 热重启场景累积). */
    g_source_unref(gSource);
    return 0;
}
int display(Display_t *disp)
{
    GtkWidget *pWindow = disp_init(disp->winTitle, disp->width, disp->height);
    if (pWindow)
    {
        g_disp.desc = disp;
        disp_set_loop(pWindow, (GSourceFunc)showWidget);
        gtk_widget_show_all(pWindow);
    }
    gtk_main();

    return 0;
}
