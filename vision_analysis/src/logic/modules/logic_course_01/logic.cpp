/* 本项目设计了若干节基础教程 logic_course_xx 帮助开发者快速上手本项目中视觉程序的开发 */
/*
    注意: 尽管本算法引擎的设计已经极大降低开发的难度，但是对于开发者来说需要
    具备以下基础：
    1. 具备 C/C++ 基础知识。
    2. 熟悉指针、const及结构体成员访问，能够判断和处理空指针。
    3. 了解 C++常用标准库，包括 std::vector、std::string和 std::shared_ptr。
    4. 了解 YOLO 目标检测的基本概念，包括检测框、类别、置信度、类别编号。
    5. 了解 OpenCV 常用类型和函数，包括 cv::Mat、cv::Rect、cv::Point、cv::Scalar及常用绘制、图像处理函数。
    6. 理解图像坐标系、检测框坐标、ROI 多边形以及 OpenCV 的 BGR 颜色顺序。
    7. 掌握 Linux 常用命令，能够完成文件操作、程序运行、进程查看和日志排查。
    8. 了解基本的 C/C++编译流程。
    9. 能够阅读和修改基础 JSON 配置文件。
*/

// 课程1：自定义文字和图形叠加
// 实现效果:在屏幕上显示出文字和图形
// 难度:★☆☆☆☆
#include "logic/core/logic_common.h"

static void logic_course_01(ChannelContext *ctx)
{
    // 空指针验证
    if (!ctx || !ctx->frame || ctx->frame->empty())
        return;

    // 叠加自定义文字, 支持中文
    draw_text(ctx, "course1 logic running, 正在运行", cv::Point(30, 50), cv::Scalar(255, 0, 0), 1, 1, DrawCommand::ALL);
    // 画一个圆
    draw_circle(ctx, cv::Point(320, 320), 100, cv::Scalar(255, 0, 0));
}

REGISTER_LOGIC(logic_course_01);
