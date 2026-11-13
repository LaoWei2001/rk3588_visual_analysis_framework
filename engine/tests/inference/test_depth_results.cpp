#include "vision/inference/inference_result_mapping.h"
#include "vision/inference/inference_roi_filter.h"
#include "vision/models/zip-depth/depth_resize.h"
#include <cassert>
#include <channel.h>
#include <iostream>
#include <limits>

int main()
{
    AlgoResult depth;
    depth.depth_map = cv::Mat(640, 640, CV_32FC1, cv::Scalar(2));
    depth.box = cv::Rect(0, 0, 640, 640);
    depth.model_id = "distance";
    depth.model_type = "yolo26_depth";
    depth.label = "depth";
    depth.score = 1.0f;
    assert(depth.depth_at(20, 10) == 2);
    assert(depth.depth_at(-1, 0) == 0 && depth.depth_at(640, 0) == 0);
    assert(depth.depth_median(cv::Rect(-50, -50, 100, 100)) == 2);
    assert(depth.depth_median(cv::Rect(0, 0, 100, 100), 0) == 0);
    assert(!AlgoResult::valid_depth(std::numeric_limits<float>::infinity()));
    assert(!AlgoResult::valid_depth(std::numeric_limits<float>::quiet_NaN()));
    assert(!AlgoResult::valid_depth(-1));
    AlgoResult small;
    small.depth_map = (cv::Mat_<float>(1, 4) << 1, 4, 2, 3);
    assert(small.depth_median(cv::Rect(0, 0, 4, 1), 1) == 2.5f);
    const cv::Mat retained = depth.depth_map;
    std::vector<AlgoResult> results{depth};
    ChannelContext context{};
    context.results = &results;
    context.infer_enabled = true;
    context.inference_valid = true;
    assert(context.depth_result("distance") == &results[0]);
    assert(context.depth_result("missing") == nullptr);
    assert(context.target_count("depth") == 0);
    assert(context.query_targets().valid() && context.query_targets().count == 0);

    const auto *identity = results[0].depth_map.data;
    map_results_to_business_frame(1920, 1080, {cv::Rect(0, 0, 1920, 1080), cv::Rect(0, 0, 640, 640)}, 640, 640,
                                  results);
    assert(results[0].depth_map.data == identity);
    map_results_to_business_frame(1280, 720, {cv::Rect(320, 180, 640, 360), cv::Rect(0, 0, 640, 640)}, 640, 640,
                                  results);
    assert(results[0].depth_at(320, 320) == 2 && results[0].depth_at(0, 0) == 0);
    assert(cv::countNonZero(results[0].depth_map) == 320 * 320);
    assert(cv::countNonZero(retained != 2) == 0); // transform must not mutate published readers

    // Polygon must mask the dense plane, never drop it by its box center.
    InferenceRoiConfig roi;
    roi.mode = "full_frame_roi_filter";
    roi.polygon = {{0.25, 0.25}, {0.4, 0.25}, {0.25, 0.4}};
    filter_results_to_inference_roi(results, roi, 640, 640);
    assert(results.size() == 1 && results[0].depth_at(170, 170) == 2);
    assert(results[0].depth_at(320, 320) == 0);

    AlgoResult base = depth;
    AlgoResult extra = depth;
    extra.depth_map = cv::Mat(640, 640, CV_32FC1, cv::Scalar(0));
    extra.depth_map(cv::Rect(100, 100, 20, 20)).setTo(3);
    results = {base, extra};
    merge_depth_results(results);
    assert(results.size() == 1 && results[0].depth_at(105, 105) == 3);
    assert(results[0].depth_at(0, 0) == 2 && depth.depth_at(105, 105) == 2);
    // Two independent depth models remain independent.
    extra.model_id = "another";
    results.push_back(extra);
    merge_depth_results(results);
    assert(results.size() == 2);

    // Relative values may be zero or negative. Invalid ROI pixels are NaN.
    AlgoResult relative;
    relative.depth_kind = DepthKind::RelativeInverse;
    relative.depth_map = (cv::Mat_<float>(2, 2) << -2, 0, 2, 4);
    assert(relative.is_relative_inverse_depth() && !relative.is_metric_depth());
    assert(relative.depth_valid_at(1, 0) && relative.depth_at(0, 0) == -2);
    assert(relative.depth_median(cv::Rect(0, 0, 2, 2), 1) == 1);
    assert(!relative.valid_depth_value(std::numeric_limits<float>::infinity()));
    cv::Mat resized;
    zip_depth::resize_depth(relative.depth_map, resized, cv::Size(3, 3));
    assert(resized.at<float>(1, 1) == 1 && resized.at<float>(2, 2) == 4);
    zip_depth::resize_depth(relative.depth_map, resized, cv::Size(1, 1));
    assert(resized.at<float>(0, 0) == -2);
    cv::Mat single = (cv::Mat_<float>(1, 2) << 0, 4);
    zip_depth::resize_depth(single, resized, cv::Size(5, 3));
    assert(resized.at<float>(2, 1) == 1 && resized.at<float>(0, 4) == 4);

    for (const auto size : {cv::Size(384, 384), cv::Size(672, 384), cv::Size(768, 768)})
    {
        // Source gradient has a closed-form reference for endpoint alignment.
        relative.depth_map.create(size, CV_32FC1);
        for (int y = 0; y < size.height; ++y)
            for (int x = 0; x < size.width; ++x)
                relative.depth_map.at<float>(y, x) = float(x) / (size.width - 1) + float(y) / (size.height - 1);
        relative.box = cv::Rect(0, 0, size.width, size.height);
        results = {relative};
        map_results_to_business_frame(1920, 1080, {cv::Rect(0, 0, 1920, 1080), cv::Rect(0, 0, size.width, size.height)},
                                      size.width, size.height, results);
        assert(results[0].depth_map.size() == cv::Size(640, 640));
        assert(std::abs(results[0].depth_at(137, 291) - (137.f + 291.f) / 639) < 1e-6);
        assert(results[0].depth_valid_at(0, 0));

        results = {relative};
        map_results_to_business_frame(1920, 1080,
                                      {cv::Rect(480, 270, 960, 540), cv::Rect(0, 0, size.width, size.height)},
                                      size.width, size.height, results);
        assert(!results[0].depth_valid_at(0, 0));
        assert(results[0].depth_valid_at(160, 160) && results[0].depth_at(160, 160) == 0);
        assert(std::abs(results[0].depth_at(479, 479) - 2) < 1e-6);
        filter_results_to_inference_roi(results, roi, 640, 640);
        assert(results[0].depth_valid_at(170, 170));
        assert(!results[0].depth_valid_at(320, 320));
    }
    results = {relative, relative};
    merge_depth_results(results);
    assert(results.size() == 2); // never blend independently normalized predictions
    std::cout << "Depth SDK queries, finite values, ROI mapping/filtering and full+ROI merge: PASS\n";
}
