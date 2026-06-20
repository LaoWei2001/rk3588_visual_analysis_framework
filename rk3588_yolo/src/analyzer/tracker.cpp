/**
 * @file tracker.cpp
 * @brief SORT (Simple Online Realtime Tracking) 标准实现
 *
 * 核心改进 (相比旧版):
 * 1. 卡尔曼状态: [cx, cy, s, r, vcx, vcy, vs] — 跟踪面积(s)和宽高比(r)
 * 2. IoU 代价矩阵: 尺度不变, 大小目标同等有效
 * 3. 匈牙利算法: 全局最优匹配, 密集场景不错配
 * 4. min_hits 确认: 新轨迹连续命中 N 帧才分配 track_id, 过滤误检
 *
 * 参考: Bewley et al., "Simple Online and Realtime Tracking", ICIP 2016
 */
#include "tracker.h"
#include "../yolo/yolo_utils.h"

#include <opencv2/video/tracking.hpp>
#include <cmath>
#include <limits>
#include <algorithm>
#include <vector>
#include <numeric>

/* ==================== 匈牙利算法 (Kuhn-Munkres) ==================== */
namespace
{

    /**
     * @brief 匈牙利算法求最小代价分配
     * @param cost_matrix NxM 代价矩阵 (N=行=轨迹, M=列=检测)
     * @param assignment  输出: assignment[row] = 分配的列, -1 表示未分配
     *
     * 实现 O(n^3) 的标准 Kuhn-Munkres 算法。
     * 支持非方阵: 内部填充到 max(N,M) x max(N,M)。
     */
    static void hungarian_solve(const std::vector<std::vector<float>> &cost_matrix,
                                std::vector<int> &assignment)
    {
        int N = static_cast<int>(cost_matrix.size());
        if (N == 0)
        {
            assignment.clear();
            return;
        }
        int M = static_cast<int>(cost_matrix[0].size());
        if (M == 0)
        {
            assignment.assign(N, -1);
            return;
        }

        int n = std::max(N, M);
        const float BIG = 1e9f;

        /* 填充为方阵 */
        std::vector<std::vector<float>> C(n + 1, std::vector<float>(n + 1, 0.0f));
        for (int i = 1; i <= N; ++i)
            for (int j = 1; j <= M; ++j)
                C[i][j] = cost_matrix[i - 1][j - 1];

        /* 匈牙利算法 (1-indexed) */
        std::vector<float> u(n + 1, 0), v(n + 1, 0);
        std::vector<int> p(n + 1, 0), way(n + 1, 0);

        for (int i = 1; i <= n; ++i)
        {
            p[0] = i;
            int j0 = 0;
            std::vector<float> minv(n + 1, BIG);
            std::vector<bool> used(n + 1, false);
            do
            {
                used[j0] = true;
                int i0 = p[j0], j1 = 0;
                float delta = BIG;
                for (int j = 1; j <= n; ++j)
                {
                    if (!used[j])
                    {
                        float cur = C[i0][j] - u[i0] - v[j];
                        if (cur < minv[j])
                        {
                            minv[j] = cur;
                            way[j] = j0;
                        }
                        if (minv[j] < delta)
                        {
                            delta = minv[j];
                            j1 = j;
                        }
                    }
                }
                for (int j = 0; j <= n; ++j)
                {
                    if (used[j])
                    {
                        u[p[j]] += delta;
                        v[j] -= delta;
                    }
                    else
                    {
                        minv[j] -= delta;
                    }
                }
                j0 = j1;
            } while (p[j0] != 0);

            do
            {
                int j1 = way[j0];
                p[j0] = p[j1];
                j0 = j1;
            } while (j0);
        }

        assignment.assign(N, -1);
        for (int j = 1; j <= n; ++j)
        {
            if (p[j] > 0 && p[j] <= N && j <= M)
                assignment[p[j] - 1] = j - 1;
        }
    }

} /* anonymous namespace */

