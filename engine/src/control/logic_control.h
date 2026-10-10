#pragma once

#include <string>
#include <vector>

#include <channel.h>
#include <control.h>

int logic_control_init(void);
void logic_control_deinit(void);
void logic_control_take_channel(int channel_id, std::vector<LogicAction> &out);
void logic_control_take_global(const std::string &instance_id, std::vector<LogicAction> &out);
