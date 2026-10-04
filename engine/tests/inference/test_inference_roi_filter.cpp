#include "config/config.h"
#include "inference/inference_roi_filter.h"
#include "inference/inference_roi_geometry.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

namespace {
AlgoResult detection(int x, int y, int width, int height) {
  AlgoResult result;
  result.box = cv::Rect(x, y, width, height);
  return result;
}

InferenceRoiConfig rectangle_roi(const cv::Rect &rect, int source_width,
                                 int source_height) {
  InferenceRoiConfig config;
  config.mode = "roi_only";
  config.x = static_cast<double>(rect.x) / source_width;
  config.y = static_cast<double>(rect.y) / source_height;
  config.width = static_cast<double>(rect.width) / source_width;
  config.height = static_cast<double>(rect.height) / source_height;
  const double right = config.x + config.width;
  const double bottom = config.y + config.height;
  config.polygon = {{config.x, config.y}, {right, config.y},
                    {right, bottom}, {config.x, bottom}};
  return config;
}

void test_keeps_only_centers_inside_roi() {
  std::vector<AlgoResult> results = {
      detection(10, 10, 10, 10), // outside
      detection(25, 25, 10, 10), // center (30, 30), inside
      detection(66, 66, 10, 10), // center (71, 71), outside
  };

  filter_results_to_inference_roi(results, rectangle_roi(cv::Rect(20, 20, 50, 50), 100, 100),
                                  100, 100);

  assert(results.size() == 1);
  assert(results.front().box == cv::Rect(25, 25, 10, 10));
}

void test_scales_source_roi_to_canonical_coordinates() {
  std::vector<AlgoResult> results = {
      detection(49, 49, 2,
                2), // canonical center (50, 50) -> source center (100, 50)
      detection(9, 9, 2, 2),
  };

  filter_results_to_inference_roi(results, rectangle_roi(cv::Rect(80, 40, 40, 20), 200, 100),
                                  100, 100);

  assert(results.size() == 1);
  assert(results.front().box == cv::Rect(49, 49, 2, 2));
}

void test_clips_and_reattaches_segmentation_mask() {
  std::vector<AlgoResult> results = {
      detection(0, 0, 10, 10),
      detection(40, 40, 10, 10),
  };
  results.front().boxMask = cv::Mat(100, 100, CV_8UC1, cv::Scalar(7));

  filter_results_to_inference_roi(results, rectangle_roi(cv::Rect(25, 25, 50, 50), 100, 100),
                                  100, 100);

  assert(results.size() == 1);
  assert(!results.front().boxMask.empty());
  assert(results.front().boxMask.at<unsigned char>(50, 50) == 7);
  assert(results.front().boxMask.at<unsigned char>(10, 10) == 0);
  assert(results.front().boxMask.at<unsigned char>(90, 90) == 0);
}

void test_config_recognizes_full_frame_filter_mode() {
  InferenceRoiConfig config;
  config.mode = "full_frame_roi_filter";

  assert(config.has_roi());
  assert(config.full_frame_roi_filter());
  assert(config.restricts_results_to_roi());
  assert(!config.roi_only());
  assert(!config.full_plus_roi());
}

void test_rectangle_uses_the_polygon_schema_and_fast_path() {
  InferenceRoiConfig config;
  config.mode = "roi_only";
  config.x = 0.2;
  config.y = 0.1;
  config.width = 0.6;
  config.height = 0.7;
  config.polygon = {{0.2, 0.1}, {0.8, 0.1}, {0.8, 0.8}, {0.2, 0.8}};

  assert(config.has_polygon());
  assert(config.is_axis_aligned_rectangle());

  config.polygon[1].second = 0.2;
  assert(!config.is_axis_aligned_rectangle());
}

void test_polygon_filters_centers_and_clips_mask() {
  InferenceRoiConfig config;
  config.mode = "roi_only";
  config.polygon = {{0.1, 0.1}, {0.9, 0.1}, {0.5, 0.9}};
  std::vector<AlgoResult> results = {
      detection(45, 35, 10, 10), // triangle center
      detection(10, 75, 10, 10), // bounding box inside, triangle outside
  };
  results.front().boxMask = cv::Mat(100, 100, CV_8UC1, cv::Scalar(9));

  filter_results_to_inference_roi(results, config, 100, 100);

  assert(results.size() == 1);
  assert(results.front().boxMask.at<unsigned char>(40, 50) == 9);
  assert(results.front().boxMask.at<unsigned char>(80, 10) == 0);
}

void test_resize_transforms() {
  const cv::Rect selection(100, 100, 800, 200);
  const auto stretch = make_inference_roi_transform(selection, 1920, 1080, 640,
                                                    640, "stretch", true);
  assert(stretch.source_rect == selection);
  assert(stretch.model_content_rect == cv::Rect(0, 0, 640, 640));

  const auto letterbox = make_inference_roi_transform(
      selection, 1920, 1080, 640, 640, "letterbox", true);
  assert(letterbox.source_rect == selection);
  assert(letterbox.model_content_rect == cv::Rect(0, 240, 640, 160));

  const auto expanded = make_inference_roi_transform(selection, 1920, 1080, 640,
                                                     640, "expand", true);
  assert(expanded.source_rect.width == expanded.source_rect.height);
  assert(expanded.source_rect.x <= selection.x &&
         expanded.source_rect.y <= selection.y);
  assert(expanded.source_rect.x + expanded.source_rect.width >=
         selection.x + selection.width);
  assert(expanded.source_rect.y + expanded.source_rect.height >=
         selection.y + selection.height);
  assert(expanded.model_content_rect == cv::Rect(0, 0, 640, 640));

  /* 宽 ROI 无法在 16:9 原图中扩成正方形时，只让高度触边，并保留横向局部；
   * 旧实现会错误退化为 1920x1080 整帧。 */
  const cv::Rect wide_selection(200, 300, 1400, 300);
  const auto wide = make_inference_roi_transform(wide_selection, 1920, 1080,
                                                 640, 640, "expand", true);
  assert(wide.source_rect == cv::Rect(200, 0, 1400, 1080));
  assert(wide.source_rect != cv::Rect(0, 0, 1920, 1080));
  assert(wide.model_content_rect == cv::Rect(0, 73, 640, 494));

  /* 靠边 ROI 应平移扩展框，而不是裁掉用户选区。 */
  const cv::Rect edge_selection(0, 50, 400, 800);
  const auto edge = make_inference_roi_transform(edge_selection, 1920, 1080,
                                                 640, 640, "expand", true);
  assert(edge.source_rect == cv::Rect(0, 50, 800, 800));
  assert((edge.source_rect & edge_selection) == edge_selection);

  for (const std::string mode : {"stretch", "expand", "letterbox"}) {
    const cv::Rect corner_selection(1710, 890, 210, 190);
    const auto transform = make_inference_roi_transform(
        corner_selection, 1920, 1080, 640, 640, mode, true);
    assert(transform.valid());
    assert((transform.source_rect & cv::Rect(0, 0, 1920, 1080)) ==
           transform.source_rect);
    assert((transform.source_rect & corner_selection) == corner_selection);
    assert((transform.model_content_rect & cv::Rect(0, 0, 640, 640)) ==
           transform.model_content_rect);
  }
}

void test_empty_padding_detection_is_not_retained() {
  std::vector<AlgoResult> results = {detection(0, 0, 0, 0),
                                     detection(40, 40, 10, 10)};
  filter_results_to_inference_roi(results, rectangle_roi(cv::Rect(0, 0, 100, 100), 100, 100),
                                  100, 100);
  assert(results.size() == 1);
  assert(results.front().box == cv::Rect(40, 40, 10, 10));
}

void test_polygon_input_masks_pixels_outside_polygon() {
  cv::Mat source(100, 200, CV_8UC3, cv::Scalar(10, 20, 30));
  const auto transform = make_inference_roi_transform(
      cv::Rect(20, 20, 160, 60), source.cols, source.rows, 100, 100,
      "letterbox", true);
  const std::vector<std::pair<double, double>> polygon = {
      {0.1, 0.2}, {0.9, 0.2}, {0.5, 0.8}};
  const cv::Mat input = make_inference_roi_input(source, transform, 100, 100,
                                                  &polygon);

  assert(input.size() == cv::Size(100, 100));
  assert(input.at<cv::Vec3b>(50, 50) == cv::Vec3b(10, 20, 30));
  assert(input.at<cv::Vec3b>(25, 10) == cv::Vec3b(0, 0, 0));
  assert(input.at<cv::Vec3b>(5, 50) == cv::Vec3b(0, 0, 0));
}

void test_stretch_polygon_mask_and_expand_context_are_distinct() {
  cv::Mat source(120, 200, CV_8UC3, cv::Scalar(40, 50, 60));
  const cv::Rect selection(60, 30, 80, 60);
  const std::vector<std::pair<double, double>> polygon = {
      {0.3, 0.25}, {0.7, 0.25}, {0.5, 0.75}};

  const auto stretch = make_inference_roi_transform(
      selection, source.cols, source.rows, 100, 100, "stretch", true);
  const cv::Mat masked = make_inference_roi_input(source, stretch, 100, 100,
                                                   &polygon);
  assert(masked.at<cv::Vec3b>(50, 50) == cv::Vec3b(40, 50, 60));
  assert(masked.at<cv::Vec3b>(90, 10) == cv::Vec3b(0, 0, 0));

  const auto expand = make_inference_roi_transform(
      selection, source.cols, source.rows, 100, 100, "expand", true);
  const cv::Mat context = make_inference_roi_input(source, expand, 100, 100);
  assert(context.at<cv::Vec3b>(50, 10) == cv::Vec3b(40, 50, 60));
}

} // namespace

int main() {
  test_keeps_only_centers_inside_roi();
  test_scales_source_roi_to_canonical_coordinates();
  test_clips_and_reattaches_segmentation_mask();
  test_config_recognizes_full_frame_filter_mode();
  test_rectangle_uses_the_polygon_schema_and_fast_path();
  test_polygon_filters_centers_and_clips_mask();
  test_resize_transforms();
  test_empty_padding_detection_is_not_retained();
  test_polygon_input_masks_pixels_outside_polygon();
  test_stretch_polygon_mask_and_expand_context_are_distinct();
  std::cout << "inference ROI filter tests passed\n";
  return 0;
}
