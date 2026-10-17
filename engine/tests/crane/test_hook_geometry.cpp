#include "hook_geometry.h"
#include "hook_guard.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace crane_safety;
static const cv::Size frame(640, 640);

static HookCalibration table()
{
    HookCalibration data;
    HookCalibrationPoint a, b;
    a.width = 40; a.height = 100; a.center_x = 300; a.center_y = 150;
    b.width = 80; b.height = 200; b.center_x = 340; b.center_y = 250;
    data.points = {a, b};
    data.min_width = 38; data.max_width = 82;
    return data;
}

static HookObservation observation(cv::Point center, bool estimated = false, int track = 7)
{
    HookObservation out;
    out.visible = out.valid = true;
    out.estimated = estimated; out.center = center; out.track_id = track;
    return out;
}

static void geometry_tests()
{
    const auto data = table();
    auto full = measure_hook({290, 100, 60, 150}, frame, 5, data, true);
    assert(full.valid && !full.estimated && !full.truncated);
    assert(full.center == cv::Point(320, 175));
    assert(full.safe_center == cv::Point(320, 200));
    auto top = measure_hook({290, 0, 60, 100}, frame, 5, data, true);
    auto bottom = measure_hook({290, 540, 60, 100}, frame, 5, data, true);
    assert(top.valid && top.estimated && top.truncated && top.center == cv::Point(320, 25));
    assert(bottom.valid && bottom.estimated && bottom.center == cv::Point(320, 615));
    assert(top.safe_center == full.safe_center && bottom.safe_center == full.safe_center);
    assert(measure_hook({290, -50, 60, 150}, frame, 5, data, true).center == top.center);
    assert(measure_hook({290, 540, 60, 150}, frame, 5, data, true).center == bottom.center);
    assert(measure_hook({290, 0, 60, 50}, frame, 5, data, true).center.y == -25);
    assert(!measure_hook({290, 0, 60, 44}, frame, 5, data, true).valid);
    assert(!measure_hook({0, 100, 60, 150}, frame, 5, data, true).valid);
    assert(!measure_hook({580, 100, 60, 150}, frame, 5, data, true).valid);
    assert(!measure_hook({290, 0, 60, 640}, frame, 5, data, true).valid);
    assert(!measure_hook({290, 100, 100, 250}, frame, 5, data, true).valid);
    assert(!measure_hook({290, 100, 60, 80}, frame, 5, data, true).valid);
    assert(!measure_hook({290, 100, 60, 0}, frame, 5, data, true).valid);
    assert(!measure_hook({290, -500, 60, 100}, frame, 5, data, true).valid);
    HookCalibration empty;
    assert(!measure_hook({290, 0, 60, 100}, frame, 5, empty, false).valid);
    assert(!measure_hook({290, 100, 60, 150}, frame, 5, empty, true).valid);
    assert(measure_hook({290, 100, 60, 150}, frame, 5, empty, false).valid);
    assert(!hook_edges({5, 100, 60, 150}, frame, 5).complete());
    assert(hook_edges({6, 100, 60, 150}, frame, 5).complete());
    HookCalibrationPoint point;
    assert(data.lookup(38, &point) && point.width == 40);
    assert(!data.lookup(37.9, &point) && !data.lookup(82.1, &point));
}

