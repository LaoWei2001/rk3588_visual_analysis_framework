#include "display/display.h"
#include "logic/core/context_access.h"
#include "logic/core/global_logic.h"
#include "pipeline/frame_source_owner.h"
#include "pipeline/frame_transform.h"
#include "runtime/app_ctrl.h"

#include <cassert>
#include <iostream>
#include <thread>

/* 独立绘制测试关闭性能文字，不启动应用控制器/采集线程。 */
extern "C" int app_ctrl_get_performance_display(void)
{
    return 0;
}

namespace
{

ChannelLogicSnapshot publication(int id, unsigned char marker)
{
    cv::Mat pixels(160, 160, CV_8UC3, cv::Scalar(marker, marker, marker));
    auto frame = std::make_shared<LazyVideoFrame>(0, nullptr, 160, 160, 160, 160, 0x0D, 160, 160, pixels.data);
    assert(frame->retain_borrowed_source(pixels.total() * pixels.elemSize()));
    pixels.setTo(cv::Scalar(99, 99, 99)); // 软件源回调结束后，原始内存可被复用。
    auto media = std::make_shared<ChannelPublicationMedia>();
    media->frame = frame;
    AlgoResult result;
    result.frame_id = id;
    result.box = cv::Rect(marker, 70, 30, 30);
    result.boxMask = cv::Mat(160, 160, CV_8UC1, cv::Scalar(marker));
    media->results.push_back(result);
    DrawCommand command{};
    command.type = DrawCommand::RECT;
    command.rect = result.box;
    media->commands.push_back(command);
    auto runtime = std::make_shared<AppRuntimeSnapshot>();
    runtime->generation = id;
    RoiZone roi;
    roi.name = "roi_" + std::to_string(id);
    media->runtime = runtime;
    runtime->roi_zones[0].push_back(roi);

    ChannelLogicSnapshot snapshot;
    snapshot.channel_id = 0;
    snapshot.publication_seq = id;
    snapshot.frame_seq = id;
    snapshot.frame_unix_ms = 10000 + id;
    snapshot.has_publication = true;
    snapshot.has_frame = true;
    snapshot.online_state = CH_ONLINE;
    VisionContextAccess::media(snapshot) = media;
    auto outputs = std::make_shared<LogicOutputSet>();
    outputs->set_int("detected_x", marker);
    snapshot.outputs = outputs;
    return snapshot;
}

void test_tick_keeps_evidence_after_later_publications()
{
    auto latest = publication(10, 10);
    std::vector<ChannelLogicSnapshot> sampled{latest};
    GlobalContext context;
    VisionContextAccess::bind_global(context, &sampled, nullptr, nullptr, nullptr);
    GlobalLogicConfig config;
    config.instance_id = "controller";
    context.config = &config;
    DrawCommand own{};
    own.type = DrawCommand::RECT;
    own.rect.x = 40;
    DrawCommand other = own;
    other.rect.x = 60;
    VisionContextAccess::layers(sampled[0])["controller"] = {own};
    VisionContextAccess::layers(sampled[0])["other"] = {other};
    own.rect.x = 45;
    VisionContextAccess::overlays(context)[0] = {own};
    std::thread producer([&] {
        for (int id = 11; id <= 30; ++id)
            latest = publication(id, static_cast<unsigned char>(id));
    });
    producer.join();

    ChannelFrameSnapshot captured;
    assert(context.get_channel_frame_snapshot(0, &captured));
    assert(latest.frame_seq == 30);
    assert(captured.logic.frame_seq == 10);
    assert(captured.logic.frame_unix_ms == 10010);
    assert(captured.frame.at<cv::Vec3b>(0, 0)[0] == 10);
    assert(captured.results[0].frame_id == 10);
    assert(captured.results[0].box.x == 10);
    assert(ChannelInput(&sampled[0]).get_int("detected_x") == captured.results[0].box.x);
    assert(captured.draw_cmds[0].rect.x == 10);
    assert(captured.draw_cmds.size() == 3);
    assert(captured.draw_cmds[1].rect.x == 45); // 当前实例使用本 tick 的新叠加层。
    assert(captured.draw_cmds[2].rect.x == 60); // 其它实例使用采样时冻结的层。
    assert(VisionContextAccess::layers(sampled[0])["controller"][0].rect.x == 40);
    assert(captured.rois[0].name == "roi_10");
    assert(!VisionContextAccess::media(captured.logic)); // 图片任务不再保留解码源。

    captured.frame.setTo(cv::Scalar(99, 99, 99));
    captured.results[0].boxMask.setTo(cv::Scalar(99));
    ChannelFrameSnapshot again;
    assert(context.get_channel_frame_snapshot(0, &again));
    assert(again.frame.at<cv::Vec3b>(0, 0)[0] == 10);
    assert(again.results[0].boxMask.at<unsigned char>(0, 0) == 10);
    VisionContextAccess::overlays(context)[0].clear();
    assert(context.get_channel_frame_snapshot(0, &again));
    assert(again.draw_cmds.size() == 2); // 清掉当前实例的层，不影响其它实例。
    assert(again.draw_cmds[1].rect.x == 60);

    latest = ChannelLogicSnapshot{}; // 断流/状态清空也不会覆盖已采样的证据。
    assert(context.get_channel_frame_snapshot(0, &again));
    assert(again.logic.frame_seq == 10);
    assert(!context.get_channel_frame_snapshot(1, &again));
    sampled[0] = ChannelLogicSnapshot{};
    sampled[0].channel_id = 0;
    assert(!context.get_channel_frame_snapshot(0, &again));
}

void test_decoder_pool_cannot_reuse_owned_buffer()
{
    GstBufferPool *pool = gst_buffer_pool_new();
    GstStructure *config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, nullptr, 256, 1, 1);
    assert(gst_buffer_pool_set_config(pool, config));
    assert(gst_buffer_pool_set_active(pool, TRUE));
    GstBuffer *buffer = nullptr;
    assert(gst_buffer_pool_acquire_buffer(pool, &buffer, nullptr) == GST_FLOW_OK);
    auto owner = retain_frame_source_buffer(buffer);
    auto frame = std::make_shared<LazyVideoFrame>(0, nullptr, 1, 1, 1, 1, 0x0D, 1, 1, nullptr, owner);
    owner.reset();
    gst_buffer_unref(buffer); // 模拟 appsink 回调结束。

