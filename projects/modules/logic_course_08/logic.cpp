// 课程8:如何定时上报图片
// 实现效果:每隔一段设定的时间就向服务器上传一张当前视频帧的截图
// 难度:★★★☆☆

#include <channel.h>
#include <events.h>
#include <cstdio>

namespace
{

struct UploadDemoState
{
    explicit UploadDemoState(uint64_t now) : upload_time(now) {}
    uint64_t upload_time;
};

static void logic_course_08(ChannelContext *ctx)
{
    if (!ctx)
    {
        return;
    }
    // 构造参数只在首次调用时用于初始化，不会每帧重置计时。
    UploadDemoState *state = ctx->get_state<UploadDemoState>(ctx->timestamp_ms);
    if (!state)
        return;
    // 如果与上一次的报警间隔了5秒
    float time_interval = ctx->param_float("time_interval");
    if (ctx->timestamp_ms - state->upload_time >= time_interval * 1000)
    {
        printf("%zu >= %f \n", ctx->timestamp_ms - state->upload_time, time_interval * 1000);
        state->upload_time = ctx->timestamp_ms;
        // printf("==========\n");
        EventRequest request;
        request.event_type = "logic_course_08";
        request.message = "定时上报";
        request.fields = {
            event_field("server_event_type", ctx->param_string("server_event_type")),
            event_field("invade_flag", ctx->param_int("invade_flag")),
            event_field("yuv_width", ctx->business_width()),
            event_field("yuv_height", ctx->business_height()),
            event_field("yuv_flag", ctx->param_string("yuv_flag")),
        };
        const EventReportResult report = report_event(ctx, request);
        if (report.accepted())
            printf("事件已创建: %s\n", report.event_id.c_str());
    }
}

} // namespace

REGISTER_LOGIC(logic_course_08);
