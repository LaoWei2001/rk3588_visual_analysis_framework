/**
 * @file logic_lesson2.cpp
 * @brief logic_lesson2 —— 展示如何将基础图形/文字叠加在640x640的视频画面上。
 */
#include "logic_common.h"
#include <thread>  // 必须包含
#include <chrono>  // 必须包含时间库

static void logic_lesson2(ChannelContext *ctx)
{
    // 拿到可写的 640×640 BGR 显示画布(首次调用 = 当前帧的副本)
    cv::Mat &img = ctx->display_canvas();

    // 随意改像素 —— 任意 cv:: 操作
    cv::GaussianBlur(img, img, cv::Size(0, 0), 3.0);
    cv::line(img, cv::Point(0,0), cv::Point(200,100), cv::Scalar(0, 0, 255), 1);
    cv::line(img, cv::Point(0,0), cv::Point(638,638), cv::Scalar(255, 0, 255), 1);
    cv::cvtColor(img, img, cv::COLOR_BGR2GRAY);
    cv::threshold(img, img, 127, 255, cv::THRESH_BINARY);
    cv::line(img, cv::Point(320,1), cv::Point(320,320), cv::Scalar(0, 255, 255), 1);
}
REGISTER_LOGIC("logic_lesson2", logic_lesson2);
