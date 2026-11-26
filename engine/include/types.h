/** @file types.h
 * @brief 通用视觉数据：检测框、类别、跟踪 ID、姿态/分割结果、ROI、绘图指令。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <algorithm>
#include <array>
#include <coordinates.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

/* 姿态结果的稳定业务语义。模型版本可以变化，logic 只依赖这里的关键点定义。 */
enum class PoseKeypointSchema : uint8_t
{
    None = 0,
    Coco17,
    Hand21,
};

enum class DepthKind : uint8_t
{
    Metric = 0,      // metres, smaller is nearer (YOLO26Depth)
    RelativeInverse, // unitless, larger is nearer (ZipDepth)
};

struct AlgoResult
{
    cv::Rect box; // OBB: axis-aligned envelope for existing tracking/business logic
    bool has_obb = false;
    std::array<cv::Point2f, 4> obb_corners{}; // actual polygon, same coordinates as box
    std::string label;
    int class_id = -1;
    float score = 0.0f;
    int track_id = -1;         // assigned by tracker
    int chn_id = -1;           // config.channels[].id（logic 可见的稳定通道 ID）
    int64_t frame_id = 0;      // monotonically increasing per channel
    uint64_t timestamp_ms = 0; // 对应帧的分析时间，与视频回调的 timestamp_ms 一致；不是日历时间
    std::string model_id;      // 同通道多模型来源ID
    std::string model_type;    // yolov8_det / yolov8_pose / yolo26_pose / ...
    int model_index = 0;       // 在本次有效模型列表中的顺序
    bool show_detection_box = true; // Business logic may supply its own localized boxes.
    cv::Scalar box_color = cv::Scalar(-1, -1, -1); // (-1,-1,-1) means use default color

    /** @brief 返回包围框的整数中心；坐标需在 int 可表示范围内。
     * 示例：
     * @code
     * AlgoResult r;
     * r.box = {10, 20, 40, 60};
     * cv::Point center = r.box_center();
     * @endcode
     */
    cv::Point box_center() const
    {
        return cv::Point(box.x + box.width / 2, box.y + box.height / 2);
    }

    std::vector<cv::Point2f> keypoints;
    std::vector<float> keypoint_scores;
    PoseKeypointSchema pose_schema = PoseKeypointSchema::None;

    /** @brief 检查是否声明了姿态 schema；不校验关键点数量/置信度。
     * 示例：
     * @code
     * AlgoResult r;
     * bool pose = r.is_pose();
     * @endcode
     */
    bool is_pose() const
    {
        return pose_schema != PoseKeypointSchema::None;
    }

    /** @brief 检查 COCO17 schema 且坐标/置信度数组均为 17 项。
     * 示例：
     * @code
     * AlgoResult r;
     * if (r.is_coco17_pose())
     * {
     *     cv::Point2f nose = r.keypoints[0];
     * }
     * @endcode
     */
    bool is_coco17_pose() const
    {
        return pose_schema == PoseKeypointSchema::Coco17 && keypoints.size() == 17 && keypoint_scores.size() == 17;
    }

    std::string text_result;
    cv::Mat boxMask;

    /* 单目深度结果：CV_32FC1，单位由 depth_kind 指定；业务坐标固定 640×640。
     * 米制无效区域为 0；相对逆深度无效区域为 NaN（0/负数也可为有效预测）。
     * 每个深度模型每帧一条结果，不参与目标计数、跟踪或 NMS。
     * Mat 引用可安全跨帧持有；如需修改像素，先 clone()。 */
    cv::Mat depth_map;
    DepthKind depth_kind = DepthKind::Metric;

    /** @brief 检查是否携带非空深度矩阵；不检查矩阵类型或像素有效性。
     * 示例：
     * @code
     * if (const auto *r = ctx->depth_result())
     * {
     *     bool has_depth = r->is_depth();
     *     (void)has_depth;
     * }
     * @endcode
     */
    bool is_depth() const
    {
        return !depth_map.empty();
    }
    /** @brief 检查非空深度结果是否使用米制单位；数值越小通常越近。
     * 示例：
     * @code
     * if (const auto *r = ctx->depth_result())
     * {
     *     if (r->is_metric_depth())
     *     {
     *         float m = r->depth_at(320, 320);
     *         (void)m;
     *     }
     * }
     * @endcode
     */
    bool is_metric_depth() const
    {
        return is_depth() && depth_kind == DepthKind::Metric;
    }
    /** @brief 检查非空结果是否为相对逆深度；不能当作米，数值越大通常越近。
     * 示例：
     * @code
     * if (const auto *r = ctx->depth_result())
     * {
     *     bool relative = r->is_relative_inverse_depth();
     *     (void)relative;
     * }
     * @endcode
     */
    bool is_relative_inverse_depth() const
    {
        return is_depth() && depth_kind == DepthKind::RelativeInverse;
    }

    /** @brief 返回当前深度类型的无效值：米制为 0，相对逆深度为 NaN。
     * 示例：
     * @code
     * AlgoResult r;
     * r.depth_map = cv::Mat(640, 640, CV_32FC1, cv::Scalar(r.invalid_depth_value()));
     * @endcode
     */
    float invalid_depth_value() const
    {
        return depth_kind == DepthKind::RelativeInverse ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
    }

    /** @brief 检查米制深度是否有限且严格大于 0；不适用于相对逆深度。
     * 示例：
     * @code
     * bool valid = AlgoResult::valid_depth(2.5f);
     * @endcode
     */
    static bool valid_depth(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits != 0 && bits < 0x7f800000U;
    }

