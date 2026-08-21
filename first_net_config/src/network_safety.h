#ifndef FIRST_NET_CONFIG_NETWORK_SAFETY_H
#define FIRST_NET_CONFIG_NETWORK_SAFETY_H

#include "netconfig_types.h"

#include <stdbool.h>
#include <stddef.h>

void network_safety_set_checkpoint_supported(bool supported);
int configure_ipv4_profile(const char *profile_uuid, IPv4Config *cfg);
bool safe_activate_with_checkpoint(const char *iface,
                                   const ConnectionProfile *profile,
                                   const IPv4Config *cfg);
bool finalize_profile(const ConnectionProfile *temp_profile,
                      const char *final_profile_name);
void cleanup_temp_profile(const ConnectionProfile *temp_profile);
void ask_final_profile_name(const char *iface, char *out, size_t out_size);
bool build_temp_profile(const char *iface, ConnectionProfile *out);

#endif