    GstBufferPoolAcquireParams acquire{};
    acquire.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT;
    GstBuffer *next = nullptr;
    assert(gst_buffer_pool_acquire_buffer(pool, &next, &acquire) != GST_FLOW_OK);
    assert(next == nullptr);
    frame.reset(); // 最后一个证据帧引用释放后，池可以复用该 buffer。
    assert(gst_buffer_pool_acquire_buffer(pool, &next, &acquire) == GST_FLOW_OK);
    gst_buffer_unref(next);
    assert(gst_buffer_pool_set_active(pool, FALSE));
    gst_object_unref(pool);
    assert(!retain_frame_source_buffer(nullptr));
}

void test_overlays_use_detection_coordinates()
{
    AlgoResult detection;
    detection.box = cv::Rect(20, 70, 30, 30);
    detection.track_id = 1;
    std::vector<AlgoResult> results{detection};
    RenderParams params;
    params.inputW = params.inputH = 160;
    params.show_fps = 0;
    params.results = &results;
    for (auto target : {DrawCommand::DISPLAY, DrawCommand::IMAGE, DrawCommand::VIDEO})
    {
        params.target_mask = target;
        for (float infer_fps : {1.0f, 10.0f, 60.0f})
        {
            params.infer_fps = infer_fps;
            cv::Mat overlay = cv::Mat::zeros(320, 320, CV_8UC3);
            render_overlays(overlay, params);
            assert(overlay.at<cv::Vec3b>(180, 40)[1] == 255);
            assert(overlay.at<cv::Vec3b>(180, 100)[1] == 255);
            assert(overlay.at<cv::Vec3b>(180, 160)[1] == 0);
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_tick_keeps_evidence_after_later_publications();
    test_decoder_pool_cannot_reuse_owned_buffer();
    test_overlays_use_detection_coordinates();
    std::cout << "Frame snapshot, decoder buffer ownership and evidence overlay regressions passed\n";
    return 0;
}