    /** @brief 按 depth_kind 判断像素有效性；相对逆深度允许有限的 0/负数。
     * 示例：
     * @code
     * AlgoResult r;
     * r.depth_kind = DepthKind::RelativeInverse;
     * bool valid = r.valid_depth_value(0);
     * @endcode
     */
    bool valid_depth_value(float value) const
    {
        if (depth_kind == DepthKind::Metric)
            return valid_depth(value);
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7fffffffU) < 0x7f800000U;
    }

    /** @brief 检查深度图坐标 (x,y) 是否有效；空图、类型不符或越界返回 false。
     * 示例：
     * @code
     * if (const auto *r = ctx->depth_result())
     * {
     *     if (r->depth_valid_at(320, 320))
     *     {
     *         float d = r->depth_at(320, 320);
     *         (void)d;
     *     }
     * }
     * @endcode
     */
    bool depth_valid_at(int x, int y) const
    {
        return depth_map.type() == CV_32FC1 && x >= 0 && y >= 0 && x < depth_map.cols && y < depth_map.rows &&
               valid_depth_value(depth_map.at<float>(y, x));
    }

    /** @brief 读取原始深度值；无效/越界返回 0，使用 depth_valid_at 区分有效的相对深度 0。
     * 示例：
     * @code
     * if (const auto *r = ctx->depth_result())
     * {
     *     float d = r->depth_at(320, 320);
     *     (void)d;
     * }
     * @endcode
     */
    float depth_at(int x, int y) const
    {
        if (depth_map.type() != CV_32FC1 || x < 0 || y < 0 || x >= depth_map.cols || y >= depth_map.rows)
            return 0.0f;
        const float value = depth_map.at<float>(y, x);
        return valid_depth_value(value) ? value : 0.0f;
    }

    /** @brief 取 region 内有效深度中位数；区域裁剪到图内，step 为采样步长（1 为逐像素）。
     * @return 无有效样本、非深度图或 step<=0 时为 0；相对逆深度结果仍不代表米。
     * 示例：
     * @code
     * if (const auto *r = ctx->depth_result())
     * {
     *     float d = r->depth_median(cv::Rect(200, 200, 100, 100), 1);
     *     (void)d;
     * }
     * @endcode
     */
    float depth_median(const cv::Rect &region, int step = 4) const
    {
        if (depth_map.type() != CV_32FC1 || step <= 0)
            return 0.0f;
        const cv::Rect area = region & cv::Rect(0, 0, depth_map.cols, depth_map.rows);
        if (area.empty())
            return 0.0f;
        std::vector<float> values;
        values.reserve(static_cast<size_t>((area.width - 1) / step + 1) * ((area.height - 1) / step + 1));
        for (int y = area.y; y < area.y + area.height; y += step)
        {
            const float *row = depth_map.ptr<float>(y);
            for (int x = area.x; x < area.x + area.width; x += step)
                if (valid_depth_value(row[x]))
                    values.push_back(row[x]);
        }
        if (values.empty())
            return 0.0f;
        const auto middle = values.begin() + values.size() / 2;
        std::nth_element(values.begin(), middle, values.end());
        if (values.size() % 2)
            return *middle;
        return (*middle + *std::max_element(values.begin(), middle)) * 0.5f;
    }
};

/*======================== ROI 区域 (一个通道可配置多个) ========================*/
/**
 * 一个 ROI 区域 = 区域名 + 多边形顶点。顶点坐标系 = 固定业务画布(640×640),
 * 与 ctx->results[].box 完全一致 —— 逻辑里可直接 cv::pointPolygonTest, 无需再缩放。
 *
 * 通道逻辑通过 ctx->rois (全部区域) 或 ctx->roi_by_name("xxx") / ctx->roi_polygon_at(i)
 * 等便捷方法访问本通道的各个区域。
 */
struct RoiZone
{
    std::string name;               /* 区域名(可空), 如 "entrance"/"exit"; 供逻辑按名取用 */
    std::vector<cv::Point> polygon; /* 顶点, 业务坐标系(640×640); >=3 个点才算有效区域 */
};

/*======================== 绘制指令 ========================*/
struct DrawCommand
{
    enum Type
    {
        RECT,
        CIRCLE,
        LINE,
        TEXT,
        POLYLINE,
        POLY_FILLED
    } type;

    enum Target : uint8_t
    {
        DISPLAY = 0x01,
        IMAGE = 0x02,
        VIDEO = 0x04,
        MEDIA = IMAGE | VIDEO,
        ALL = DISPLAY | IMAGE | VIDEO
    };
    uint8_t target = ALL;

    cv::Rect rect;
    cv::Point center;
    int radius = 0;
    cv::Point pt1, pt2;
    std::vector<cv::Point> points; /* POLYLINE: 折线顶点(640×640 业务坐标系) */
    bool closed = false;           /* POLYLINE: 是否闭合 */
    double alpha = 1.0;            /* 透明度 0~1, <1 半透明叠加(RECT/CIRCLE/POLYLINE/POLY_FILLED 均支持) */
    std::string text;
    cv::Point text_pos;
    /* true 时 text_pos.x 表示文字右边界；用于与窗口宽度无关的右对齐状态标签。 */
    bool text_align_right = false;
    double font_scale = 0.6;
    bool text_shadow_enabled = false;
    cv::Scalar text_shadow_color = cv::Scalar(0, 0, 0);
    int text_shadow_width = 2;

    cv::Scalar color = cv::Scalar(0, 255, 0);
    int thickness = 2;
};