/* IoU 计算使用 yolo_utils.h 中的 compute_iou(cv::Rect_<float>, cv::Rect_<float>) */
using ::compute_iou;

/* ==================== 框 ↔ 状态转换 ==================== */
/* 状态: [cx, cy, s, r, vcx, vcy, vs]
 * cx, cy = 中心坐标
 * s      = 面积 (width * height)
 * r      = 宽高比 (width / height), 视为常量不直接参与速度估计
 */
static void box_to_state(const cv::Rect &box, float &cx, float &cy, float &s, float &r)
{
    float w = static_cast<float>(box.width);
    float h = static_cast<float>(box.height);
    cx = box.x + w * 0.5f;
    cy = box.y + h * 0.5f;
    s = w * h;
    r = (h > 0.0f) ? (w / h) : 1.0f;
}

static cv::Rect_<float> state_to_box(float cx, float cy, float s, float r)
{
    if (s <= 0.0f)
        s = 1.0f;
    if (r <= 0.0f)
        r = 1.0f;
    float w = std::sqrt(s * r);
    float h = s / w;
    return cv::Rect_<float>(cx - w * 0.5f, cy - h * 0.5f, w, h);
}

/* ==================== 内部轨迹结构 ==================== */
struct TrackEntry
{
    int id = -1;
    cv::KalmanFilter kf;
    cv::Rect_<float> last_box; /* 卡尔曼预测/修正后的框 */
    int miss = 0;              /* 连续未匹配帧数 */
    int hits = 0;              /* 连续命中帧数 */
    int total_hits = 0;        /* 总命中帧数 */
    bool confirmed = false;    /* 是否已确认(满足 min_hits) */
};

/* ==================== 卡尔曼滤波工厂 ==================== */
static cv::KalmanFilter make_kf(const cv::Rect &box)
{
    /*  7维状态: [cx, cy, s, r, vcx, vcy, vs]
     *  4维观测: [cx, cy, s, r]
     */
    cv::KalmanFilter kf(7, 4, 0, CV_32F);

    /* 状态转移矩阵 F */
    kf.transitionMatrix = cv::Mat::eye(7, 7, CV_32F);
    kf.transitionMatrix.at<float>(0, 4) = 1.0f; /* cx += vcx */
    kf.transitionMatrix.at<float>(1, 5) = 1.0f; /* cy += vcy */
    kf.transitionMatrix.at<float>(2, 6) = 1.0f; /* s  += vs  */

    /* 观测矩阵 H: 取前4个状态 */
    kf.measurementMatrix = cv::Mat::zeros(4, 7, CV_32F);
    kf.measurementMatrix.at<float>(0, 0) = 1.0f;
    kf.measurementMatrix.at<float>(1, 1) = 1.0f;
    kf.measurementMatrix.at<float>(2, 2) = 1.0f;
    kf.measurementMatrix.at<float>(3, 3) = 1.0f;

    /* 过程噪声 Q */
    cv::setIdentity(kf.processNoiseCov);
    kf.processNoiseCov.at<float>(4, 4) = 0.01f;
    kf.processNoiseCov.at<float>(5, 5) = 0.01f;
    kf.processNoiseCov.at<float>(6, 6) = 0.0001f;
    /* 位置和面积的过程噪声 */
    kf.processNoiseCov.at<float>(0, 0) = 1.0f;
    kf.processNoiseCov.at<float>(1, 1) = 1.0f;
    kf.processNoiseCov.at<float>(2, 2) = 1.0f;
    kf.processNoiseCov.at<float>(3, 3) = 1.0f;

    /* 观测噪声 R
     * cx/cy 从 1.0 降到 0.1: 更信任检测位置，Kalman 增益从 ~0.5 提升到 ~0.9，
     * 框位置更紧跟原始检测，减少快速运动时的视觉滞后。
     * 代价: 位置平滑度略降，检测本身有抖动时框会轻微抖动。
     * s/r 保持较高噪声 (10.0): 面积和宽高比的检测本身不稳定，继续平滑。 */
    cv::setIdentity(kf.measurementNoiseCov);
    kf.measurementNoiseCov.at<float>(0, 0) = 0.1f;  // cx: 原 1.0，更信任检测位置
    kf.measurementNoiseCov.at<float>(1, 1) = 0.1f;  // cy: 原 1.0，更信任检测位置
    kf.measurementNoiseCov.at<float>(2, 2) = 10.0f; // s (面积): 保持平滑
    kf.measurementNoiseCov.at<float>(3, 3) = 10.0f; // r (宽高比): 保持平滑

    /* 初始状态 */
    cv::setIdentity(kf.errorCovPost, cv::Scalar::all(10));
    kf.errorCovPost.at<float>(4, 4) = 10000.0f;
    kf.errorCovPost.at<float>(5, 5) = 10000.0f;
    kf.errorCovPost.at<float>(6, 6) = 10000.0f;

    float cx, cy, s, r;
    box_to_state(box, cx, cy, s, r);
    kf.statePost = cv::Mat::zeros(7, 1, CV_32F);
    kf.statePost.at<float>(0) = cx;
    kf.statePost.at<float>(1) = cy;
    kf.statePost.at<float>(2) = s;
    kf.statePost.at<float>(3) = r;

    return kf;
}

