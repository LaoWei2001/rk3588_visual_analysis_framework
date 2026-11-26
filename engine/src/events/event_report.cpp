#include "application/config/config.h"
#include "application/runtime/app_ctrl.h"
#include "event_report_internal.h"
#include "logic/context_access.h"
#include "logic/global_logic.h"
#include <algorithm>
#include <channel.h>
#include <cstdio>
#include <ctime>
#include <drawing.h>
#include <mutex>
#include <utility>

using namespace event_report_detail;

namespace
{
class RuntimeEventSink final : public EventSink
{
  public:
    EventReportResult report(ChannelContext &context, const EventRequest &request) override;
    EventReportResult report(GlobalContext &context, const EventRequest &request) override;
};

struct EventIdentity
{
    std::string scope;
    int source_channel_id = -1;
    int video_source_channel_id = -1;
    uint64_t snapshot_unix_ms = 0;
};

static EventReportResult report_event_impl(ChannelContext *ctx, const EventRequest &input,
                                           const CompositeImage *composite_image, const EventIdentity &identity)
{
    auto result = [](EventReportStatus status, const std::string &event_id, const std::string &detail) {
        EventReportResult value;
        value.status = status;
        value.event_id = event_id;
        value.detail = detail;
        return value;
    };

    if (!ctx || !ctx->config || input.event_type.empty())
        return result(EventReportStatus::INVALID_REQUEST, "", "ctx/config is null or event type is empty");
    const ChannelConfig &cfg = *ctx->config;
    cJSON *policy = parse_object_or_empty(cfg.report_policy_json);
    if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(policy, "enabled")))
    {
        cJSON_Delete(policy);
        return result(EventReportStatus::DISABLED, "", "report policy is disabled");
    }
    // Every accepted event requests both local images. Deliveries may additionally request video.
    const bool need_video = delivery_needs_video(policy, input.event_type);
    const int resolved_video_source_channel_id = identity.video_source_channel_id;
    if (need_video && !app_ctrl_has_channel(resolved_video_source_channel_id))
    {
        cJSON_Delete(policy);
        return result(EventReportStatus::INVALID_REQUEST, "", "event video source channel is not available");
    }

    float merge_sec = (float)policy_number(policy, "merge_window_sec", 5.0);
    merge_sec = std::max(0.0f, std::min(60.0f, merge_sec));
    const float video_pre_sec = cfg.event_video.pre_sec;
    const float video_post_sec = cfg.event_video.post_sec;
    const int video_fps = cfg.event_video.fps;
    const std::string image_overlay = policy_string(policy, "image_overlay", "custom");
    std::string merge_key = identity.scope + ":" + input.event_type;
    uint64_t now_ms = steady_now_ms();
    const bool allow_merge = input.merge_mode == EventMergeMode::POLICY && merge_sec > 0.0f;

    pthread_mutex_lock(&g_mtx);
    if (!ensure_workers_locked())
    {
        record_failure("event worker unavailable", "");
        pthread_mutex_unlock(&g_mtx);
        cJSON_Delete(policy);
        return result(EventReportStatus::WORKER_UNAVAILABLE, "", "event persistence worker could not start");
    }
    auto found = g_active.find(merge_key);
    if (allow_merge && found != g_active.end() &&
        now_ms - found->second.last_trigger_ms <= (uint64_t)(merge_sec * 1000.0f))
    {
        found->second.last_trigger_ms = now_ms;
        std::string existing = found->second.event_id;
        pthread_mutex_unlock(&g_mtx);
        PersistenceJob merge_job;
        merge_job.type = PersistenceJob::MERGE;
        merge_job.event_id = existing;
        merge_job.trigger_unix_ms = ctx->unix_ms;
        merge_job.trigger_time = ctx->time_str();
        merge_job.fields = input.fields;
        merge_job.extend_video = need_video;
        pthread_mutex_lock(&g_persist_mtx);
        g_persist_jobs.push(std::move(merge_job));
        pthread_cond_signal(&g_persist_cv);
        pthread_mutex_unlock(&g_persist_mtx);
        cJSON_Delete(policy);
        return result(EventReportStatus::MERGED, existing, "merge queued for local persistence");
    }
    std::string event_id = make_event_id(identity.scope);
    if (allow_merge)
    {
        ActiveEvent active;
        active.event_id = event_id;
        active.last_trigger_ms = now_ms;
        g_active[merge_key] = active;
    }
    pthread_mutex_unlock(&g_mtx);

    const std::string dir = store_dir_path() + "/" + event_id;

    /*
     * schema v3 按写入者拆分状态，避免 C++ 和 Python 对同一 JSON 做读改写：
     *   event.json          C++ 拥有事件、来源、业务字段和策略快照
     *   media_state.json    C++ 图片/录像模块拥有
     *   delivery_state.json Python 上传服务在初始化后独占
     * event.json 最后落盘，兼作“事件目录创建完成”的可见标记。
     */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", 3);

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "id", event_id.c_str());
    cJSON_AddStringToObject(event, "type", input.event_type.c_str());
    cJSON_AddStringToObject(event, "message", input.message.c_str());
    cJSON_AddNumberToObject(event, "created_unix_sec", (double)time(nullptr));
    cJSON_AddNumberToObject(event, "trigger_unix_ms", (double)ctx->unix_ms);
    cJSON_AddNumberToObject(event, "last_trigger_unix_ms", (double)ctx->unix_ms);
    cJSON_AddNumberToObject(event, "trigger_count", 1);
    ChannelContext snapshot_clock{};
    snapshot_clock.unix_ms = identity.snapshot_unix_ms != 0 ? identity.snapshot_unix_ms : ctx->unix_ms;
    const std::string snap_time = snapshot_clock.time_str();
    cJSON_AddStringToObject(event, "snap_time", snap_time.c_str());
    cJSON_AddStringToObject(event, "end_time", snap_time.c_str());
    cJSON_AddItemToObject(event, "merged_triggers", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "event", event);

    cJSON *source = cJSON_CreateObject();
    if (identity.source_channel_id >= 0)
        cJSON_AddNumberToObject(source, "channel_id", identity.source_channel_id);
    if (need_video)
        cJSON_AddNumberToObject(source, "video_channel_id", resolved_video_source_channel_id);
    if (composite_image)
    {
        std::vector<int> actual_channel_ids;
        actual_channel_ids.reserve(composite_image->panes.size());
        for (const ImageJob::Pane &pane : composite_image->panes)
            actual_channel_ids.push_back(pane.channel_id);
        cJSON_AddStringToObject(source, "image_selection_mode", composite_image->selection_mode.c_str());
        cJSON_AddItemToObject(source, "requested_image_channel_ids",
                              channel_ids_to_json(composite_image->requested_channel_ids));
        cJSON_AddItemToObject(source, "image_channel_ids", channel_ids_to_json(actual_channel_ids));
        cJSON_AddItemToObject(source, "missing_image_channel_ids",
                              channel_ids_to_json(composite_image->missing_channel_ids));
        cJSON *image_frames = cJSON_CreateArray();
        for (const ImageJob::Pane &pane : composite_image->panes)
        {
            cJSON *frame = cJSON_CreateObject();
            cJSON_AddNumberToObject(frame, "channel_id", pane.channel_id);
            cJSON_AddNumberToObject(frame, "publication_seq", static_cast<double>(pane.publication_seq));
            cJSON_AddNumberToObject(frame, "frame_id", static_cast<double>(pane.frame_id));
            cJSON_AddNumberToObject(frame, "frame_unix_ms", static_cast<double>(pane.frame_unix_ms));
            cJSON_AddItemToArray(image_frames, frame);
        }
        cJSON_AddItemToObject(source, "image_frames", image_frames);
    }
    cJSON_AddItemToObject(root, "source", source);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "fields", fields_to_json(input.fields));
    cJSON_AddItemToObject(root, "data", data);

    cJSON *media_root = cJSON_CreateObject();
    cJSON_AddNumberToObject(media_root, "schema_version", 3);
    cJSON_AddStringToObject(media_root, "status", "requested");
    cJSON *media_entries = cJSON_CreateObject();
    {
        cJSON *image = cJSON_CreateObject();
        cJSON_AddStringToObject(image, "status", "requested");
        cJSON_AddStringToObject(image, "error", "");
        cJSON *files = cJSON_CreateObject();
        cJSON_AddStringToObject(files, "annotated_image", "");
        cJSON_AddStringToObject(files, "raw_image", "");
        cJSON_AddItemToObject(image, "files", files);
        cJSON_AddItemToObject(media_entries, "image", image);
    }
    if (need_video)
    {
        cJSON *video = cJSON_CreateObject();
        cJSON_AddStringToObject(video, "status", "requested");
        cJSON_AddStringToObject(video, "error", "");
        cJSON *files = cJSON_CreateObject();
        cJSON_AddStringToObject(files, "video", "");
        cJSON_AddItemToObject(video, "files", files);
        cJSON_AddItemToObject(media_entries, "video", video);
    }
    cJSON_AddItemToObject(media_root, "media", media_entries);

    cJSON *delivery_root = cJSON_CreateObject();
    cJSON_AddNumberToObject(delivery_root, "schema_version", 3);
    add_delivery_statuses(policy, input.event_type, delivery_root);
    cJSON_AddItemToObject(root, "policy_snapshot", policy); /* ownership transferred */

    PersistenceJob persist_job;
    persist_job.type = PersistenceJob::CREATE;
    persist_job.event_id = event_id;
    persist_job.event_dir = dir;
    persist_job.merge_key = merge_key;
    std::vector<std::string> media_errors;
    {
        const cv::Mat *event_frame = composite_image ? nullptr : ctx->source_frame();
        const bool has_composite_frame =
            composite_image && std::any_of(composite_image->panes.begin(), composite_image->panes.end(),
                                           [](const ImageJob::Pane &pane) { return !pane.frame.empty(); });
        if ((!composite_image && (!event_frame || event_frame->empty())) || (composite_image && !has_composite_frame))
        {
            const char *reason = composite_image ? "all selected channel frames are empty" : "source frame is unavailable";
            cJSON *image = cJSON_GetObjectItemCaseSensitive(media_entries, "image");
            cJSON_ReplaceItemInObjectCaseSensitive(image, "status", cJSON_CreateString("failed"));
            cJSON_ReplaceItemInObjectCaseSensitive(image, "error", cJSON_CreateString(reason));
            media_errors.push_back(std::string("image: ") + reason);
        }
        else
        {
            cJSON *image = cJSON_GetObjectItemCaseSensitive(media_entries, "image");
            cJSON_ReplaceItemInObjectCaseSensitive(image, "status", cJSON_CreateString("generating"));
            ImageJob job;
            job.event_dir = dir;
            job.overlay_mode = image_overlay;
            if (composite_image)
            {
                job.composite_width = composite_image->width;
                job.composite_height = composite_image->height;
                job.composite_rows = composite_image->rows;
                job.composite_cols = composite_image->cols;
                job.panes = composite_image->panes;
            }
            else
            {
                /* 原始帧只读。图片线程通过 Mat 引用保留像素，绘制标注时再复制。 */
                job.raw = *event_frame;
                if (image_overlay != "none")
                {
                    job.render_params = channel_render_params(*ctx);
                    job.render_params.show_fps = 0;
                    /* 上报图片自动复用实时画面的完整叠加层；同时允许开发者额外声明
                     * IMAGE 专用指令。业务 logic 不需要再在原图上重复画框/文字。 */
                    job.render_params.target_mask = static_cast<uint8_t>(DrawCommand::DISPLAY | DrawCommand::IMAGE);
                    job.render_params.show_system_overlays = true;
                    job.render_params.show_custom_overlays = true;
                    if (ctx->config && ctx->config->inference_roi.has_roi())
                        job.inference_roi = ctx->config->inference_roi;
                    if (ctx->rois)
                        job.rois = *ctx->rois;
                    if (ctx->results)
                        job.results = *ctx->results;
                    if (VisionContextAccess::drawing(*ctx))
                        job.commands = *VisionContextAccess::drawing(*ctx);
                    job.render_params.inference_roi = nullptr;
                    job.render_params.roi_zones = nullptr;
                    job.render_params.results = nullptr;
                    job.render_params.draw_cmds = nullptr;
                }
            }
            persist_job.has_image_job = true;
            persist_job.image_job = std::move(job);
        }
    }

    if (need_video)
    {
        cJSON *video = cJSON_GetObjectItemCaseSensitive(media_entries, "video");
        cJSON_ReplaceItemInObjectCaseSensitive(video, "status", cJSON_CreateString("generating"));
        persist_job.has_video_job = true;
        persist_job.video_job.event_id = event_id;
        persist_job.video_job.channel_id = resolved_video_source_channel_id;
        persist_job.video_job.event_type = input.event_type;
        persist_job.video_job.pre_sec = video_pre_sec;
        persist_job.video_job.post_sec = video_post_sec;
        persist_job.video_job.fps = video_fps;
        persist_job.video_job.output_path = dir + "/clip.mp4";
    }
    refresh_media_status(media_root);

    const bool serialized = json_to_text(delivery_root, persist_job.delivery_json) &&
                            json_to_text(media_root, persist_job.media_json) &&
                            json_to_text(root, persist_job.event_json);
    cJSON_Delete(delivery_root);
    cJSON_Delete(media_root);
    cJSON_Delete(root);
    if (!serialized)
    {
        record_failure("event serialization failed", event_id);
        clear_active_event(merge_key, event_id);
        return result(EventReportStatus::STORAGE_ERROR, "", "event state could not be serialized");
    }

    pthread_mutex_lock(&g_persist_mtx);
    g_persist_jobs.push(std::move(persist_job));
    pthread_cond_signal(&g_persist_cv);
    pthread_mutex_unlock(&g_persist_mtx);

    if (!media_errors.empty())
    {
        record_failure("event media unavailable", event_id);
        clear_active_event_by_id(event_id);
        std::string detail;
        for (const std::string &error : media_errors)
        {
            if (!detail.empty())
                detail += "; ";
            detail += error;
        }
        return result(EventReportStatus::CREATED_MEDIA_FAILED, event_id, detail + "; event queued for persistence");
    }
    return result(EventReportStatus::CREATED, event_id, "event queued for local persistence");
}

