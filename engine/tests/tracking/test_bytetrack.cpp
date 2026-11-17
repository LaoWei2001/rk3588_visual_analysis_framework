#include <bytetrack.h>

#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

AlgoResult detection(float score, int x = 20, int class_id = 0, const std::string &model_id = "detector")
{
    AlgoResult result;
    result.box = cv::Rect(x, 20, 50, 60);
    result.class_id = class_id;
    result.model_id = model_id;
    result.model_type = "yolov8_det";
    result.score = score;
    return result;
}

bool check(bool condition, const char *expression, const char *test_name, int line)
{
    if (condition)
        return true;
    std::cerr << test_name << ':' << line << ": check failed: " << expression << '\n';
    return false;
}

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!check((expr), #expr, __func__, __LINE__))                                                                 \
            return false;                                                                                              \
    } while (false)

bool low_score_detection_keeps_confirmed_track()
{
    ByteTracker tracker(0.3f, 0.2f, 0.1f, 2, 2, 0.5f);

    std::vector<AlgoResult> frame{detection(0.9f, 20)};
    tracker.update(frame);
    CHECK(frame.size() == 1);
    CHECK(frame[0].track_id == -1);

    frame = {detection(0.9f, 22)};
    tracker.update(frame);
    CHECK(frame[0].track_id == 1);

    // Low scores stay internal, even across more frames than tracker_max_miss.
    // Returning high scores must retain the ID, proving internal tracking continued.
    for (int i = 0; i < 8; ++i)
    {
        frame = {detection(0.25f, 24 + 2 * i)};
        tracker.update(frame);
        CHECK(frame.empty());
    }

    frame = {detection(0.9f, 40)};
    tracker.update(frame);
    CHECK(frame[0].track_id == 1);
    return true;
}

bool low_score_detection_never_starts_track()
{
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

bool model_thresholds_and_classes_are_isolated()
{
    ByteTracker tracker(0.3f, 0.2f, 0.1f, 10, 1, 0.3f);
    tracker.setModelHighThresholds({{"strict", 0.7f}, {"normal", 0.4f}});

    std::vector<AlgoResult> frame{detection(0.5f, 20, 0, "strict"), detection(0.5f, 120, 0, "normal")};
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

bool confirmed_tracks_obey_each_model_threshold()
{
    ByteTracker tracker(0.3f, 0.2f, 0.1f, 10, 1, 0.6f);
    tracker.setModelHighThresholds({{"strict", 0.9f}, {"normal", 0.4f}});
    std::vector<AlgoResult> frame{detection(0.95f, 20, 0, "strict"), detection(0.5f, 120, 0, "normal")};
    tracker.update(frame);
    CHECK(frame.size() == 2);
    const int strict_id = frame[0].track_id, normal_id = frame[1].track_id;
    CHECK(strict_id > 0 && normal_id > 0 && strict_id != normal_id);

    frame = {detection(0.5f, 22, 0, "strict"), detection(0.5f, 122, 0, "normal")};
    tracker.update(frame);
    CHECK(frame.size() == 1);
    CHECK(frame[0].model_id == "normal");
    CHECK(frame[0].track_id == normal_id);

    // Compare the raw score, not its two-decimal display text.
    frame = {detection(std::nextafter(0.9f, 0.f), 24, 0, "strict")};
    tracker.update(frame);
    CHECK(frame.empty());
    frame = {detection(0.9f, 26, 0, "strict")};
    tracker.update(frame);
    CHECK(frame.size() == 1);
    CHECK(frame[0].score == 0.9f && frame[0].track_id == strict_id);

    // A model without a dedicated entry obeys the default output threshold.
    frame = {detection(0.7f, 220, 0, "fallback")};
    tracker.update(frame);
    CHECK(frame.size() == 1);
    const int fallback_id = frame[0].track_id;
    frame = {detection(0.5f, 222, 0, "fallback")};
    tracker.update(frame);
    CHECK(frame.empty());
    frame = {detection(0.6f, 224, 0, "fallback")};
    tracker.update(frame);
    CHECK(frame.size() == 1 && frame[0].track_id == fallback_id);
    return true;
}

bool threshold_updates_apply_to_confirmed_tracks()
{
    ByteTracker tracker(0.3f, 0.2f, 0.1f, 10, 1, 0.5f);
    tracker.setModelHighThresholds({{"detector", 0.5f}});
    std::vector<AlgoResult> frame{detection(0.8f)};
    tracker.update(frame);
    CHECK(frame.size() == 1);
    const int id = frame[0].track_id;
    tracker.setModelHighThresholds({{"detector", 0.9f}});
    frame = {detection(0.8f)};
    tracker.update(frame);
    CHECK(frame.empty());
    tracker.setModelHighThresholds({{"detector", 0.5f}});
    frame = {detection(0.8f)};
    tracker.update(frame);
    CHECK(frame.size() == 1 && frame[0].track_id == id);
    return true;
}

bool reset_restarts_channel_local_ids()
{
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

int main()
{
    if (!low_score_detection_keeps_confirmed_track() || !low_score_detection_never_starts_track() ||
        !model_thresholds_and_classes_are_isolated() || !confirmed_tracks_obey_each_model_threshold() ||
        !threshold_updates_apply_to_confirmed_tracks() || !reset_restarts_channel_local_ids())
        return 1;
    std::cout << "bytetrack regression tests passed\n";
    return 0;
}
