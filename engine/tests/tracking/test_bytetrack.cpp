#include "tracking/bytetrack.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

AlgoResult detection(float score, int x = 20, int class_id = 0,
                     const std::string &model_id = "detector") {
  AlgoResult result;
  result.box = cv::Rect(x, 20, 50, 60);
  result.class_id = class_id;
  result.model_id = model_id;
  result.model_type = "yolov8_det";
  result.score = score;
  return result;
}

bool check(bool condition, const char *expression, const char *test_name,
           int line) {
  if (condition)
    return true;
  std::cerr << test_name << ':' << line << ": check failed: " << expression
            << '\n';
  return false;
}

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!check((expr), #expr, __func__, __LINE__))                             \
      return false;                                                            \
  } while (false)

bool low_score_detection_keeps_confirmed_track() {
  ByteTracker tracker(0.3f, 0.2f, 0.1f, 10, 2, 0.5f);

  std::vector<AlgoResult> frame{detection(0.9f, 20)};
  tracker.update(frame);
  CHECK(frame.size() == 1);
  CHECK(frame[0].track_id == -1);

  frame = {detection(0.9f, 22)};
  tracker.update(frame);
  CHECK(frame[0].track_id == 1);

  frame = {detection(0.25f, 24)};
  tracker.update(frame);
  CHECK(frame.size() == 1);
  CHECK(frame[0].track_id == 1);
  CHECK(frame[0].track_hits == 3);

  frame = {detection(0.9f, 26)};
  tracker.update(frame);
  CHECK(frame[0].track_id == 1);
  return true;
}

bool low_score_detection_never_starts_track() {
  ByteTracker tracker(0.3f, 0.2f, 0.1f, 10, 1, 0.5f);
  std::vector<AlgoResult> frame{detection(0.25f)};
  tracker.update(frame);
  CHECK(frame.empty());

  frame = {detection(0.05f)};
  tracker.update(frame);
  CHECK(frame.empty());

  frame = {detection(0.9f)};
  tracker.update(frame);
  CHECK(frame.size() == 1);
  CHECK(frame[0].track_id == 1);
  return true;
}

bool model_thresholds_and_classes_are_isolated() {
  ByteTracker tracker(0.3f, 0.2f, 0.1f, 10, 1, 0.3f);
  tracker.setModelHighThresholds({{"strict", 0.7f}, {"normal", 0.4f}});

  std::vector<AlgoResult> frame{detection(0.5f, 20, 0, "strict"),
                                detection(0.5f, 120, 0, "normal")};
  tracker.update(frame);
  CHECK(frame.size() == 1);
  CHECK(frame[0].model_id == "normal");
  CHECK(frame[0].track_id == 1);

  /* 同位置但类别不同的低分框不能续接已有轨迹。 */
  frame = {detection(0.2f, 120, 1, "normal")};
  tracker.update(frame);
  CHECK(frame.empty());
  return true;
}

bool reset_restarts_channel_local_ids() {
  ByteTracker tracker(0.3f, 0.2f, 0.1f, 10, 1, 0.5f);
  std::vector<AlgoResult> frame{detection(0.9f)};
  tracker.update(frame);
  CHECK(frame[0].track_id == 1);
  tracker.reset();
  frame = {detection(0.9f)};
  tracker.update(frame);
  CHECK(frame[0].track_id == 1);
  return true;
}

} // namespace

int main() {
  if (!low_score_detection_keeps_confirmed_track() ||
      !low_score_detection_never_starts_track() ||
      !model_thresholds_and_classes_are_isolated() ||
      !reset_restarts_channel_local_ids())
    return 1;
  std::cout << "bytetrack regression tests passed\n";
  return 0;
}
