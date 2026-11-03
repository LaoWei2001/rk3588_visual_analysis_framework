#pragma once

#include <channel.h>
#include <global.h>

/* Engine dispatch and capability enumeration. Business uses REGISTER_* only. */
ChannelLogicFunc channel_logic_get(const char *name);
ChannelLogicActionFunc channel_logic_action_get(const char *name);
std::vector<std::string> channel_logic_names();

GlobalLogicFunc global_logic_get(const char *name);
GlobalLogicActionFunc global_logic_action_get(const char *name);
std::vector<std::string> global_logic_names();
