/**
 * @file tracker.h
 * @brief SORT (Simple Online Realtime Tracking) 多目标跟踪器
 *
 * 基于 SORT 算法，增加带门控的运动恢复关联:
 * - 7维卡尔曼滤波 [cx, cy, area, aspect_ratio, vcx, vcy, va]
 * - IoU 门控 + 匈牙利算法最大基数/最小代价匹配
 * - 已确认轨迹的预测中心、面积和宽高比二次关联
 * - 新轨迹需连续命中确认, 防止误检闪烁
 *
 * 每个通道应持有自己的 Tracker 实例, ID 独立分配。
 */
#pragma once

#include <memory>
#include <types.h>
#include <vector>

class Tracker
{
  public:
    /** @brief 为一路视频创建 SORT 跟踪器；IoU 阈值 0~1，max_miss/min_hits 为正帧数。
     * @note 每通道独立实例，按帧串行 update；调用方保证参数合法。
     * 示例：
     * @code
     * Tracker tracker(0.3f, 10, 3);
     * @endcode
     */
    explicit Tracker(float tracker_iou_thresh = 0.3f, int tracker_max_miss = 10, int tracker_min_hits = 3);
    /** @brief 释放内部轨迹；外部 detections 中已写入的 ID 不会被修改。
     * 示例：
     * @code
     * {
     *     Tracker tracker;
     * }
     * @endcode
     */
    ~Tracker();

    /** @brief 用本帧检测更新轨迹并原位写 track_id；未确认目标为 -1，空输入推动丢失计数。
     * @note 检测已完成阈值过滤，所有帧使用同一坐标系；深度载荷不参与跟踪。
     * 示例：
     * @code
     * std::vector<AlgoResult> detections;
     * tracker.update(detections);
     * @endcode
     */
    void update(std::vector<AlgoResult> &detections);

    /** @brief 清空全部轨迹并重置 ID 序列；断流或切换业务来源时调用。
     * 示例：
     * @code
     * tracker.reset();
     * @endcode
     */
    void reset();

    /** @brief 更新后续匹配使用的 IoU 阈值（0~1）；不清空已有轨迹。
     * 示例：
     * @code
     * tracker.setTrackerIoUThresh(0.4f);
     * @endcode
     */
    void setTrackerIoUThresh(float thresh);
    /** @brief 更新连续丢失帧数上限（正数）；不立即清空轨迹。
     * 示例：
     * @code
     * tracker.setTrackerMaxMiss(15);
     * @endcode
     */
    void setTrackerMaxMiss(int max_miss);

    /** @brief 禁止复制构造，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = tracker;
     * (void)borrowed;
     * @endcode
     */
    Tracker(const Tracker &) = delete;
    /** @brief 禁止复制赋值，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = tracker;
     * (void)borrowed;
     * @endcode
     */
    Tracker &operator=(const Tracker &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
