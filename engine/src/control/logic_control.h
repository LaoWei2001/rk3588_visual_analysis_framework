#pragma once

#include <string>
#include <vector>

#include "logic/core/channel_logic.h"

int logic_control_init(void);
void logic_control_deinit(void);
void logic_control_take_channel(int channel_id, std::vector<LogicAction> &out);
void logic_control_take_global(const std::string &instance_id, std::vector<LogicAction> &out);

/** 设置指定通道的运行时推理开关(绝对设置, 非 toggle)。
 *  enable=true  → 恢复 NPU 推理(下一帧生效)
 *  enable=false → 暂停 NPU 推理(画面仍正常显示)
 *  线程安全, 可从任意 logic 函数内调用。 */
void logic_control_set_channel_inference(int channel_id, bool enable);

/** 读取指定通道的运行时推理开关当前状态。 */
int logic_control_get_channel_inference(int channel_id);
