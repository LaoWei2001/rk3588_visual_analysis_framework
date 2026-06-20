/**
 * @file logic_tools.h
 * @brief 业务逻辑共享数据结构与工具函数
 *
 * 包含:
 * - HookState:      logic_hook 使用的吊钩安全圈检测跨帧状态
 * - PersonAlarmState: logic_person_alarm 使用的人员入侵检测跨帧状态
 * - logic_roll_compute_occupancy(): 基于 HSV 的物料占用率检测算法
 */

#pragma once

#include <opencv2/opencv.hpp>
#include <cstdint>
#include <set>
#include <map>

/*======================== HookState — 吊钩安全圈检测状态 ========================*/

struct HookState
{
    /* ---- 核心状态机 ---- */
    bool     alarm_active         = false; // 钩子正在圈外（出圈计时中）
    bool     alarm_sent_latch     = false; // 已发送报警（锁存，防重复上报）
    bool     alarm_reset_counting = false; // 正在圈内冷却倒计时
    uint64_t alarm_start_ms       = 0;     // 本次出圈开始时刻
    uint64_t alarm_reset_start_ms = 0;     // 本次进圈冷却开始时刻
    uint64_t last_found_ms        = 0;     // 最后一次检测到目标的时间戳（用于漏检宽限期）

    /* ---- 显示用累计时间（语义见注释）----
     *
     * disp_outside_sec:
     *   - 钩子在圈外时持续累加（报警前）
     *   - 钩子回圈内且尚未报警 → 归零（短暂误检）
     *   - 报警触发后冻结，直到冷却完成才归零
     *
     * disp_inside_sec:
     *   - 仅在"已报警 + 钩子在圈内"时累加（冷却计时）
     *   - 钩子再次出圈 → 归零
     *   - 冷却完成后归零（连同 disp_outside_sec 一起重置）
     */
    float disp_outside_sec = 0.0f;
    float disp_inside_sec  = 0.0f;
};

/*======================== PersonAlarmState — 人员检测报警状态 ========================*/

/**
 * 由 logic_person_alarm (channel_logic) 写入,
 * 由 global_example / global_default (global_logic) 读取。
 */
struct PersonAlarmState
{
    bool person_detected = false;
    int person_count = 0;
};

/*======================== DifyPersonVerifyState — 人员Dify二次核验状态 ========================*/

/**
 * 由 logic_dify_person_verify 使用。
 * 利用 tracker 分配的 track_id 防止对同一人员的重复上报。
 */
struct DifyPersonVerifyState
{
    std::set<int> reported_ids;             // 已上报过Dify的 track_id 集合
    std::map<int, int> miss_frames;         // 每个 track_id 连续丢失的帧数, 超过阈值才清除
    uint64_t last_upload_ms = 0;            // 最近一次上报时间戳
    uint64_t last_dbg_ms = 0;               // 调试打印节流
    int first = 1;                          // 首帧跳过(避免初始化误报)
};

/*======================== logic_roll_compute_occupancy ========================*/

/**
 * @brief 基于 HSV 色彩空间的物料占用率检测
 *
 * 在 ROI 区域内计算与背景色调差异较大的像素占比,
 * 用于检测传送带 / 料斗等场景的物料堆积情况。
 *
 * @param bgr         输入 BGR 图像
 * @param roi_points  ROI 多边形顶点
 * @param sat_min     饱和度下限
 * @param val_min     明度下限
 * @param hue_tol     色调容差 (环形距离)
 * @param min_area    最小连通域面积 (滤噪)
 * @param[out] ratio  占用像素占比 (0~1)
 * @param[out] occupancy_mask  占用区域二值掩码
 * @param[out] bg_hue 检测到的背景主色调 (0~180)
 * @return true 成功, false 失败 (输入无效)
 */
bool logic_roll_compute_occupancy(const cv::Mat &bgr,
                                  const std::vector<cv::Point> &roi_points,
                                  int sat_min,
                                  int val_min,
                                  int hue_tol,
                                  int min_area,
                                  double &ratio,
                                  cv::Mat &occupancy_mask,
                                  int &bg_hue);
