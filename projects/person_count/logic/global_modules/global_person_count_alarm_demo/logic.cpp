#include <rkvision/global_context.h>

#include <cstdio>
#include <memory>

static void global_person_count_alarm_demo(GlobalContext *gctx)
{
    bool *reported;
    int64_t total;
    int input_count;
    int i;

    if (gctx == NULL || gctx->state == NULL)
        return;

    /* state 是框架提供的跨轮次存储，这个示例只在里面保存一个“是否已报警”标志。 */
    if (gctx->state->get() == NULL)
        *gctx->state = std::make_shared<bool>(false);
    reported = (bool *)gctx->state->get();

    /* 获取本次全局逻辑收到的有效通道数量。 */
    input_count = (int)gctx->input_count();
    total = 0;

    EventRequest request;
    for (i = 0; i < input_count; i++)
    {
        const ChannelInput *channel = gctx->input_at(i);
        int64_t count;

        if (channel == NULL)
            continue;
        count = channel->get_int("person_count");

        total += count;
        if (count > 0)
            request.evidence_channel_ids.push_back(channel->channel_id());
    }

    /* 每次运行都打印。锁住 stdout，避免其他工作线程把文字插进这一行中间。 */
    flockfile(stdout);
    printf("[GlobalPersonCount][%s] ", gctx->config != NULL ? gctx->config->instance_id.c_str() : "unknown");
    for (i = 0; i < input_count; i++)
    {
        const ChannelInput *channel = gctx->input_at(i);
        int64_t count;

        if (channel == NULL)
            continue;
        count = channel->get_int("person_count");
        printf("%s通道%d:%lld人", i == 0 ? "" : ",", channel->channel_id(), (long long)count);
    }
    printf("%s总人数%lld人\n", input_count == 0 ? "" : ",", (long long)total);
    fflush(stdout);
    funlockfile(stdout);

    int64_t threshold = gctx->param_int("alarm_threshold");
    if (total < threshold)
    {
        *reported = false;
        return;
    }
    if (*reported)
        return;

    request.event_type = "person_count_alarm";
    request.message = "多通道人数达到报警阈值";
    /* event_field() 是清单校验器要求的上报字段声明形式。 */
    request.fields = {
        event_field("total_person_count", total),
        event_field("alarm_threshold", threshold),
    };

    if (report_event(gctx, request).accepted())
        *reported = true;
}

REGISTER_GLOBAL_LOGIC(global_person_count_alarm_demo);
