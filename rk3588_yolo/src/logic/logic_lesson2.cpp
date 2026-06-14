/**
 * @file logic_lesson2.cpp
 * @brief logic_lesson2 —— 展示如何将自定义的图形/文字叠加在画面上显示。
 */
#include "logic_common.h"

static void logic_lesson2(ChannelContext *ctx)
{
    
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行, 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson2", logic_lesson2);
