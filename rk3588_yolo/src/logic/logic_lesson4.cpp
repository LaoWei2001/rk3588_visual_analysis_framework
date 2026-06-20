/**
 * @file logic_lesson4.cpp
 * @brief logic_lesson4 —— 展示如何绘制ROI区域, 并将ROI区域应用于算法编排。
 */
#include "logic_common.h"

static void logic_lesson4(ChannelContext *ctx)
{
    int roi_num = ctx->roi_count();
    printf("当前id为%d的视频流中设置了%d个ROI区域\n", ctx->chnId, roi_num);
    for (int i = 0; i < roi_num; i++)
    {
        const char *roi_name = ctx->roi_name_at(i);
        printf("第%d个区域的名字为：%s\n", i, roi_name);
    }
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行, 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson4", logic_lesson4);
