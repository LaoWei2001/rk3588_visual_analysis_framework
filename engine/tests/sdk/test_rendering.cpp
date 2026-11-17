#include <cassert>
#include <iostream>
#include <rendering.h>

int main()
{
    // This executable links only the inference SDK, with no application context.
    cv::Mat image(100, 200, CV_8UC3, cv::Scalar(20, 40, 60));
    DrawCommand image_only;
    image_only.type = DrawCommand::RECT;
    image_only.target = DrawCommand::IMAGE;
    image_only.rect = {10, 20, 20, 30};
    image_only.thickness = -1;
    image_only.color = {100, 120, 140};
    image_only.alpha = 0.5;
    DrawCommand display_only = image_only;
    display_only.target = DrawCommand::DISPLAY;
    display_only.rect = {60, 20, 20, 30};
    std::vector<DrawCommand> commands{image_only, display_only};

    vision::OverlayOptions options;
    options.inputW = options.inputH = 100;
    options.show_fps = 0;
    options.target_mask = DrawCommand::IMAGE;
    options.draw_cmds = &commands;
    vision::render_overlays(image, options);
    assert(image.at<cv::Vec3b>(30, 40) == cv::Vec3b(60, 80, 100));
    assert(image.at<cv::Vec3b>(30, 140) == cv::Vec3b(20, 40, 60));
    assert(image.at<cv::Vec3b>(70, 40) == cv::Vec3b(20, 40, 60));
    assert(commands[0].rect == cv::Rect(10, 20, 20, 30));

    // Turning off business drawing leaves the destination unchanged.
    const cv::Mat original = image.clone();
    options.show_custom_overlays = false;
    options.target_mask = DrawCommand::ALL;
    vision::render_overlays(image, options);
    assert(cv::norm(image, original, cv::NORM_INF) == 0);

    // Depth-only presentation does not leak source pixels while awaiting results.
    options.depth_background = true;
    vision::render_overlays(image, options);
    assert(cv::norm(image, cv::NORM_INF) == 0);

    // Unversioned callers may reuse and modify the same depth buffer in place.
    std::vector<AlgoResult> results(1);
    results[0].depth_map = cv::Mat(16, 16, CV_32FC1);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            results[0].depth_map.at<float>(y, x) = float(x + 1);
    options.results = &results;
    vision::render_overlays(image, options);
    const cv::Mat first_depth = image.clone();
    assert(cv::norm(first_depth, cv::NORM_INF) > 0);
    results[0].depth_map.setTo(0.0f);
    vision::render_overlays(image, options);
    assert(cv::norm(image, cv::NORM_INF) == 0);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            results[0].depth_map.at<float>(y, x) = float(16 - x);
    vision::render_overlays(image, options);
    assert(cv::norm(image, first_depth, cv::NORM_INF) > 0);

    // All segmentation models contribute to the image, including cached results and reordered inputs.
    std::vector<AlgoResult> masks(2);
    masks[0].model_id = "left";
    masks[1].model_id = "right";
    for (auto &result : masks)
        result.boxMask = cv::Mat(100, 200, CV_8UC1, cv::Scalar(0));
    masks[0].boxMask(cv::Rect(10, 10, 20, 20)).setTo(1);
    masks[1].boxMask(cv::Rect(150, 10, 20, 20)).setTo(2);
    vision::OverlayOptions segmentation;
    segmentation.results = &masks;
    segmentation.inputW = 200;
    segmentation.inputH = 100;
    segmentation.show_fps = 0;
    segmentation.result_frame_id = 42;
    for (int pass = 0; pass < 3; ++pass)
    {
        image.setTo(0);
        vision::render_overlays(image, segmentation);
        assert(cv::norm(image(cv::Rect(10, 10, 20, 20)), cv::NORM_INF) > 0);
        assert(cv::norm(image(cv::Rect(150, 10, 20, 20)), cv::NORM_INF) > 0);
        assert(cv::norm(image(cv::Rect(50, 50, 20, 20)), cv::NORM_INF) == 0);
        if (pass == 1)
            std::swap(masks[0], masks[1]);
    }
    std::cout << "Standalone renderer: coordinates, target selection, alpha and depth background passed\n";
}
