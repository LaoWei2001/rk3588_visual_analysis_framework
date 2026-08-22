/**
 * @file logic_common.h
 * @brief 各通道逻辑模块共用的头文件汇总
 *
 * 每个 logic 模块只需 #include "logic/core/logic_common.h" 即可拿到:
 *   - ChannelContext / param_* / draw_* / RoiZone / RenderParams / REGISTER_LOGIC (channel_logic.h)
 *   - AlgoResult / render_overlays / ChannelFrameSnapshot 等 (经 inference_engine.h / app_ctrl.h)
 *   - 通用事件上报接口 (event_report)
 *   - 中英文文字渲染 (draw_text_unicode)
 */
#pragma once

#include <opencv2/opencv.hpp>

#include "event/event_report.h"
#include "inference/inference_engine.h"
#include "channel_logic.h"
#include "runtime/app_ctrl.h"
#include "display/text_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
