#pragma once

/* Internal event worker lifecycle and video completion callbacks. */
#include <events.h>

/* 录像模块完成 MP4 后调用。 */
void event_report_video_ready(const std::string &event_id, const std::string &video_path);
void event_report_video_failed(const std::string &event_id, const std::string &video_path, const std::string &reason);

/* 排空并停止事件持久化/图片落盘线程。 */
void event_report_deinit(void);
