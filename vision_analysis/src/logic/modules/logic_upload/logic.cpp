/**
 * @file logic_upload.cpp
 * @brief 通用告警框架最小演示。
 *
 * 任一目标框中心从 ROI 外进入当前通道任一已配置 ROI 时触发一次；全部目标离开所有
 * ROI 后解除锁存。图片/视频选择、目标、Profile 和 JSON 映射来自 report_policy。
 */
#include "logic/core/logic_common.h"

struct UploadDemoState
{
    bool latched = false;
};

static void logic_upload(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<UploadDemoState>();
    auto state = std::static_pointer_cast<UploadDemoState>(*ctx->state);

    if (ctx->roi_count() <= 0)
    {
        state->latched = false;
        draw_text(ctx, "upload demo: configure ROI first", cv::Point(20, 30), cv::Scalar(0, 165, 255), 0.55, 1);
        return;
    }

    const AlgoResult *best = nullptr;
    int best_roi = ROI_NONE;
    for (const auto &result : *ctx->results)
    {
        const int roi_index = ctx->roi_index_of(result.box);
        if (roi_index != ROI_NONE && (!best || result.score > best->score))
        {
            best = &result;
            best_roi = roi_index;
        }
    }

    if (!best)
    {
        state->latched = false; /* 所有目标离开 ROI 后，允许下一次进入时重新报警。 */
        draw_text(ctx, "upload demo: waiting target enter ROI", cv::Point(20, 30), cv::Scalar(160, 160, 160), 0.55, 1);
        return;
    }

    const std::string roi_name = ctx->roi_name_at(best_roi);
    const std::string status_text = "upload demo: target entered ROI " + roi_name;
    draw_text(ctx, status_text.c_str(), cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);

    /* 演示：这些绘制只进入上报图片/视频，不显示在实时画面。
     * Web 选择“仅自定义”或“全部叠加”时，Dify 能直接从媒体中读取这些信息。 */
    const std::string upload_text =
        "ROI=" + roi_name + " label=" + best->label + " score=" + std::to_string(best->score);
    draw_text(ctx, upload_text.c_str(), cv::Point(20, 62), cv::Scalar(0, 0, 255), 0.65, 2, DrawCommand::UPLOAD);
    draw_rect(ctx, best->box, cv::Scalar(0, 0, 255), 3, 1.0, DrawCommand::UPLOAD);
    if (state->latched)
        return;
    state->latched = true;

    const std::string event_id = report_alarm(ctx, "upload_demo_roi_entry", "检测到目标进入ROI",
                                              {
                                                  alarm_field("label", best->label),
                                                  alarm_field("score", best->score),
                                                  alarm_field("track_id", best->track_id),
                                                  alarm_field("roi_index", best_roi),
                                                  alarm_field("roi_name", roi_name),
                                                  alarm_field("box_x", best->box.x),
                                                  alarm_field("box_y", best->box.y),
                                                  alarm_field("box_width", best->box.width),
                                                  alarm_field("box_height", best->box.height),
                                              });
    if (!event_id.empty())
        printf("[logic_upload] alarm event created: %s\n", event_id.c_str());
}

REGISTER_LOGIC(logic_upload);
