/**
 * @file logic_lesson5.cpp
 * @brief logic_lesson5 —— 展示如何加入可热重载的自定义参数。
 */
#include "logic_common.h"

static void logic_lesson5(ChannelContext *ctx)
{
    
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行, 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson5", logic_lesson5);