EventReportResult RuntimeEventSink::report(ChannelContext &context, const EventRequest &request)
{
    ChannelContext *ctx = &context;
    EventIdentity identity;
    if (ctx)
    {
        identity.scope = "ch" + std::to_string(ctx->chnId);
        identity.source_channel_id = ctx->chnId;
        identity.video_source_channel_id = ctx->chnId;
    }
    return report_event_impl(ctx, request, nullptr, identity);
}

EventReportResult RuntimeEventSink::report(GlobalContext &context, const EventRequest &request)
{
    GlobalContext *gctx = &context;
    auto invalid = [](const std::string &detail) {
        EventReportResult value;
        value.status = EventReportStatus::INVALID_REQUEST;
        value.detail = detail;
        return value;
    };

    if (!gctx || !gctx->config)
        return invalid("global ctx/config is null");
    if (request.source_channel_id >= 0 && !gctx->channel(request.source_channel_id))
        return invalid("global event source channel is not available in this application");

    auto runtime = app_ctrl_get_runtime_snapshot();
    if (!runtime)
        return invalid("global event runtime config is not available");

    cJSON *policy = parse_object_or_empty(gctx->config->report_policy_json);
    ImageSelectionPolicy image_selection;
    std::string image_selection_error;
    const bool image_selection_valid = parse_image_selection(policy, &image_selection, &image_selection_error);
    cJSON_Delete(policy);
    if (!image_selection_valid)
        return invalid(image_selection_error);

    CompositeImage composite;

    /* 从本 tick 已保留的 publication 取证，避免告警字段属于旧版本而图片来自新版本。
     * 采样只共享引用，真正上报才物化和复制图片；无需阻止通道继续发布。 */
    auto capture_report_frame = [&](int channel_id, ChannelFrameSnapshot *out) -> bool {
        if (!gctx->get_channel_frame_snapshot(channel_id, out))
            return false;
        return out->logic.has_publication && out->logic.has_frame && out->logic.online_state == CH_ONLINE &&
               out->logic.config_generation == runtime->generation;
    };

    std::vector<int> image_channel_ids;
    if (image_selection.mode == "selected")
    {
        for (int channel_id : image_selection.channel_ids)
            append_unique_channel(image_channel_ids, channel_id);
    }
    else if (image_selection.mode == "event_evidence")
    {
        for (int channel_id : request.evidence_channel_ids)
            append_unique_channel(image_channel_ids, channel_id);
    }
    else if (VisionContextAccess::connected_channels(*gctx))
    {
        for (int channel_id : *VisionContextAccess::connected_channels(*gctx))
            append_unique_channel(image_channel_ids, channel_id);
    }

    /* 旧全局 Logic 可能尚未填写 evidence_channel_ids；这时使用全部连入通道，
     * 但绝不再挑其中一路冒充“主通道”。selected 为空则让图片明确失败。 */
    if (image_channel_ids.empty() && image_selection.mode == "event_evidence" &&
        VisionContextAccess::connected_channels(*gctx))
        for (int channel_id : *VisionContextAccess::connected_channels(*gctx))
            append_unique_channel(image_channel_ids, channel_id);

    composite.width = runtime->config.disp_width & ~3;
    composite.height = runtime->config.disp_height & ~1;
    composite.selection_mode = image_selection.mode;
    composite.requested_channel_ids = image_channel_ids;
    composite.panes.reserve(image_channel_ids.size());

    for (int channel_id : image_channel_ids)
    {
        ChannelFrameSnapshot snapshot;
        if (!capture_report_frame(channel_id, &snapshot))
        {
            composite.missing_channel_ids.push_back(channel_id);
            continue;
        }

        ImageJob::Pane pane;
        pane.channel_id = channel_id;
        pane.publication_seq = snapshot.logic.publication_seq;
        pane.frame_id = snapshot.logic.frame_seq;
        pane.frame_unix_ms = snapshot.logic.frame_unix_ms;
        pane.frame = snapshot.frame;
        pane.results = snapshot.results;
        pane.rois = snapshot.rois;
        pane.commands = snapshot.draw_cmds;
        const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, channel_id);
        if (channel && channel->inference_roi.has_roi())
            pane.inference_roi = channel->inference_roi;
        pane.render_params.chnId = channel_id;
        pane.render_params.inputW = pane.frame.cols;
        pane.render_params.inputH = pane.frame.rows;
        pane.render_params.disp_fps = snapshot.logic.disp_fps;
        pane.render_params.infer_fps = snapshot.logic.infer_fps;
        pane.render_params.result_frame_id = snapshot.logic.frame_seq;
        pane.render_params.show_fps = 0;
        pane.render_params.target_mask = static_cast<uint8_t>(DrawCommand::DISPLAY | DrawCommand::IMAGE);
        pane.render_params.show_system_overlays = true;
        pane.render_params.show_custom_overlays = true;
        pane.render_params.depth_background = channel_uses_depth_background(channel);
        composite.panes.push_back(std::move(pane));
    }

    /* 缺失通道采用紧凑跳过策略；任意一路可用就继续生成图片。全部不可用时仍进入
     * 本地事件箱，由 report_event_impl 将图片明确标记为 failed，而不是丢弃告警。 */
    const int pane_count = static_cast<int>(composite.panes.size());
    composite.cols = std::max(1, std::min(runtime->config.tile_cols, std::max(1, pane_count)));
    composite.rows = std::max(1, (std::max(1, pane_count) + composite.cols - 1) / composite.cols);

    /* 全局事件直接使用全局节点的上报配置，不借用任意通道充当事件身份。 */
    ChannelConfig report_config;
    report_config.report_policy_json = gctx->config->report_policy_json;
    report_config.event_video = gctx->config->event_video;

    ChannelContext ctx{};
    ctx.chnId = -1;
    ctx.timestamp_ms = gctx->timestamp_ms;
    ctx.unix_ms = gctx->unix_ms;
    if (const ChannelLogicSnapshot *source = gctx->channel(request.source_channel_id))
    {
        if (source->frame_unix_ms != 0)
        {
            ctx.timestamp_ms = source->frame_steady_ms;
            ctx.unix_ms = source->frame_unix_ms;
        }
    }
    ctx.config = &report_config;

    EventIdentity identity;
    identity.scope = "global_" + gctx->config->instance_id;
    identity.source_channel_id = request.source_channel_id;
    identity.video_source_channel_id = gctx->config->media_source_channel_id;
    for (const ImageJob::Pane &pane : composite.panes)
        identity.snapshot_unix_ms = std::max(identity.snapshot_unix_ms, pane.frame_unix_ms);
    return report_event_impl(&ctx, request, &composite, identity);
}
} // namespace

EventSink &event_report_runtime_sink()
{
    static RuntimeEventSink sink;
    return sink;
}
