#pragma once

#include <gst/gst.h>

// block=false does not enforce max-bytes: appsrc will continue queuing buffers.
// The feeder is the only producer. Stop submitting before copying/converting an
// image when two frames are pending; the downstream leaky queue keeps the latest.
inline bool rtsp_source_has_room(GstElement *source, guint64 frame_bytes)
{
    guint64 queued_bytes = 0;
    g_object_get(source, "current-level-bytes", &queued_bytes, nullptr);
    return queued_bytes < frame_bytes * 2;
}
