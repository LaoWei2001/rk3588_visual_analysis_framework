#include "yolo/yolo26pose.h"

#include <cstdio>
#include <exception>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    fprintf(stderr, "usage: %s MODEL.rknn IMAGE [OUTPUT.jpg]\n", argv[0]);
    return 2;
  }

  try {
    cv::Mat image = cv::imread(argv[2], cv::IMREAD_COLOR);
    if (image.empty()) {
      fprintf(stderr, "cannot read image: %s\n", argv[2]);
      return 2;
    }

    Yolo26Pose model(argv[1], "", RKNN_NPU_CORE_AUTO, 0.25f, 0.45f);
    std::vector<AlgoResult> results;
    YoloPerfStat perf;
    if (!model.infer(image, results, &perf)) {
      fprintf(stderr, "Yolo26Pose::infer failed\n");
      return 1;
    }

    printf("detections=%zu preprocess=%.2fms inference=%.2fms "
           "postprocess=%.2fms\n",
           results.size(), perf.preprocess_ms, perf.infer_ms,
           perf.postprocess_ms);
    for (size_t i = 0; i < results.size(); ++i) {
      const AlgoResult &result = results[i];
      printf("  #%zu score=%.3f box=[%d,%d,%d,%d] keypoints=%zu\n", i,
             result.score, result.box.x, result.box.y, result.box.width,
             result.box.height, result.keypoints.size());
      if (result.keypoints.size() != 17 ||
          result.keypoint_scores.size() != 17) {
        fprintf(stderr, "unexpected keypoint count\n");
        return 1;
      }
      cv::rectangle(image, result.box, cv::Scalar(60, 220, 60), 2, cv::LINE_AA);
      for (size_t keypoint = 0; keypoint < result.keypoints.size(); ++keypoint)
        if (result.keypoint_scores[keypoint] >= 0.5f)
          cv::circle(image, result.keypoints[keypoint], 4,
                     cv::Scalar(0, 255, 255), -1, cv::LINE_AA);
    }

    if (results.empty()) {
      fprintf(stderr, "model returned no detections\n");
      return 1;
    }
    if (argc == 4 && !cv::imwrite(argv[3], image)) {
      fprintf(stderr, "cannot write output: %s\n", argv[3]);
      return 1;
    }
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "exception: %s\n", error.what());
    return 1;
  }
}
