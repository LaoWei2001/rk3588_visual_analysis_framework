#pragma once

#include <cstdint>
#include <opencv2/opencv.hpp>
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
