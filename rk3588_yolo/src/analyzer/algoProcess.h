#pragma once
#include "../config/config.h"
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

struct AlgoResult
{
    cv::Rect box;
    std::string label;
    int class_id = -1;
    float score = 0.0f;
    int track_id = -1; // assigned by tracker
    int chn_id = -1;
    int64_t frame_id = 0;      // monotonically increasing per channel
    uint64_t timestamp_ms = 0; // wall clock in milliseconds

    cv::Scalar box_color = cv::Scalar(-1, -1, -1); // (-1,-1,-1) means use default color

    /* 卡尔曼速度 (模型输入像素/推理帧), 由 tracker 写入。
     * 仅对 confirmed 轨迹 (track_id >= 0) 有效，用于显示层前向外推补偿管线延迟。
     * 未经 tracker 处理时保持 0。*/
    float vx = 0.0f;    // cx 方向速度 (pixels/inference-frame)
    float vy = 0.0f;    // cy 方向速度 (pixels/inference-frame)
    int track_hits = 0; // 轨迹连续命中帧数，越大速度估计越可靠

    /* ---- 便捷访问方法 ---- */
    cv::Point box_center() const
    {
        return cv::Point(box.x + box.width / 2, box.y + box.height / 2);
    }
    bool box_contains(const cv::Point &p) const
    {
        return box.contains(p);
    }
    // 检测框中心到指定点的距离平方 (避免 sqrt, 适合阈值比较)
    int dist_sq_to(const cv::Point &p) const
    {
        cv::Point c = box_center();
        int dx = c.x - p.x, dy = c.y - p.y;
        return dx * dx + dy * dy;
    }

    /* Model specific optional fields */
    std::vector<cv::Point2f> keypoints; // for pose estimation
    std::string text_result;            // for OCR
    cv::Mat boxMask;                    // for segmentation (mask of the whole image with class ids,
                                        // or object specific mask)
};

int algorithm_init(const AppConfig &cfg);
int algorithm_process_mat(int chnId, cv::Mat &&frame, int fd = -1, int srcW = 0, int srcH = 0, int srcFmt = 0,
                          int srcStrH = 0, int srcStrV = 0, int64_t frame_seq = 0);
void algorithm_deinit();
/*
 * out_frame 是产出 out 这批检测结果时使用的 yolo 输入帧 (BGR, inputW×inputH)。
 * logic 用这一帧做"图像 + 检测框"一致的报警/上报。
 * 没有新结果时返回 false, out 和 out_frame 都被 clear/release。
 */
bool algorithm_take_results(int chnId, std::vector<AlgoResult> &out, cv::Mat &out_frame, int64_t &out_frame_id);
int algorithm_get_input_w();
int algorithm_get_input_h();
float algorithm_get_infer_fps(int chnId);
void algorithm_update_thresh(int chnId, float obj_thresh, float nms_thresh);
void algorithm_update_detect_classes(int chnId, const std::vector<std::string> &class_names);
void algorithm_reload_channel_model(int chnId, const ChannelConfig &new_cfg);

/**
 * @brief 阻塞直到指定通道有新的推理结果就绪, 或超时。
 * 不消费结果 (has_new 由 algorithm_take_results 消费)。
 * 用于层次二分发器线程: NPU 完成后立即唤醒而不等待下一帧解码回调。
 * @param chnId   通道号
 * @param timeout_ms 最长等待毫秒数
 * @return true 若结果就绪 (不代表 has_new 仍为 true, 取决于竞争), false
 * 超时/关机
 */
bool algorithm_wait_result(int chnId, int timeout_ms);
