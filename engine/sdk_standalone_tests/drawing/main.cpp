// 单独测试 SDK 绘图：准备一张图片，调用绘图函数，再保存绘制结果。
#include <drawing.h>
#include <logic_session.h>
#include <rendering.h>

#include <exception>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <vector>

// 你主要修改这个函数；这里的写法也可以用在正式通道逻辑中。
void test_drawing(ChannelContext *ctx)
{
    // 坐标按 640×640 填写；颜色顺序是蓝、绿、红。
    draw_text(ctx, "SDK 绘图测试", {32, 48}, cv::Scalar(255, 255, 255), 1.0);

    // 左上角 (50,100)，宽 180、高 120，绿色边框宽 3 像素。
    draw_rect(ctx, cv::Rect(50, 100, 180, 120), cv::Scalar(0, 255, 0), 3);

    // 圆心 (440,160)，半径 60，橙色边框。
    draw_circle(ctx, {440, 160}, 60, cv::Scalar(0, 165, 255), 3);

    draw_line(ctx, {40, 270}, {600, 270}, cv::Scalar(255, 255, 0), 2);

    // 先半透明填色，再画封闭的边框。
    std::vector<cv::Point> points{{80, 360}, {220, 320}, {260, 500}, {70, 500}};
    draw_poly_filled(ctx, points, cv::Scalar(255, 120, 0), 0.35);
    draw_polyline(ctx, points, cv::Scalar(255, 120, 0), 2, 1.0, true);

    draw_text(ctx, "修改坐标和颜色后，重新编译查看效果", {32, 580});
}

int main()
{
    try
    {
        if (!text_overlay_available())
        {
            std::cerr << "无法加载字体，请把 RK_OVERLAY_FONT 设置为字体文件路径。\n";
            return 1;
        }

        // 1. 准备深灰色底图；image 要保留到 session 使用结束。
        cv::Mat image(640, 640, CV_8UC3, cv::Scalar(32, 32, 32));

        // 2. 让 SDK 准备 ctx，并调用上面的 test_drawing。
        vision::LogicSessionOptions options;
        options.logic = test_drawing;
        vision::LogicSession session(options);
        vision::LogicFrame frame;
        frame.source = &image;
        session.run(frame);

        // 3. draw_rect 等函数先记录要画的内容，这里把它们真正画到图片上。
        vision::OverlayOptions overlays;
        overlays.inputW = overlays.inputH = 640;
        overlays.show_fps = 1;
        overlays.show_system_overlays = false;
        overlays.target_mask = DrawCommand::IMAGE;
        overlays.draw_cmds = &session.drawing();
        cv::Mat result = image.clone();
        vision::render_overlays(result, overlays);

        // 4. 保存到运行命令时所在的文件夹。
        if (!cv::imwrite("drawing-result.png", result))
        {
            std::cerr << "保存图片失败。\n";
            return 1;
        }
        std::cout << "已生成 drawing-result.png\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "绘图测试失败：" << error.what() << '\n';
        return 1;
    }
}
