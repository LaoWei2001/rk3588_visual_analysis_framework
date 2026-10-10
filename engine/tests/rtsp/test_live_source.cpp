#include "rtsp/live_source.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static GstBuffer *frame(unsigned sequence, size_t bytes = 4096)
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    GST_BUFFER_PTS(buffer) = sequence * GST_SECOND / 30;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    return buffer;
}

static void pressure_test(bool bounded)
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch("appsrc name=source is-live=true format=time block=false max-bytes=8192 "
                                            "! identity sleep-time=200000 ! fakesink sync=false async=false",
                                            &error);
    assert(pipeline && !error);
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    assert(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    unsigned accepted = 0;
    for (unsigned i = 0; i < 100; ++i)
    {
        if (bounded && !rtsp_source_has_room(source, 4096))
            continue;
        GstBuffer *buffer = frame(i);
        GstFlowReturn result;
        g_signal_emit_by_name(source, "push-buffer", buffer, &result);
        gst_buffer_unref(buffer);
        assert(result == GST_FLOW_OK);
        ++accepted;
    }
    guint64 bytes = 0;
    g_object_get(source, "current-level-bytes", &bytes, nullptr);
    if (bounded)
    {
        assert(bytes <= 8192 && accepted <= 3);
        // Consumption resumes without inheriting an unbounded FIFO.
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        assert(rtsp_source_has_room(source, 4096));
    }
    else
        assert(bytes > 8192); // Regression: block=false alone does NOT drop.
    std::printf("%s: accepted=%u pending=%llu bytes\n", bounded ? "bounded" : "old", accepted,
                static_cast<unsigned long long>(bytes));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(pipeline);
}

struct Consumer
{
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::vector<GstClockTime> pts;
};
static void handoff(GstElement *, GstBuffer *buffer, gpointer user)
{
    auto &state = *static_cast<Consumer *>(user);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.pts.push_back(GST_BUFFER_PTS(buffer));
    state.cv.notify_all();
    if (state.pts.size() == 1)
        state.cv.wait(lock, [&] { return state.release; });
}
static void latest_decoded_frame_test()
{
    GError *error = nullptr;
    GstElement *pipeline =
        gst_parse_launch("appsrc name=source is-live=true format=time block=false max-bytes=0 "
                         "! queue name=latest max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
                         "! identity name=consumer signal-handoffs=true ! fakesink sync=false async=false",
                         &error);
    assert(pipeline && !error);
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    auto *consumer = gst_bin_get_by_name(GST_BIN(pipeline), "consumer");
    auto *queue = gst_bin_get_by_name(GST_BIN(pipeline), "latest");
    Consumer state;
    g_signal_connect(consumer, "handoff", G_CALLBACK(handoff), &state);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstFlowReturn result;
    GstBuffer *first = frame(0);
    g_signal_emit_by_name(source, "push-buffer", first, &result);
    gst_buffer_unref(first);
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        assert(state.cv.wait_for(lock, std::chrono::seconds(2), [&] { return !state.pts.empty(); }));
    }
    for (unsigned i = 1; i < 40; ++i)
    {
        GstBuffer *buffer = frame(i);
        g_signal_emit_by_name(source, "push-buffer", buffer, &result);
        gst_buffer_unref(buffer);
        assert(result == GST_FLOW_OK);
    }
    // Wait until the independent decoder queue has drained the producer.
    guint64 bytes = 1;
    for (int i = 0; i < 200 && bytes; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        g_object_get(source, "current-level-bytes", &bytes, nullptr);
    }
    assert(bytes == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    guint count = 0;
    g_object_get(queue, "current-level-buffers", &count, nullptr);
    assert(count == 1);
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.release = true;
        state.cv.notify_all();
        assert(state.cv.wait_for(lock, std::chrono::seconds(2), [&] { return state.pts.size() >= 2; }));
        assert(state.pts[1] == 39 * GST_SECOND / 30);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(queue);
    gst_object_unref(consumer);
    gst_object_unref(source);
    gst_object_unref(pipeline);
    std::puts("Decoded queue resumes at latest frame with original timestamp");
}
int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    pressure_test(false);
    pressure_test(true);
    latest_decoded_frame_test();
}
