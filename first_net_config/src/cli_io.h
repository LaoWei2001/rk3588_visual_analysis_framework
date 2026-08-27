#ifndef FIRST_NET_CONFIG_CLI_IO_H
#define FIRST_NET_CONFIG_CLI_IO_H

#include <stdbool.h>
#include <stddef.h>

#include "netconfig_types.h"

void trim_space(char *s);
void read_line(const char *prompt, char *buf, size_t size);
int read_int(const char *prompt, int min_value, int max_value);
bool read_yes_no(const char *prompt, bool default_yes);
bool read_exact_yes(const char *prompt);
bool read_exact_word(const char *prompt, const char *expected);
ConfigLifetime read_config_lifetime(void);
void read_password(const char *prompt, char *buf, size_t size);

#endif
