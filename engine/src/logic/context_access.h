#pragma once

#include <global.h>

/* Engine-only bindings. No allocation, copying or synchronization is added. */
struct VisionContextAccess
{
    static uint64_t analysis_time(uint64_t steady_ms, int64_t video_ms)
    {
        // 业务常用 0 表示“尚未开始”；文件时钟从 1 起，时间差仍与视频完全一致。
        return video_ms >= 0 ? static_cast<uint64_t>(video_ms) + 1 : steady_ms;
    }
    static void bind_frames(ChannelContext &ctx, ChannelFrameGetter business, ChannelFrameGetter source, void *opaque)
    {
        ctx.business_frame_getter = business;
        ctx.source_frame_getter = source;
        ctx.frame_getter_opaque = opaque;
    }

    static void bind_outputs(ChannelContext &ctx, LogicOutputSet *outputs)
    {
        ctx.outputs = outputs;
    }
    static void bind_parameters(ChannelContext &ctx, const LogicParameterSet *parameters)
    {
        ctx.logic_parameters = parameters;
    }
    static void bind_parameters(GlobalContext &ctx, const LogicParameterSet *parameters)
    {
        ctx.logic_parameters = parameters;
    }
    static void bind_drawing(ChannelContext &ctx, std::vector<DrawCommand> *commands)
    {
        ctx.draw_cmds = commands;
    }
    static std::vector<DrawCommand> *drawing(const ChannelContext &ctx)
    {
        return ctx.draw_cmds;
    }
    static void bind_events(ChannelContext &ctx, EventSink *sink)
    {
        ctx.event_sink = sink;
    }
    static EventSink *events(const ChannelContext &ctx)
    {
        return ctx.event_sink;
    }
    static void bind_events(GlobalContext &ctx, EventSink *sink)
    {
        ctx.event_sink = sink;
    }
    static EventSink *events(const GlobalContext &ctx)
    {
        return ctx.event_sink;
    }
    static bool has_canvas(const ChannelContext &ctx)
    {
        return ctx.canvas && ctx.show_canvas;
    }
    static void bind_canvas(ChannelContext &ctx, cv::Mat *canvas, bool *enabled)
    {
        ctx.canvas = canvas;
        ctx.show_canvas = enabled;
    }

    static void bind_global(GlobalContext &ctx, const std::vector<ChannelLogicSnapshot> *snapshots,
                            const std::vector<int> *channels, const std::vector<ChannelInput> *inputs)
    {
        ctx.channel_snapshots = snapshots;
        ctx.connected_channel_ids = channels;
        ctx.ready_inputs = inputs;
    }

    static const std::vector<int> *connected_channels(const GlobalContext &ctx)
    {
        return ctx.connected_channel_ids;
    }
    static std::map<int, std::vector<DrawCommand>> &overlays(GlobalContext &ctx)
    {
        return ctx.image_draw_commands;
    }
    static std::shared_ptr<const ChannelPublicationMedia> &media(ChannelLogicSnapshot &snapshot)
    {
        return snapshot.media;
    }
    static const std::shared_ptr<const ChannelPublicationMedia> &media(const ChannelLogicSnapshot &snapshot)
    {
        return snapshot.media;
    }
    static std::map<std::string, std::vector<DrawCommand>> &layers(ChannelLogicSnapshot &snapshot)
    {
        return snapshot.global_draw_commands_by_owner;
    }
    static const std::map<std::string, std::vector<DrawCommand>> &layers(const ChannelLogicSnapshot &snapshot)
    {
        return snapshot.global_draw_commands_by_owner;
    }
};
