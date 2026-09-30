// 课程8:如何定时上报图片
// 实现效果:每隔一段设定的时间就向服务器上传一张当前视频帧的截图
// 难度:★★★☆☆

#include "logic/core/logic_common.h"

namespace
{

struct UploadDemoState
{
    uint64_t upload_time = 0;
};

static void logic_course_08(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
    {
        return;
    }
    if (!(*ctx->state))
    {
        std::shared_ptr<UploadDemoState> p = std::make_shared<UploadDemoState>();
        // 设中间变量p将时间初始化之后再传给*(ctx->state)
        p->upload_time = ctx->timestamp_ms;
        *(ctx->state) = p;
    }
    std::shared_ptr<UploadDemoState> state = std::static_pointer_cast<UploadDemoState>(*ctx->state);
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
            event_field("yuv_width", inference_get_input_w()),
            event_field("yuv_height", inference_get_input_h()),
            event_field("yuv_flag", ctx->param_string("yuv_flag")),
        };
        const EventReportResult report = report_event(ctx, request);
        if (report.accepted())
            printf("事件已创建: %s\n", report.event_id.c_str());
    }
}

} // namespace

REGISTER_LOGIC(logic_course_08);
