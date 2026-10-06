#include "app_ctrl.h"
#include "pipeline/frame_transform.h"

int app_ctrl_materialize_channel_frame_snapshot(const ChannelLogicSnapshot &snapshot, ChannelFrameSnapshot *out)
{
    if (!out)
        return 0;
    *out = ChannelFrameSnapshot();
    out->logic = snapshot;
    if (!snapshot.media || !snapshot.media->frame)
    {
        out->logic.has_frame = false;
        return 1;
    }

    const auto &media = *snapshot.media;
    const cv::Mat *frame = media.frame->model_frame();
    if (frame)
        out->frame = frame->clone();
    out->logic.has_frame = !out->frame.empty();
    out->results = media.results;
    for (auto &result : out->results)
        if (!result.boxMask.empty())
            result.boxMask = result.boxMask.clone();
    out->draw_cmds = media.commands;
    for (const auto &layer : snapshot.global_draw_commands_by_owner)
        out->draw_cmds.insert(out->draw_cmds.end(), layer.second.begin(), layer.second.end());

    if (media.runtime)
    {
        const int id = snapshot.channel_id;
        if (id >= 0 && id < MAX_CHANNEL_NUM)
            out->rois = media.runtime->roi_zones[id];
    }
    /* 图片与结果已经复制到调用方；不让图片任务继续占用解码池缓冲区。 */
    out->logic.media.reset();
    return 1;
}
