/**
 * @file logic_lesson3_det.cpp
 * @brief logic_lesson3_det —— 展示如何将yolo推理(目标检测)应用于视频流后,
 * 获取检测结果。
 */
#include "logic_common.h"

static void logic_lesson3_det(ChannelContext *ctx)
{
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行,
// 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson3_det", logic_lesson3_det);
