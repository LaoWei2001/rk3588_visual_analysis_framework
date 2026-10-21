#pragma once

#include <atomic>
#include <mutex>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

// A retired media may finish unpreparing after a new media has been configured.
// Keep ownership and active state together; readers retain their own source ref.
class RtspMediaSource
{
public:
    ~RtspMediaSource() { clear(); }

    void replace(GstRTSPMedia *media, GstElement *source)
    {
        GstElement *previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = source_;
            source_ = GST_ELEMENT(gst_object_ref(source));
            owner_ = media; // Identity only; never dereferenced. Callback owns media.
            active_.store(true, std::memory_order_release);
        }
        if (previous)
            gst_object_unref(previous);
    }

    bool release(GstRTSPMedia *media)
    {
        GstElement *previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (owner_ != media)
                return false;
            previous = source_;
            source_ = nullptr;
            owner_ = nullptr;
            active_.store(false, std::memory_order_release);
        }
        if (previous)
            gst_object_unref(previous);
        return true;
    }

    GstElement *acquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return source_ ? GST_ELEMENT(gst_object_ref(source_)) : nullptr;
    }

    bool active() const { return active_.load(std::memory_order_acquire); }

    void clear()
    {
        GstElement *previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = source_;
            source_ = nullptr;
            owner_ = nullptr;
            active_.store(false, std::memory_order_release);
        }
        if (previous)
            gst_object_unref(previous);
    }

private:
    std::mutex mutex_;
    GstElement *source_{nullptr};
    GstRTSPMedia *owner_{nullptr};
    std::atomic<bool> active_{false};
};
