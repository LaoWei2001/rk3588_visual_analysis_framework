#pragma once
#include <runtime.h>

// 内部调用必须确保 APP_CTRL 存活；公开查询由 VideoRuntime 的生命周期锁保护。
std::vector<vision::VideoChannelStatus> app_ctrl_channel_status();
void app_ctrl_capture_status(int channel_id, vision::VideoChannelState state, const char *error = nullptr) noexcept;
void app_ctrl_network_rtt(int channel_id, float milliseconds) noexcept;
void app_ctrl_worker_failed(int channel_id, const char *worker, const char *error) noexcept;
