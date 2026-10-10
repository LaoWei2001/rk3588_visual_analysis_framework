/** @file types.h
 * @brief 通用视觉数据：检测框、类别、跟踪 ID、姿态/分割结果、ROI、绘图指令和帧时间。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <coordinates.h>
#include <cstdint>
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

struct AlgoResult
{
    cv::Rect box;
    std::string label;
    int class_id = -1;
    float score = 0.0f;
    int track_id = -1;         // assigned by tracker
    int chn_id = -1;           // config.channels[].id（logic 可见的稳定通道 ID）
    int64_t frame_id = 0;      // monotonically increasing per channel
    uint64_t timestamp_ms = 0; // 对应业务帧进入分析管线时的 steady 毫秒；不是日历时间
    std::string model_id;      // 同通道多模型来源ID
    std::string model_type;    // yolov8_det / yolov8_pose / yolo26_pose / ...
    int model_index = 0;       // 在本次有效模型列表中的顺序
    cv::Scalar box_color = cv::Scalar(-1, -1, -1); // (-1,-1,-1) means use default color

    cv::Point box_center() const
    {
        return cv::Point(box.x + box.width / 2, box.y + box.height / 2);
    }

    bool box_contains(const cv::Point &point) const
    {
        return box.contains(point);
    }

    int dist_sq_to(const cv::Point &point) const
    {
        const cv::Point center = box_center();
        const int dx = center.x - point.x;
        const int dy = center.y - point.y;
        return dx * dx + dy * dy;
    }

    std::vector<cv::Point2f> keypoints;
    std::vector<float> keypoint_scores;
    PoseKeypointSchema pose_schema = PoseKeypointSchema::None;

    bool is_pose() const
    {
        return pose_schema != PoseKeypointSchema::None;
    }

    bool is_coco17_pose() const
    {
        return pose_schema == PoseKeypointSchema::Coco17 && keypoints.size() == 17 && keypoint_scores.size() == 17;
    }

    std::string text_result;
    cv::Mat boxMask;
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

/*======================== 帧时间 (年月日时分秒, 由 ctx->datetime() 拆出) ========================*/
struct FrameTime
{
    int year;   /* 如 2026 */
    int month;  /* 1~12 */
    int day;    /* 1~31 */
    int hour;   /* 0~23 */
    int minute; /* 0~59 */
    int second; /* 0~59 */
    int millis; /* 0~999 */
};
