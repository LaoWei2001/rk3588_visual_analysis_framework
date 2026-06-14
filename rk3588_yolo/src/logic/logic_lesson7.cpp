/**
 * @file logic_lesson7.cpp
 * @brief logic_lesson7 —— 展示如何在通道逻辑函数中添加上报服务器的功能。
 */
#include "logic_common.h"

static void logic_lesson7(ChannelContext *ctx)
{
    
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行, 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson7", logic_lesson7);
