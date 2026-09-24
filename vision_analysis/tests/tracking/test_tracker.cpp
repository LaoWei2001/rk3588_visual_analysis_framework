#include "tracking/tracker.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{

AlgoResult detection(int x, int y = 20, int width = 40, int height = 40, int class_id = 0)
{
    AlgoResult result;
    result.box = cv::Rect(x, y, width, height);
    result.class_id = class_id;
    result.model_id = "detector";
    result.model_type = "yolov8_det";
    result.score = 0.9f;
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
        if (!check((expr), #expr, __func__, __LINE__))                                                                \
            return false;                                                                                              \
    } while (false)

bool confirms_after_consecutive_hits_and_recovers_fast_motion()
{
    Tracker tracker(0.3f, 5, 3);

    std::vector<AlgoResult> frame{detection(0)};
    tracker.update(frame);
    CHECK(frame[0].track_id == -1);

    frame = {detection(8)};
    tracker.update(frame);
    CHECK(frame[0].track_id == -1);

    frame = {detection(16)};
    tracker.update(frame);
    CHECK(frame[0].track_id > 0);
    const int stable_id = frame[0].track_id;

    /* 与常规预测框已无 IoU，但仍在已确认轨迹的运动/尺寸门控内。 */
    frame = {detection(70)};
    tracker.update(frame);
    CHECK(frame[0].track_id == stable_id);
    CHECK(frame[0].track_hits >= 4);
    return true;
}

bool preserves_ids_when_same_class_targets_cross()
{
    Tracker tracker(0.3f, 5, 1);

    std::vector<AlgoResult> frame{detection(0, 20, 50, 50), detection(100, 20, 50, 50)};
    tracker.update(frame);
    const int left_to_right_id = frame[0].track_id;
    const int right_to_left_id = frame[1].track_id;
    CHECK(left_to_right_id > 0);
    CHECK(right_to_left_id > 0);
    CHECK(left_to_right_id != right_to_left_id);

    frame = {detection(20, 20, 50, 50), detection(80, 20, 50, 50)};
    tracker.update(frame);
    CHECK(frame[0].track_id == left_to_right_id);
    CHECK(frame[1].track_id == right_to_left_id);

    frame = {detection(40, 20, 50, 50), detection(60, 20, 50, 50)};
    tracker.update(frame);
    CHECK(frame[0].track_id == left_to_right_id);
    CHECK(frame[1].track_id == right_to_left_id);

    /* 检测顺序刻意反转；ID 应跟随运动方向，而不是 vector 下标。 */
    frame = {detection(60, 20, 50, 50), detection(40, 20, 50, 50)};
    tracker.update(frame);
    CHECK(frame[0].track_id == left_to_right_id);
    CHECK(frame[1].track_id == right_to_left_id);
    return true;
}

bool removes_interrupted_tentative_track()
{
    Tracker tracker(0.3f, 5, 3);

    std::vector<AlgoResult> frame{detection(10)};
    tracker.update(frame);
    CHECK(frame[0].track_id == -1);

    std::vector<AlgoResult> empty;
    tracker.update(empty);

    frame = {detection(10)};
    tracker.update(frame);
    CHECK(frame[0].track_id == -1);
    tracker.update(frame);
    CHECK(frame[0].track_id == -1);
    tracker.update(frame);
    CHECK(frame[0].track_id == 2); /* 断帧前的 tentative ID=1 已被清理 */
    return true;
}

bool clears_stale_output_fields()
{
    Tracker tracker(0.3f, 5, 3);
    AlgoResult result = detection(0);
    result.track_id = 99;
    result.vx = 12.0f;
    result.vy = -8.0f;
    result.track_hits = 42;
    std::vector<AlgoResult> frame{result};

    tracker.update(frame);
    CHECK(frame[0].track_id == -1);
    CHECK(std::fabs(frame[0].vx) < 1e-6f);
    CHECK(std::fabs(frame[0].vy) < 1e-6f);
    CHECK(frame[0].track_hits == 0);
    return true;
}

} // namespace

int main()
{
    if (!confirms_after_consecutive_hits_and_recovers_fast_motion() ||
        !preserves_ids_when_same_class_targets_cross() || !removes_interrupted_tentative_track() ||
        !clears_stale_output_fields())
        return 1;

    std::cout << "tracker regression tests passed\n";
    return 0;
}
