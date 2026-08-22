#pragma once

#include <cstdint>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

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
    std::string model_type;    // yolov8_det / yolov8_pose / ...
    int model_index = 0;       // 在本次有效模型列表中的顺序
    cv::Scalar box_color = cv::Scalar(-1, -1, -1); // (-1,-1,-1) means use default color

    /* 卡尔曼速度 (模型输入像素/推理帧), 由 tracker 写入。
     * 仅对 confirmed 轨迹 (track_id >= 0) 有效，用于显示层前向外推补偿管线延迟。
     * 未经 tracker 处理时保持 0。*/
    float vx = 0.0f;
    float vy = 0.0f;
    int track_hits = 0;

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
    std::string text_result;
    cv::Mat boxMask;
};
