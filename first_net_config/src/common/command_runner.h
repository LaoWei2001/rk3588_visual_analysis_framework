#ifndef FIRST_NET_CONFIG_COMMAND_RUNNER_H
#define FIRST_NET_CONFIG_COMMAND_RUNNER_H

#include <stddef.h>

int run_cmd(const char *const argv[]);
int run_cmd_silent(const char *const argv[]);
int capture_cmd(const char *const argv[], char *out, size_t out_size);

#endif
