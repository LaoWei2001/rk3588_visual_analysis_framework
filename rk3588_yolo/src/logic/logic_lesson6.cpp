/**
 * @file logic_lesson6.cpp
 * @brief logic_lesson6 —— 展示如何编写需要应用跨帧状态的算法。
 */
#include "logic_common.h"

static void logic_lesson6(ChannelContext *ctx)
{
    
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行, 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson6", logic_lesson6);
