#ifndef FIRST_NET_CONFIG_NETWORK_SAFETY_H
#define FIRST_NET_CONFIG_NETWORK_SAFETY_H

#include "netconfig_types.h"

#include <stdbool.h>
#include <stddef.h>

int configure_ipv4_profile(const char *profile_uuid, IPv4Config *cfg);
NetworkActivationResult safe_activate_with_reconnect(
    const char *iface,
    const ConnectionProfile *profile,
    const IPv4Config *cfg,
    const char *final_profile_name,
    NetworkProfileMode profile_mode);
bool handle_pending_network_change(void);
int run_pending_network_watchdog(const char *transaction_id);
bool finalize_profile(const ConnectionProfile *temp_profile,
                      const char *final_profile_name,
                      const ConnectionProfile *previous_profile);
void cleanup_temp_profile(const ConnectionProfile *temp_profile);
void ask_final_profile_name(const char *default_name, char *out, size_t out_size);
bool build_temp_profile(const char *iface, ConnectionProfile *out);
bool bind_created_profile_uuid(ConnectionProfile *profile);

#endif
