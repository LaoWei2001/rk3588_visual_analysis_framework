/**
 * @file bytetrack.h
 * @brief ByteTrack 多目标跟踪器（每个视频通道独立实例）
 *
 * 高置信度检测负责创建/确认轨迹；低置信度检测只用于续接当前仍活跃的
 * 已确认轨迹，因此不会把孤立低分框暴露给业务逻辑。
 */
#pragma once

#include "inference/inference_types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ByteTracker
{
  public:
    /**
     * @param high_iou_thresh 第一轮（高置信度）关联的最小 IoU
     * @param low_iou_thresh  第二轮（低置信度）关联的最小 IoU
     * @param low_score_thresh 进入第二轮关联的最低置信度
     * @param tracker_max_miss 已确认轨迹允许连续丢失的最大帧数
     * @param tracker_min_hits 新轨迹确认所需的连续高置信度命中数
     * @param default_high_score_thresh 找不到模型专属阈值时使用的高置信度阈值
     */
    explicit ByteTracker(float high_iou_thresh = 0.3f, float low_iou_thresh = 0.2f, float low_score_thresh = 0.1f,
                         int tracker_max_miss = 30, int tracker_min_hits = 3, float default_high_score_thresh = 0.3f);
    ~ByteTracker();

    /** 更新一帧。未匹配的低分框和低于 low_score_thresh 的框会从 detections 中移除。 */
    void update(std::vector<AlgoResult> &detections);

    /** 配置各模型的高置信度阈值；key 为 AlgoResult.model_id。 */
    void setModelHighThresholds(const std::unordered_map<std::string, float> &thresholds);

    /** 清空轨迹并让此通道的 ID 从 1 重新开始。 */
    void reset();

    ByteTracker(const ByteTracker &) = delete;
    ByteTracker &operator=(const ByteTracker &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
