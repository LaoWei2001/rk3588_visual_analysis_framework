/**
 * @file bytetrack.h
 * @brief ByteTrack 多目标跟踪器（每个视频通道独立实例）
 *
 * 高置信度检测负责创建/确认轨迹；低置信度检测只用于续接当前仍活跃的
 * 已确认轨迹。对外只返回达到所属模型置信阈值的检测，低分框不进入业务结果或默认绘图。
 */
#pragma once

#include <types.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ByteTracker
{
  public:
    /** @brief 创建两阶段跟踪器；前两项为高/低分关联 IoU，第三项为低分门槛。
     * @param tracker_max_miss 丢失上限；tracker_min_hits 为确认帧数，default_high_score_thresh 为缺省高分门槛。
     * @note 有限阈值限制到 [0,1]、帧数至少 1；每通道独立实例，串行访问。
     * 示例：
     * @code
     * ByteTracker tracker(0.3f, 0.2f, 0.1f, 30, 3, 0.5f);
     * @endcode
     */
    explicit ByteTracker(float high_iou_thresh = 0.3f, float low_iou_thresh = 0.2f, float low_score_thresh = 0.1f,
                         int tracker_max_miss = 30, int tracker_min_hits = 3, float default_high_score_thresh = 0.3f);
    /** @brief 释放当前实例的轨迹资源。
     * 示例：
     * @code
     * {
     *     ByteTracker tracker;
     * }
     * @endcode
     */
    ~ByteTracker();

    /** @brief 原位关联并写入 track_id；低分检测只内部续接，高分检测才返回给业务。
     * @note 要在删除低分框之前调用；未确认的高分框保留，ID 为 -1；输入坐标须跨帧一致。
     * 示例：
     * @code
     * std::vector<AlgoResult> detections;
     * tracker.update(detections);
     * @endcode
     */
    void update(std::vector<AlgoResult> &detections);

    /** @brief 替换模型专属高分门槛，key 为 model_id；缺省模型使用构造参数中的门槛。
     * @note 有限阈值限制到 [0,1]；同一实例更新参数与 update() 不可并发。
     * 示例：
     * @code
     * tracker.setModelHighThresholds({{"detector", 0.5f}});
     * @endcode
     */
    void setModelHighThresholds(const std::unordered_map<std::string, float> &thresholds);

    /** @brief 清空轨迹并让下一条轨迹从 ID=1 开始，不清除模型门槛表。
     * 示例：
     * @code
     * tracker.reset();
     * @endcode
     */
    void reset();

    /** @brief 禁止复制构造，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = tracker;
     * (void)borrowed;
     * @endcode
     */
    ByteTracker(const ByteTracker &) = delete;
    /** @brief 禁止复制赋值，避免复制资源或跟踪状态；共享实例请借用引用。
     * 示例：
     * @code
     * auto &borrowed = tracker;
     * (void)borrowed;
     * @endcode
     */
    ByteTracker &operator=(const ByteTracker &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
