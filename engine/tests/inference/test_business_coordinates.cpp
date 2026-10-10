#include "display/display.h"
#include "inference/inference_internal.h"
#include "inference/inference_result_mapping.h"
#include "inference/inference_roi_filter.h"
#include "logic/core/context_access.h"
#include "logic/core/global_logic.h"
#include "pipeline/frame_transform.h"
#include "pipeline/image_convert.h"
#include "runtime/app_ctrl.h"
#include <channel.h>
#include <coordinates.h>
#include <rga/im2d.h>

#include <cassert>
#include <cmath>
#include <iostream>
#include <type_traits>

// 独立渲染验证不启动配置监视器或性能文字。
extern "C" int app_ctrl_get_performance_display()
{
    return 0;
}

namespace
{
AlgoResult detection(const cv::Rect &box)
{
    AlgoResult result;
    result.box = box;
    result.keypoints = {cv::Point2f(box.x + box.width / 2.0f, box.y + box.height / 2.0f), cv::Point2f(-1, -1)};
    return result;
}

void expect_box(const cv::Rect &actual, const cv::Rect &expected)
{
    // 包围框用 floor/ceil 保留边缘，浮点缩放最多允许一个业务像素的取整差。
    assert(std::abs(actual.x - expected.x) <= 1);
    assert(std::abs(actual.y - expected.y) <= 1);
    assert(std::abs(actual.br().x - expected.br().x) <= 1);
    assert(std::abs(actual.br().y - expected.br().y) <= 1);
}

void test_fixed_contract_and_model_switch()
{
    static_assert(std::is_const<decltype(InferenceRuntime::input_w)>::value,
                  "model load/reload must not change business width");
    static_assert(InferenceRuntime::input_w == 640 && InferenceRuntime::input_h == 640,
                  "business dimensions are a stable contract");
    ChannelContext channel{};
    GlobalContext global{};
    APP_CTRL control{};
    assert(channel.business_center() == cv::Point(320, 320));
    assert(global.business_center() == channel.business_center());
    assert(channel.business_size() == cv::Size(640, 640));
    assert(global.model_width == 640 && global.model_height == 640);
    assert(control.inputW == 640 && control.inputH == 640);

    // 相同取景在不同模型下依次输入，包含切回 640、非正方形输入以及改变顺序。
    for (const cv::Size model :
         {cv::Size(960, 960), cv::Size(640, 640), cv::Size(1280, 736), cv::Size(640, 640), cv::Size(960, 960)})
    {
        std::vector<AlgoResult> results{
            detection(cv::Rect(model.width / 4, model.height / 4, model.width / 2, model.height / 2))};
        const InferenceRoiTransform full{cv::Rect(0, 0, 1920, 1080), cv::Rect(0, 0, model.width, model.height)};
        map_results_to_business_frame(1920, 1080, full, model.width, model.height, results);
        expect_box(results[0].box, cv::Rect(160, 160, 320, 320));
        assert(cv::norm(results[0].keypoints[0] - cv::Point2f(320, 320)) < .01);
        assert(results[0].keypoints[1] == cv::Point2f(-1, -1));
        assert(channel.business_center() == cv::Point(320, 320));
    }
}

void test_identity_reuses_mask_and_preserves_box()
{
    std::vector<AlgoResult> results{detection(cv::Rect(160, 160, 320, 320))};
    results[0].boxMask = cv::Mat(640, 640, CV_8UC1, cv::Scalar(7));
    const auto *original = results[0].boxMask.data;
    const InferenceRoiTransform full{cv::Rect(0, 0, 1920, 1080), cv::Rect(0, 0, 640, 640)};
    map_results_to_business_frame(1920, 1080, full, 640, 640, results);
    assert(results[0].box == cv::Rect(160, 160, 320, 320));
    assert(results[0].boxMask.data == original); // 身份变换不创建、复制或缩放像素。
    assert(results[0].boxMask.at<unsigned char>(320, 320) == 7);
}

void test_resized_and_shared_masks()
{
    cv::Mat mask(960, 960, CV_8UC1, cv::Scalar(0));
    mask(cv::Rect(240, 240, 480, 480)).setTo(9);
    std::vector<AlgoResult> results{detection(cv::Rect(240, 240, 480, 480)), detection(cv::Rect(300, 300, 120, 120))};
    for (auto &result : results)
        result.boxMask = mask;
    const InferenceRoiTransform full{cv::Rect(0, 0, 1920, 1080), cv::Rect(0, 0, 960, 960)};
    map_results_to_business_frame(1920, 1080, full, 960, 960, results);
    assert(results[0].boxMask.size() == cv::Size(640, 640));
    assert(results[0].boxMask.data == results[1].boxMask.data); // 同一分割掩码只缩放一次。
    assert(results[0].boxMask.at<unsigned char>(320, 320) == 9);
    assert(results[0].boxMask.at<unsigned char>(80, 80) == 0);
    assert(mask.size() == cv::Size(960, 960) && mask.at<unsigned char>(480, 480) == 9);
}

void test_crop_padding_and_roi_filter()
{
    const cv::Rect selection(480, 270, 960, 540);
    for (int model_size : {640, 960})
        for (const std::string mode : {"stretch", "letterbox", "expand"})
        {
            const auto transform =
                make_inference_roi_transform(selection, 1920, 1080, model_size, model_size, mode, true);
            std::vector<AlgoResult> results{detection(transform.model_content_rect)};
            results[0].boxMask = cv::Mat(model_size, model_size, CV_8UC1, cv::Scalar(5));
            map_results_to_business_frame(1920, 1080, transform, model_size, model_size, results);
            const auto &source = transform.source_rect;
            const int left = static_cast<int>(std::floor(source.x * (640.0 / 1920)));
            const int top = static_cast<int>(std::floor(source.y * (640.0 / 1080)));
            const int right = static_cast<int>(std::ceil((source.x + source.width) * (640.0 / 1920)));
            const int bottom = static_cast<int>(std::ceil((source.y + source.height) * (640.0 / 1080)));
            expect_box(results[0].box, cv::Rect(left, top, right - left, bottom - top));
            assert(cv::norm(results[0].keypoints[0] - cv::Point2f(320, 320)) < 1.0);
            assert(results[0].boxMask.at<unsigned char>(320, 320) == 5);
            assert(results[0].boxMask.at<unsigned char>(10, 10) == 0);

            InferenceRoiConfig roi;
            roi.mode = "roi_only";
            roi.polygon = {{.25, .25}, {.75, .25}, {.75, .75}, {.25, .75}};
            results.push_back(detection(cv::Rect(0, 0, 20, 20)));
            filter_results_to_inference_roi(results, roi, 640, 640);
            assert(results.size() == 1);
            assert(results[0].boxMask.at<unsigned char>(320, 320) == 5);
            assert(results[0].boxMask.at<unsigned char>(100, 320) == 0);
        }

    const auto letterbox = make_inference_roi_transform(selection, 1920, 1080, 960, 960, "letterbox", true);
    std::vector<AlgoResult> padding{detection(cv::Rect(100, 10, 40, 40))};
    map_results_to_business_frame(1920, 1080, letterbox, 960, 960, padding);
    assert(padding[0].box.empty());
}

const cv::Mat *business_getter(void *opaque)
{
    return static_cast<LazyVideoFrame *>(opaque)->model_frame();
}

void test_lazy_business_frame_and_direct_model_input()
{
    cv::Mat source(720, 1280, CV_8UC3, cv::Scalar(10, 20, 30));
    auto frame = std::make_shared<LazyVideoFrame>(0, nullptr, source.cols, source.rows, source.cols, source.rows,
                                                  RK_FORMAT_BGR_888, 640, 640, source.data);
    assert(frame->retain_borrowed_source(source.total() * source.elemSize()));
    ChannelContext channel{};
    VisionContextAccess::bind_frames(channel, business_getter, nullptr, frame.get());
    const auto *business = channel.business_frame();
    assert(business && business->size() == cv::Size(640, 640));
    assert(channel.model_frame() == business);

    cv::Mat model_input;
    assert(frame->resized_frame(960, 960, model_input));
    assert(model_input.size() == cv::Size(960, 960));
    assert(model_input.at<cv::Vec3b>(480, 480) == cv::Vec3b(10, 20, 30));
    const auto *input_pixels = model_input.data;
    assert(frame->resized_frame(960, 960, model_input));
    assert(model_input.data == input_pixels); // 同尺寸 CPU 回退跨调用复用输出内存。
    assert(channel.business_frame() == business && business->size() == cv::Size(640, 640));
    const auto *original = frame->source_frame();
    assert(original && original->size() == source.size());

    // 原图含有超过 640 画布能保留的单像素细节；960 输入必须直接保留原图，不能放大业务图。
    cv::Mat detail(960, 960, CV_8UC3);
    for (int y = 0; y < detail.rows; ++y)
        for (int x = 0; x < detail.cols; ++x)
            detail.at<cv::Vec3b>(y, x) = cv::Vec3b((x & 1) ? 255 : 0, (y & 1) ? 255 : 0, 30);
    LazyVideoFrame detailed_frame(0, nullptr, 960, 960, 960, 960, RK_FORMAT_BGR_888, 640, 640, detail.data);
    assert(detailed_frame.model_frame()->size() == cv::Size(640, 640));
    assert(detailed_frame.resized_frame(960, 960, model_input));
    assert(cv::norm(model_input, detail, cv::NORM_INF) == 0);
}

void test_display_and_evidence_overlays_use_business_coordinates()
{
    DrawCommand center{};
    center.type = DrawCommand::CIRCLE;
    center.center = cv::Point(320, 320); // 已有业务代码的固定中心无需修改。
    center.radius = 10;
    center.color = cv::Scalar(255, 0, 0);
    center.thickness = -1;
    std::vector<DrawCommand> commands{center};
    for (int model_size : {640, 960})
    {
        std::vector<AlgoResult> results{
            detection(cv::Rect(model_size / 4, model_size / 4, model_size / 2, model_size / 2))};
        const InferenceRoiTransform full{cv::Rect(0, 0, 1920, 1080), cv::Rect(0, 0, model_size, model_size)};
        map_results_to_business_frame(1920, 1080, full, model_size, model_size, results);
        RenderParams params;
        params.inputW = params.inputH = 640;
        params.results = &results;
        params.draw_cmds = &commands;
        params.show_fps = 0;
        for (auto target : {DrawCommand::DISPLAY, DrawCommand::IMAGE, DrawCommand::VIDEO})
        {
            params.target_mask = target;
            cv::Mat view = cv::Mat::zeros(720, 1280, CV_8UC3);
            render_overlays(view, params);
            assert(view.at<cv::Vec3b>(360, 640)[0] == 255); // 所有出口的业务中心仍是画面中心。
            assert(view.at<cv::Vec3b>(360, 320)[1] == 255); // 模型框与业务绘制使用同一缩放规则。
        }
    }
}

void test_software_color_formats_and_output_reuse()
{
    cv::Mat source(12, 16, CV_8UC3, cv::Scalar(10, 20, 30));
    cv::Mat output;
    for (int format : {static_cast<int>(RK_FORMAT_BGR_888), 0x0D})
    {
        assert(convert_raw_to_bgr(source.data, 10, 8, 16, 12, format, output));
        assert(output.size() == cv::Size(10, 8) && output.isContinuous());
        assert(output.at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 20, 30));
        const auto *pixels = output.data;
        assert(convert_raw_to_bgr(source.data, 10, 8, 16, 12, format, output));
        assert(output.data == pixels);
    }
    for (int format : {static_cast<int>(RK_FORMAT_RGB_888), 0x0E})
    {
        assert(convert_raw_to_bgr(source.data, 10, 8, 16, 12, format, output));
        assert(output.at<cv::Vec3b>(0, 0) == cv::Vec3b(30, 20, 10));
    }
}
} // namespace

int main()
{
    cv::setNumThreads(1);
    test_fixed_contract_and_model_switch();
    test_identity_reuses_mask_and_preserves_box();
    test_resized_and_shared_masks();
    test_crop_padding_and_roi_filter();
    test_lazy_business_frame_and_direct_model_input();
    test_display_and_evidence_overlays_use_business_coordinates();
    test_software_color_formats_and_output_reuse();
    std::cout << "Business coordinates regression tests passed\n";
}
