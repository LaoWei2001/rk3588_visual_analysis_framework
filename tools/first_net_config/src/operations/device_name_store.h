#ifndef FIRST_NET_CONFIG_DEVICE_NAME_STORE_H
#define FIRST_NET_CONFIG_DEVICE_NAME_STORE_H

#include <stdbool.h>
#include <stddef.h>

bool device_name_read_file(const char *path, char *value, size_t value_size);
bool device_name_write_file(const char *path, const char *device_name);
bool device_name_update_hosts(const char *path, const char *device_name);

#endif
