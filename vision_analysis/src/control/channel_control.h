#pragma once

#include <vector>

#include "logic/core/channel_logic.h"

int channel_control_init(void);
void channel_control_deinit(void);
void channel_control_take(int channel_id, std::vector<ChannelAction> &out);

/** 设置指定通道的运行时推理开关(绝对设置, 非 toggle)。
 *  enable=true  → 恢复 NPU 推理(下一帧生效)
 *  enable=false → 暂停 NPU 推理(画面仍正常显示)
 *  线程安全, 可从任意 logic 函数内调用。 */
void channel_control_set_infer_enable(int channel_id, bool enable);

/** 读取指定通道的运行时推理开关当前状态。 */
int channel_control_get_infer_enable(int channel_id);