/* ==================== Tracker::Impl ==================== */
struct Tracker::Impl
{
    float tracker_iou_thresh = 0.3f;
    int tracker_max_miss = 10;
    int tracker_min_hits = 3;
    int next_track_id = 1; /* 每实例独立 ID 发号器 */
    std::vector<TrackEntry> tracks;

    /** 预测所有轨迹到当前帧 */
    void predict()
    {
        for (auto &t : tracks)
        {
            cv::Mat pred = t.kf.predict();
            float cx = pred.at<float>(0);
            float cy = pred.at<float>(1);
            float s = std::max(pred.at<float>(2), 1.0f);
            float r = pred.at<float>(3);
            t.last_box = state_to_box(cx, cy, s, r);
            t.miss++;
        }
    }

    /** 主更新: predict → IoU 代价矩阵 → 匈牙利匹配 → 创建/删除轨迹 */
    void update(std::vector<AlgoResult> &dets)
    {
        predict();

        if (dets.empty())
        {
            prune();
            return;
        }

        size_t T = tracks.size();
        size_t D = dets.size();

        /* ---- 构建 IoU 代价矩阵 (代价 = 1 - IoU) ---- */
        std::vector<std::vector<float>> cost(T, std::vector<float>(D, 0.0f));
        for (size_t i = 0; i < T; ++i)
        {
            for (size_t j = 0; j < D; ++j)
            {
                cv::Rect_<float> det_box(
                    static_cast<float>(dets[j].box.x),
                    static_cast<float>(dets[j].box.y),
                    static_cast<float>(dets[j].box.width),
                    static_cast<float>(dets[j].box.height));
                float iou = compute_iou(tracks[i].last_box, det_box);
                cost[i][j] = 1.0f - iou;
            }
        }

        /* ---- 匈牙利最优匹配 ---- */
        std::vector<int> track_assignment; /* track_assignment[track_idx] = det_idx or -1 */
        if (T > 0 && D > 0)
        {
            hungarian_solve(cost, track_assignment);
        }
        else
        {
            track_assignment.assign(T, -1);
        }

        /* 检查匹配是否满足 IoU 阈值, 不满足的视为未匹配 */
        std::vector<bool> det_matched(D, false);
        for (size_t i = 0; i < T; ++i)
        {
            int j = track_assignment[i];
            if (j >= 0 && j < static_cast<int>(D))
            {
                float iou = 1.0f - cost[i][j];
                if (iou < tracker_iou_thresh)
                {
                    track_assignment[i] = -1; /* IoU 太低, 拒绝匹配 */
                }
                else
                {
                    det_matched[j] = true;
                }
            }
        }

        /* ---- 更新已匹配轨迹 ---- */
        for (size_t i = 0; i < T; ++i)
        {
            int j = track_assignment[i];
            if (j < 0)
                continue;

            TrackEntry &tr = tracks[i];
            float cx, cy, s, r;
            box_to_state(dets[j].box, cx, cy, s, r);
            cv::Mat meas = (cv::Mat_<float>(4, 1) << cx, cy, s, r);
            cv::Mat est = tr.kf.correct(meas);

            float ecx = est.at<float>(0);
            float ecy = est.at<float>(1);
            float es = std::max(est.at<float>(2), 1.0f);
            float er = est.at<float>(3);
            tr.last_box = state_to_box(ecx, ecy, es, er);
            tr.miss = 0;
            tr.hits++;
            tr.total_hits++;

            if (!tr.confirmed && tr.hits >= tracker_min_hits)
            {
                tr.confirmed = true;
            }

            /* 只给已确认轨迹分配 track_id */
            if (tr.confirmed)
            {
                dets[j].track_id = tr.id;
            }

            /* 把 Kalman 矫正框写回 det，使显示层拿到平滑后的位置（R=0.1 时
             * 矫正框 ≈ 90% 原始检测，几乎无平滑误差但消除抖动）。
             * 同时写入速度和命中数，供显示层做前向外推补偿管线延迟。*/
            /* dets[j].box 保持原始 YOLO 检测框不变，确保业务逻辑取到的坐标
             * 与修改前完全一致。仅写入速度和命中数供显示层前向外推使用。*/
            dets[j].vx  = est.at<float>(4); // vcx: 像素/推理帧
            dets[j].vy  = est.at<float>(5); // vcy: 像素/推理帧
            dets[j].track_hits = tr.hits;
        }

        /* ---- 未匹配的检测 → 创建新轨迹 ---- */
        for (size_t j = 0; j < D; ++j)
        {
            if (det_matched[j])
                continue;
            TrackEntry tr;
            tr.id = next_track_id++;
            tr.kf = make_kf(dets[j].box);
            float cx, cy, s, r;
            box_to_state(dets[j].box, cx, cy, s, r);
            tr.last_box = state_to_box(cx, cy, s, r);
            tr.miss = 0;
            tr.hits = 1;
            tr.total_hits = 1;
            tr.confirmed = (tracker_min_hits <= 1);
            tracks.push_back(tr);

            if (tr.confirmed)
            {
                dets[j].track_id = tr.id;
            }
            /* 未确认的新轨迹: dets[j].track_id 保持 -1 */
        }

        /* ---- 未匹配的轨迹: hits 重置 ---- */
        for (size_t i = 0; i < T; ++i)
        {
            if (track_assignment[i] < 0)
            {
                tracks[i].hits = 0;
            }
        }

        prune();
    }

    /** 删除丢失过久的轨迹 */
    void prune()
    {
        tracks.erase(
            std::remove_if(tracks.begin(), tracks.end(),
                           [this](const TrackEntry &t)
                           { return t.miss > tracker_max_miss; }),
            tracks.end());
    }
};

/* ==================== Tracker 公有接口 ==================== */
Tracker::Tracker(float tracker_iou_thresh, int tracker_max_miss, int tracker_min_hits)
    : impl_(std::make_unique<Impl>())
{
    impl_->tracker_iou_thresh = tracker_iou_thresh;
    impl_->tracker_max_miss = tracker_max_miss;
    impl_->tracker_min_hits = tracker_min_hits;
}

Tracker::~Tracker() = default;

void Tracker::update(std::vector<AlgoResult> &detections)
{
    impl_->update(detections);
}

void Tracker::reset()
{
    impl_->tracks.clear();
    impl_->next_track_id = 1;
}

void Tracker::setTrackerIoUThresh(float thresh)
{
    impl_->tracker_iou_thresh = thresh;
}

void Tracker::setTrackerMaxMiss(int max_miss)
{
    impl_->tracker_max_miss = max_miss;
}
