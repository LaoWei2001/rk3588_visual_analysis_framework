/**
 * @file tracker.h
 * @brief SORT (Simple Online Realtime Tracking) 多目标跟踪器
 *
 * 基于标准 SORT 算法:
 * - 7维卡尔曼滤波 [cx, cy, area, aspect_ratio, vcx, vcy, va]
 * - IoU 距离矩阵 + 匈牙利算法最优匹配
 * - 新轨迹需连续命中确认, 防止误检闪烁
 *
 * 每个通道应持有自己的 Tracker 实例, ID 独立分配。
 */
#pragma once

#include "algoProcess.h"
#include <memory>
#include <vector>

class Tracker
{
  public:
    /**
     * @param tracker_iou_thresh  IoU 匹配阈值 (0~1), 低于此值视为不匹配
     * @param tracker_max_miss    连续丢失帧数上限, 超过则删除轨迹
     * @param tracker_min_hits    新轨迹需连续命中多少帧才确认(分配 track_id)
     */
    explicit Tracker(float tracker_iou_thresh = 0.3f, int tracker_max_miss = 10, int tracker_min_hits = 3);
    ~Tracker();

    /** 用当前帧检测结果更新轨迹, 自动分配 track_id 到每个 AlgoResult */
    void update(std::vector<AlgoResult> &detections);

    /** 清空所有轨迹状态 */
    void reset();

    void setTrackerIoUThresh(float thresh);
    void setTrackerMaxMiss(int max_miss);

    /* 不可拷贝 */
    Tracker(const Tracker &) = delete;
    Tracker &operator=(const Tracker &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
