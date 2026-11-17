#pragma once

/* Internal event worker lifecycle and video completion callbacks. */
#include <events.h>

/* 录像模块完成 MP4 后调用。 */
bool event_report_video_ready(const std::string &event_id, const std::string &video_path);
void event_report_video_failed(const std::string &event_id, const std::string &video_path, const std::string &reason);

/* 排空并停止事件持久化/图片落盘线程。 */
void event_report_deinit(void);
// Read after BOTH event_report_deinit and event_video_recorder_deinit.
// Sticky for the process lifetime: draining workers does not erase earlier failures.
unsigned long event_report_failure_count();

/* Bind once when constructing production logic contexts. */
EventSink &event_report_runtime_sink();
