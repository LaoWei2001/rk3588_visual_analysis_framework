#include <channel.h>
#include "inference/inference_result_mapping.h"
#include "inference/inference_roi_filter.h"
#include <cassert>
#include <limits>
#include <iostream>

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
    map_results_to_business_frame(1920, 1080, {cv::Rect(0,0,1920,1080), cv::Rect(0,0,640,640)}, 640, 640, results);
    assert(results[0].depth_map.data == identity);
    map_results_to_business_frame(1280, 720, {cv::Rect(320,180,640,360), cv::Rect(0,0,640,640)}, 640, 640, results);
    assert(results[0].depth_at(320,320) == 2 && results[0].depth_at(0,0) == 0);
    assert(cv::countNonZero(results[0].depth_map) == 320 * 320);
    assert(cv::countNonZero(retained != 2) == 0); // transform must not mutate published readers

    // Polygon must mask the dense plane, never drop it by its box center.
    InferenceRoiConfig roi;
    roi.mode = "full_frame_roi_filter";
    roi.polygon = {{0.25,0.25},{0.4,0.25},{0.25,0.4}};
    filter_results_to_inference_roi(results, roi, 640, 640);
    assert(results.size() == 1 && results[0].depth_at(170,170) == 2);
    assert(results[0].depth_at(320,320) == 0);

    AlgoResult base = depth;
    AlgoResult extra = depth;
    extra.depth_map = cv::Mat(640,640,CV_32FC1,cv::Scalar(0));
    extra.depth_map(cv::Rect(100,100,20,20)).setTo(3);
    results = {base, extra};
    merge_depth_results(results);
    assert(results.size() == 1 && results[0].depth_at(105,105) == 3);
    assert(results[0].depth_at(0,0) == 2 && depth.depth_at(105,105) == 2);
    // Two independent depth models remain independent.
    extra.model_id = "another";
    results.push_back(extra);
    merge_depth_results(results);
    assert(results.size() == 2);
    std::cout << "Depth SDK queries, finite values, ROI mapping/filtering and full+ROI merge: PASS\n";
}
