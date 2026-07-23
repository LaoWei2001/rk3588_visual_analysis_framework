#pragma once

#include <vector>

#include "logic/core/channel_logic.h"

int channel_control_init(void);
void channel_control_deinit(void);
void channel_control_take(int channel_id, std::vector<ChannelAction> &out);
