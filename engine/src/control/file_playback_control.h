#pragma once

/**
 * 本地文件播放控制服务。
 *
 * 与 channel/global Logic 控制完全独立：只查询采集器时间轴并执行 seek，
 * 因此业务模块未启用或切换时也不会互相影响。
 */
int file_playback_control_init(void);
void file_playback_control_deinit(void);

