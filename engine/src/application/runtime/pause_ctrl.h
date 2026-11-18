#pragma once
namespace pause_ctrl
{
// 本地空格键/SIGUSR1 只切换视频文件；Web/SDK 的单路控制不依赖此开关。
void init(bool enabled);
void toggle();
bool is_paused();
} // namespace pause_ctrl
