#include "app_ctrl.h"
#include "logic/core/context_access.h"
#include "pipeline/frame_transform.h"

int app_ctrl_materialize_channel_frame_snapshot(const ChannelLogicSnapshot &snapshot, ChannelFrameSnapshot *out)
{
    if (!out)
        return 0;
    *out = ChannelFrameSnapshot();
    out->logic = snapshot;
    if (!VisionContextAccess::media(snapshot) || !VisionContextAccess::media(snapshot)->frame)
    {
        out->logic.has_frame = false;
        return 1;
    }

    const auto &media = *VisionContextAccess::media(snapshot);
    const cv::Mat *frame = media.frame->model_frame();
    if (frame)
        out->frame = frame->clone();
    out->logic.has_frame = !out->frame.empty();
    out->results = media.results;
    for (auto &result : out->results)
        if (!result.boxMask.empty())
            result.boxMask = result.boxMask.clone();
    out->draw_cmds = media.commands;
    for (const auto &layer : VisionContextAccess::layers(snapshot))
        out->draw_cmds.insert(out->draw_cmds.end(), layer.second.begin(), layer.second.end());

    if (media.runtime)
    {
        const int id = snapshot.channel_id;
        if (id >= 0 && id < MAX_CHANNEL_NUM)
            out->rois = media.runtime->roi_zones[id];
    }
    /* 图片与结果已经复制到调用方；不让图片任务继续占用解码池缓冲区。 */
    VisionContextAccess::media(out->logic).reset();
    return 1;
}

bool GlobalContext::get_channel_frame_snapshot(int configured_id, ChannelFrameSnapshot *out) const
{
    const ChannelLogicSnapshot *expected = channel(configured_id);
    if (!out || !expected)
        return false;
    ChannelLogicSnapshot evidence = *expected;
    const auto overlay = image_draw_commands.find(configured_id);
    if (config && overlay != image_draw_commands.end())
    {
        if (overlay->second.empty())
            evidence.global_draw_commands_by_owner.erase(config->instance_id);
        else
            evidence.global_draw_commands_by_owner[config->instance_id] = overlay->second;
    }
    return app_ctrl_materialize_channel_frame_snapshot(evidence, out) != 0 && out->logic.has_frame;
}
