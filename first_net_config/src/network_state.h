#ifndef FIRST_NET_CONFIG_NETWORK_STATE_H
#define FIRST_NET_CONFIG_NETWORK_STATE_H

#include <stdbool.h>
#include <stddef.h>

bool generate_connection_uuid(char *out, size_t out_size);
int find_connection_uuid_by_name(const char *name, char *out, size_t out_size);
bool resolve_connection_uuid(const char *selector, char *out, size_t out_size);
bool get_connection_name(const char *uuid, char *out, size_t out_size);
bool connection_is_active(const char *uuid);
int delete_connection_by_uuid(const char *uuid);
bool has_any_ipv4(const char *ifname);
bool interface_has_ipv4(const char *ifname, const char *expected_ip);
bool get_active_connection_uuid(const char *ifname, char *out, size_t out_size);
bool default_route_matches(const char *ifname, const char *gateway);
bool choose_ethernet_interface(char *iface, size_t iface_size);
void ask_interface(char *iface, size_t size, const char *default_iface);
void show_devices(void);
void show_network_state(void);
void scan_wifi(void);
bool nmcli_available(void);
bool checkpoint_supported(void);

#endif