static void csv_tests()
{
    char path[] = "/tmp/crane-hook-test-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    {
        std::ofstream file(path);
        file << "\xEF\xBB\xBF" "box_x,box_y,box_width,box_height,center_x,center_y,box_scale\r\n"
             << "290,100,60,150,320,175,94.8\r\n"
             << "291,102,60,150,321,177,94.8\r\n"
             << "280,100,80,200,320,200,126.5\r\n"
             << "290,0,60,100,320,50,77\r\n" // old cropped sample: reject
             << "0,100,60,150,30,175,94.8\r\n"
             << "290,100,nan,150,320,175,94.8\r\n"
             << "290,100,inf,150,320,175,94.8\r\n"
             << "290,100,60,150,320,200,94.8\r\n"
             << "290,100,-60,150,320,175,94.8\r\n"
             << "bad,row\r\n";
    }
    auto data = load_hook_calibration(path, 5, frame, 5);
    assert(data.error.empty() && data.accepted == 3 && data.rejected == 7);
    assert(data.points.size() == 2 && data.points[0].center_x == 320.5);
    assert(data.min_width == 57.5 && data.max_width == 82.5);
    assert(load_hook_calibration(path, 5, {1280, 720}, 5).points.empty());
    {
        std::ofstream file(path);
        file << "box_x,box_y,box_width,box_height,center_x,center_y,frame_width,frame_height\n"
             << "290,100,60,150,320,175,640,640\n"
             << "290,100,60,150,320,175,1280,720\n";
    }
    data = load_hook_calibration(path, 5, frame, 5);
    assert(data.accepted == 1 && data.rejected == 1);
    {
        std::ofstream file(path);
        file << "box_scale,center_x,center_y\n100,320,200\n";
    }
    assert(!load_hook_calibration(path, 5, frame, 5).error.empty());
    unlink(path);
    assert(!load_hook_calibration(path, 5, frame, 5).error.empty());
}

static void alarm_tests()
{
    HookConfig config;
    config.safe_center_x = config.safe_center_y = 320;
    config.safe_radius = 100; config.confirm_ms = 300; config.clear_ms = 500;
    HookGuard guard;
    auto outside = observation({450, 320}, true);
    auto inside = observation({320, 320});
    assert(!guard.update(outside, frame, 0, config).alarm);
    auto out = guard.update(outside, frame, 300, config);
    assert(out.alarm && out.triggered && out.position_estimated);
    assert(guard.update(observation({320, 320}, true), frame, 1000, config).alarm);
    out = guard.update(observation({320, 320}, true), frame, 2000, config);
    assert(out.alarm && !out.cleared && out.safe_elapsed_ms == 0);
    assert(guard.update(inside, frame, 2100, config).alarm);
    HookObservation invalid = inside; invalid.valid = false;
    out = guard.update(invalid, frame, 2500, config);
    assert(out.alarm && !out.measurement_valid && out.held_during_loss && out.safe_elapsed_ms == 0);
    assert(guard.update(inside, frame, 2600, config).alarm);
    assert(guard.update(inside, frame, 3099, config).alarm);
    out = guard.update(inside, frame, 3100, config);
    assert(!out.alarm && out.cleared);

    guard.reset();
    assert(!guard.update(outside, frame, 0, config).alarm);
    assert(!guard.update(HookObservation{}, frame, 250, config).alarm);
    assert(!guard.update(outside, frame, 300, config).alarm);
    assert(!guard.update(outside, frame, 599, config).alarm);
    assert(guard.update(outside, frame, 600, config).alarm);
    assert(guard.update(HookObservation{}, frame, 601, config).held_during_loss);
    out = guard.update(HookObservation{}, frame, 1200, config);
    assert(out.alarm && !out.held_during_loss && !out.measurement_valid);
    guard.update(inside, frame, 1300, config);
    assert(guard.update(observation({320, 320}, false, 8), frame, 1700, config).alarm);
    assert(guard.update(observation({320, 320}, false, 8), frame, 2199, config).alarm);
    assert(guard.update(observation({320, 320}, false, 8), frame, 2200, config).cleared);
    config.confirm_ms = config.clear_ms = 0;
    assert(guard.update(outside, frame, 2300, config).triggered);
    assert(guard.update(observation({320, 320}, true), frame, 2400, config).alarm);
    assert(guard.update(inside, frame, 2500, config).cleared);
    config.enabled = false;
    assert(!guard.update(outside, frame, 2600, config).alarm);
}

int main()
{
    geometry_tests(); csv_tests(); alarm_tests();
    std::cout << "Hook geometry, CSV compatibility, and alarm timing tests passed\n";
}
