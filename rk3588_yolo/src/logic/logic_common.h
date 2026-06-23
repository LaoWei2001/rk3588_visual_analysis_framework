/**
 * @file logic_common.h
 * @brief 各通道逻辑(logic_xxx.cpp)共用的头文件汇总
 *
 * 每个 logic 文件只需 #include "logic_common.h" 即可拿到:
 *   - ChannelContext / draw_* / RoiZone / RenderParams / REGISTER_LOGIC
 * (channel_logic.h)
 *   - AlgoResult / render_overlays / ChannelSnapshot 等 (经 algoProcess.h /
 * app_ctrl.h)
 *   - 共享状态结构体与算法 (logic_tools.h: HookState / PersonAlarmState /
 *     DifyPersonVerifyState / logic_roll_compute_occupancy ...)
 *   - 上报接口 (alarm_uploader_enqueue / dify_uploader_enqueue)
 *   - 中英文文字渲染 (draw_text_unicode)
 */
#pragma once

#include <opencv2/opencv.hpp>

#include "channel_logic.h"
#include "logic_tools.h"

#include "../analyzer/algoProcess.h"
#include "../core/app_ctrl.h"
#include "../player/text_overlay.h"
#include "../uploader/alarm_uploader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* 是否允许上报: 画布上连了"上报配置"节点时才为 true。
 * 把它作为上报函数的 report_enable 参数传进去即可(函数内部 false 时自动跳过):
 *     alarm_uploader_enqueue(img, raw, ctx->chnId, "type", report_enabled(ctx),
 * url); dify_uploader_enqueue(img, prompt, event_id, report_enabled(ctx), url,
 * key); 没连节点 → 不上报。这样"上报配置"节点才是真正的开关, 而不是摆设。 */
static inline bool report_enabled(const ChannelContext *ctx)
{
    return ctx && ctx->config && ctx->config->report_enable;
}
