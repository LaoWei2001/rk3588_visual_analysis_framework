/**
 * @file logic_lesson1.cpp
 * @brief logic_lesson1 —— 展示如何获取视频流信息。
 */
#include "logic_common.h"

static void logic_lesson1(ChannelContext *ctx)
{
    printf("--------------------------\n");
    printf("当前视频流信息：\n");

    printf("视频流所属的通道号为：%d\n", ctx->chnId);
    printf("视频流类型为：%s\n", ctx->config->stream.src_type.c_str());
    // 或者 std::cout << "当前视频流类型为：" << ctx->config->stream.src_type << "\n";
    printf("是否启用yolo推理：%d\n", ctx->infer_enabled);
    // cols:列数(x轴),rows:行数(y轴)
    printf("帧分辨率为：%d x %d\n", ctx->frame->cols, ctx->frame->rows);
    printf("本通道使用的逻辑函数为：%s\n", ctx->config->logic.c_str());
    printf("当前帧的帧号为：%ld\n", ctx->frame_id);
    printf("显示的帧率为：%f FPS\n", ctx->disp_fps);
    // 时间戳为系统的开机运行时长(ms)
    printf("当前帧的时间戳为：%lu ms\n", ctx->timestamp_ms);
    printf("当前一帧距离上一帧经过了%f ms\n", ctx->dt_ms);
}

// 每一个实现逻辑函数的logic_xxx.cpp文件最底部都必须添加这一行, 完成自行编写的逻辑的注册。
REGISTER_LOGIC("logic_lesson1", logic_lesson1);
