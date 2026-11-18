#pragma once
#include <runtime.h>

std::vector<vision::FilePlaybackStatus> file_playback_status();
bool file_playback_set_paused(int channel_id, bool paused, std::string *error = nullptr);
bool file_playback_seek(int channel_id, int64_t position_ms, std::string *error = nullptr);

/**
 * 本地文件播放控制服务。
 *
 * 与 channel/global Logic 控制完全独立：通过同一 SDK 控制入口查询、暂停、继续和跳转，
 * 因此业务模块未启用或切换时也不会互相影响。
 */
int file_playback_control_init(void);
void file_playback_control_deinit(void);
