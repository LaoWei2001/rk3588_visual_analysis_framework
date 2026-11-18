#include "pause_ctrl.h"
#include "application/control/file_playback_control.h"
#include <atomic>

namespace pause_ctrl
{
namespace
{
std::atomic<bool> enabled{false};
}
void init(bool value)
{
    enabled.store(value);
}
bool is_paused()
{
    const auto files = file_playback_status();
    bool any = false;
    for (const auto &file : files)
        if (file.available && !file.ended)
        {
            any = true;
            if (!file.pause_requested)
                return false;
        }
    return any;
}
void toggle()
{
    if (!enabled.load())
        return;
    const bool paused = !is_paused();
    for (const auto &file : file_playback_status())
        if (file.available && !file.ended)
            file_playback_set_paused(file.channel_id, paused);
}
} // namespace pause_ctrl
