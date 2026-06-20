/**
 * @file logic_lesson9.cpp
 * @brief logic_lesson9 —— 展示如何编写全局逻辑，对多个视频流通道的检测结果进行分析。
 */
#include "logic_common.h"

static void logic_lesson9(ChannelContext *ctx)
{
    
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行, 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson9", logic_lesson9);
