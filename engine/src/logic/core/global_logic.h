#pragma once

/* Internal global-logic lifecycle; business types are defined in global.h. */
#include <global.h>

int global_logic_start_all(const std::vector<GlobalLogicConfig> &cfgs);
int global_logic_reload_all(const std::vector<GlobalLogicConfig> &cfgs);
void global_logic_stop_all(void);
int global_logic_get_instance_count(void);
void *global_logic_thread_func(void *arg);
