/**
 * @file logic_common.h
 * @brief 各通道逻辑模块共用的头文件汇总
 *
 * 每个 logic 模块只需 #include "logic/core/logic_common.h" 即可拿到:
 *   - ChannelContext / param_* / draw_* / RoiZone / RenderParams / REGISTER_LOGIC (channel_logic.h)
 *   - AlgoResult / render_overlays / ChannelSnapshot 等 (经 algoProcess.h / app_ctrl.h)
 *   - 通用告警上报接口 (alarm_report)
 *   - 中英文文字渲染 (draw_text_unicode)
 */
#pragma once

#include <opencv2/opencv.hpp>

#include "channel_logic.h"
#include "analyzer/algoProcess.h"
#include "core/app_ctrl.h"
#include "alarm/alarm_report.h"
#include "player/text_overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
