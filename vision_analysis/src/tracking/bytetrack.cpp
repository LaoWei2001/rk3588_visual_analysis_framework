/**
 * @file bytetrack.cpp
 * @brief ByteTrack 的纯 C++/OpenCV 实现
 *
 * 关联顺序：
 *  1. 已确认/短时丢失轨迹与高置信度检测关联；
 *  2. 第一轮未匹配、且上一帧仍活跃的轨迹与低置信度检测关联；
 *  3. 暂定轨迹只与剩余高置信度检测关联；
 *  4. 仅剩余高置信度检测可创建新轨迹。
 */
#include "bytetrack.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <opencv2/video/tracking.hpp>
#include <utility>

namespace
{

static float clamp01(float value)
{
    return std::max(0.0f, std::min(value, 1.0f));
}

static float rect_iou(const cv::Rect_<float> &a, const cv::Rect &b)
{
    if (a.width <= 0.0f || a.height <= 0.0f || b.width <= 0 || b.height <= 0)
        return 0.0f;
    const float left = std::max(a.x, static_cast<float>(b.x));
    const float top = std::max(a.y, static_cast<float>(b.y));
    const float right = std::min(a.x + a.width, static_cast<float>(b.x + b.width));
    const float bottom = std::min(a.y + a.height, static_cast<float>(b.y + b.height));
    const float intersection = std::max(0.0f, right - left) * std::max(0.0f, bottom - top);
    const float union_area = a.width * a.height + static_cast<float>(b.width * b.height) - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

/* O(n^3) Kuhn-Munkres 最小代价分配，支持非方阵。 */
static void hungarian_solve(const std::vector<std::vector<float>> &cost, std::vector<int> &assignment)
{
    const int rows = static_cast<int>(cost.size());
    if (rows == 0)
    {
        assignment.clear();
        return;
    }
    const int cols = static_cast<int>(cost.front().size());
    if (cols == 0)
    {
        assignment.assign(rows, -1);
        return;
    }

    const int n = std::max(rows, cols);
    const float big = 1e9f;
    std::vector<std::vector<float>> square(n + 1, std::vector<float>(n + 1, 0.0f));
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j)
            square[i][j] = cost[i - 1][j - 1];

    std::vector<float> u(n + 1, 0.0f), v(n + 1, 0.0f);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    for (int i = 1; i <= n; ++i)
    {
        p[0] = i;
        int j0 = 0;
        std::vector<float> minv(n + 1, big);
        std::vector<unsigned char> used(n + 1, 0);
        do
        {
            used[j0] = 1;
            const int i0 = p[j0];
            float delta = big;
            int j1 = 0;
            for (int j = 1; j <= n; ++j)
            {
                if (used[j])
                    continue;
                const float current = square[i0][j] - u[i0] - v[j];
                if (current < minv[j])
                {
                    minv[j] = current;
                    way[j] = j0;
                }
                if (minv[j] < delta)
                {
                    delta = minv[j];
                    j1 = j;
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
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    assignment.assign(rows, -1);
    for (int j = 1; j <= n; ++j)
        if (p[j] > 0 && p[j] <= rows && j <= cols)
            assignment[p[j] - 1] = j - 1;
}

static void box_to_state(const cv::Rect &box, float &cx, float &cy, float &area, float &ratio)
{
    const float width = static_cast<float>(box.width);
    const float height = static_cast<float>(box.height);
    cx = static_cast<float>(box.x) + width * 0.5f;
    cy = static_cast<float>(box.y) + height * 0.5f;
    area = width * height;
    ratio = height > 0.0f ? width / height : 1.0f;
}

static cv::Rect_<float> state_to_box(float cx, float cy, float area, float ratio)
{
    area = std::max(area, 1.0f);
    ratio = std::max(ratio, 0.01f);
    const float width = std::sqrt(area * ratio);
    const float height = area / width;
    return cv::Rect_<float>(cx - width * 0.5f, cy - height * 0.5f, width, height);
}

static cv::KalmanFilter make_kalman_filter(const cv::Rect &box)
{
    cv::KalmanFilter filter(7, 4, 0, CV_32F);
    filter.transitionMatrix = cv::Mat::eye(7, 7, CV_32F);
    filter.transitionMatrix.at<float>(0, 4) = 1.0f;
    filter.transitionMatrix.at<float>(1, 5) = 1.0f;
    filter.transitionMatrix.at<float>(2, 6) = 1.0f;

    filter.measurementMatrix = cv::Mat::zeros(4, 7, CV_32F);
    for (int i = 0; i < 4; ++i)
        filter.measurementMatrix.at<float>(i, i) = 1.0f;

    cv::setIdentity(filter.processNoiseCov);
    filter.processNoiseCov.at<float>(4, 4) = 0.01f;
    filter.processNoiseCov.at<float>(5, 5) = 0.01f;
    filter.processNoiseCov.at<float>(6, 6) = 0.0001f;
    cv::setIdentity(filter.measurementNoiseCov);
    filter.measurementNoiseCov.at<float>(0, 0) = 0.1f;
    filter.measurementNoiseCov.at<float>(1, 1) = 0.1f;
    filter.measurementNoiseCov.at<float>(2, 2) = 10.0f;
    filter.measurementNoiseCov.at<float>(3, 3) = 10.0f;
    cv::setIdentity(filter.errorCovPost, cv::Scalar::all(10.0f));
    filter.errorCovPost.at<float>(4, 4) = 10000.0f;
    filter.errorCovPost.at<float>(5, 5) = 10000.0f;
    filter.errorCovPost.at<float>(6, 6) = 10000.0f;

    float cx = 0.0f, cy = 0.0f, area = 1.0f, ratio = 1.0f;
    box_to_state(box, cx, cy, area, ratio);
    filter.statePost = cv::Mat::zeros(7, 1, CV_32F);
    filter.statePost.at<float>(0) = cx;
    filter.statePost.at<float>(1) = cy;
    filter.statePost.at<float>(2) = area;
    filter.statePost.at<float>(3) = ratio;
    return filter;
}

struct ByteTrackEntry
{
    int id = -1;
    std::string model_id;
    std::string model_type;
    int class_id = -1;
    cv::KalmanFilter filter;
    cv::Rect_<float> predicted_box;
    int miss = 0;
    int hits = 0;
    int total_hits = 0;
    bool confirmed = false;
};

} // namespace

struct ByteTracker::Impl
{
    float high_iou_thresh = 0.3f;
    float low_iou_thresh = 0.2f;
    float low_score_thresh = 0.1f;
    float default_high_score_thresh = 0.3f;
    int max_miss = 30;
    int min_hits = 3;
    int next_track_id = 1;
    std::unordered_map<std::string, float> model_high_thresholds;
    std::vector<ByteTrackEntry> tracks;

    float high_score_threshold(const AlgoResult &detection) const
    {
        const auto it = model_high_thresholds.find(detection.model_id);
        return it == model_high_thresholds.end() ? default_high_score_thresh : it->second;
    }

    void predict()
    {
        for (auto &track : tracks)
        {
            const cv::Mat state = track.filter.predict();
            track.predicted_box =
                state_to_box(state.at<float>(0), state.at<float>(1), state.at<float>(2), state.at<float>(3));
            ++track.miss;
        }
    }

    bool compatible(size_t track_index, const AlgoResult &detection) const
    {
        const ByteTrackEntry &track = tracks[track_index];
        return detection.box.width > 0 && detection.box.height > 0 && track.model_id == detection.model_id &&
               track.model_type == detection.model_type && track.class_id == detection.class_id;
    }

    void associate(const std::vector<size_t> &track_indices, const std::vector<size_t> &detection_indices,
                   const std::vector<AlgoResult> &detections, float iou_threshold, std::vector<int> &track_assignment,
                   std::vector<unsigned char> &detection_matched) const
    {
        if (track_indices.empty() || detection_indices.empty())
            return;
        const size_t rows = track_indices.size();
        const size_t columns = detection_indices.size();
        const float tie_scale = 1.0f / static_cast<float>(std::min(rows, columns) + 1U);
        std::vector<std::vector<float>> cost(rows, std::vector<float>(columns, 0.0f));
        for (size_t row = 0; row < rows; ++row)
        {
            for (size_t column = 0; column < columns; ++column)
            {
                const size_t track_index = track_indices[row];
                const size_t detection_index = detection_indices[column];
                if (!compatible(track_index, detections[detection_index]))
                    continue;
                const float iou = rect_iou(tracks[track_index].predicted_box, detections[detection_index].box);
                if (iou >= iou_threshold)
                    cost[row][column] = -1.0f + tie_scale * (1.0f - iou);
            }
        }

        std::vector<int> local_assignment;
        hungarian_solve(cost, local_assignment);
        for (size_t row = 0; row < rows; ++row)
        {
            const int column = local_assignment[row];
            if (column < 0 || column >= static_cast<int>(columns) || cost[row][column] >= 0.0f)
                continue;
            const size_t track_index = track_indices[row];
            const size_t detection_index = detection_indices[static_cast<size_t>(column)];
            track_assignment[track_index] = static_cast<int>(detection_index);
            detection_matched[detection_index] = 1;
        }
    }

    void correct_track(size_t track_index, AlgoResult &detection, bool allow_confirmation)
    {
        ByteTrackEntry &track = tracks[track_index];
        float cx = 0.0f, cy = 0.0f, area = 1.0f, ratio = 1.0f;
        box_to_state(detection.box, cx, cy, area, ratio);
        const cv::Mat measurement = (cv::Mat_<float>(4, 1) << cx, cy, area, ratio);
        const cv::Mat estimate = track.filter.correct(measurement);
        track.predicted_box =
            state_to_box(estimate.at<float>(0), estimate.at<float>(1), estimate.at<float>(2), estimate.at<float>(3));
        track.miss = 0;
        ++track.hits;
        ++track.total_hits;
        if (allow_confirmation && !track.confirmed && track.hits >= min_hits)
        {
            track.confirmed = true;
            track.id = next_track_id++;
        }
        if (track.confirmed)
        {
            detection.track_id = track.id;
            detection.vx = estimate.at<float>(4);
            detection.vy = estimate.at<float>(5);
            detection.track_hits = track.hits;
        }
    }

    void create_track(AlgoResult &detection)
    {
        ByteTrackEntry track;
        track.model_id = detection.model_id;
        track.model_type = detection.model_type;
        track.class_id = detection.class_id;
        track.filter = make_kalman_filter(detection.box);
        track.predicted_box =
            cv::Rect_<float>(static_cast<float>(detection.box.x), static_cast<float>(detection.box.y),
                             static_cast<float>(detection.box.width), static_cast<float>(detection.box.height));
        track.hits = 1;
        track.total_hits = 1;
        track.confirmed = min_hits <= 1;
        if (track.confirmed)
        {
            track.id = next_track_id++;
            detection.track_id = track.id;
            detection.track_hits = 1;
        }
        tracks.push_back(std::move(track));
    }

    void prune()
    {
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [this](const ByteTrackEntry &track) {
                                        return track.confirmed ? track.miss > max_miss : track.miss > 0;
                                    }),
                     tracks.end());
    }

    void update(std::vector<AlgoResult> &detections)
    {
        for (auto &detection : detections)
        {
            detection.track_id = -1;
            detection.vx = 0.0f;
            detection.vy = 0.0f;
            detection.track_hits = 0;
        }

        predict();
        const size_t track_count = tracks.size();
        const size_t detection_count = detections.size();
        std::vector<int> track_assignment(track_count, -1);
        std::vector<unsigned char> detection_matched(detection_count, 0);
        std::vector<unsigned char> keep_detection(detection_count, 0);
        std::vector<size_t> high_detections;
        std::vector<size_t> low_detections;
        high_detections.reserve(detection_count);
        low_detections.reserve(detection_count);

        for (size_t i = 0; i < detection_count; ++i)
        {
            const AlgoResult &detection = detections[i];
            if (!std::isfinite(detection.score) || detection.box.width <= 0 || detection.box.height <= 0)
                continue;
            const float high_threshold = high_score_threshold(detection);
            if (detection.score >= high_threshold)
            {
                high_detections.push_back(i);
                keep_detection[i] = 1;
            }
            else if (detection.score >= low_score_thresh)
            {
                low_detections.push_back(i);
            }
        }

        std::vector<size_t> confirmed_tracks;
        std::vector<size_t> tentative_tracks;
        for (size_t i = 0; i < track_count; ++i)
            (tracks[i].confirmed ? confirmed_tracks : tentative_tracks).push_back(i);

        /* 第一轮：高置信度框可以重激活 max_miss 窗口内的已确认轨迹。 */
        associate(confirmed_tracks, high_detections, detections, high_iou_thresh, track_assignment, detection_matched);

        /* 第二轮：低分框只续接上一帧仍活跃的已确认轨迹，不重激活已经丢失的轨迹。 */
        std::vector<size_t> active_unmatched_tracks;
        for (size_t index : confirmed_tracks)
            if (track_assignment[index] < 0 && tracks[index].miss == 1)
                active_unmatched_tracks.push_back(index);
        associate(active_unmatched_tracks, low_detections, detections, low_iou_thresh, track_assignment,
                  detection_matched);

        /* 暂定轨迹只能由高置信度框连续确认。 */
        std::vector<size_t> remaining_high;
        for (size_t index : high_detections)
            if (!detection_matched[index])
                remaining_high.push_back(index);
        associate(tentative_tracks, remaining_high, detections, high_iou_thresh, track_assignment, detection_matched);

        for (size_t track_index = 0; track_index < track_count; ++track_index)
        {
            const int detection_index = track_assignment[track_index];
            if (detection_index < 0)
            {
                tracks[track_index].hits = 0;
                continue;
            }
            const bool high_match = keep_detection[static_cast<size_t>(detection_index)] != 0;
            correct_track(track_index, detections[static_cast<size_t>(detection_index)], high_match);
            if (!high_match && tracks[track_index].confirmed)
                keep_detection[static_cast<size_t>(detection_index)] = 1;
        }

        /* ByteTrack 的核心约束：低分检测永远不能创建轨迹。 */
        for (size_t detection_index : high_detections)
            if (!detection_matched[detection_index])
                create_track(detections[detection_index]);

        prune();

        std::vector<AlgoResult> visible;
        visible.reserve(detections.size());
        for (size_t i = 0; i < detections.size(); ++i)
            if (keep_detection[i])
                visible.push_back(std::move(detections[i]));
        detections.swap(visible);
    }
};

ByteTracker::ByteTracker(float high_iou_thresh, float low_iou_thresh, float low_score_thresh, int tracker_max_miss,
                         int tracker_min_hits, float default_high_score_thresh)
    : impl_(std::make_unique<Impl>())
{
    impl_->high_iou_thresh = clamp01(high_iou_thresh);
    impl_->low_iou_thresh = clamp01(low_iou_thresh);
    impl_->low_score_thresh = clamp01(low_score_thresh);
    impl_->max_miss = std::max(1, tracker_max_miss);
    impl_->min_hits = std::max(1, tracker_min_hits);
    impl_->default_high_score_thresh = clamp01(default_high_score_thresh);
}

ByteTracker::~ByteTracker() = default;

void ByteTracker::update(std::vector<AlgoResult> &detections)
{
    impl_->update(detections);
}

void ByteTracker::setModelHighThresholds(const std::unordered_map<std::string, float> &thresholds)
{
    impl_->model_high_thresholds.clear();
    for (const auto &item : thresholds)
        impl_->model_high_thresholds[item.first] = clamp01(item.second);
}

void ByteTracker::reset()
{
    impl_->tracks.clear();
    impl_->next_track_id = 1;
}
