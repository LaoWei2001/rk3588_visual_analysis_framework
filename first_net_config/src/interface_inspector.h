#ifndef FIRST_NET_CONFIG_INTERFACE_INSPECTOR_H
#define FIRST_NET_CONFIG_INTERFACE_INSPECTOR_H

#include <stdbool.h>

void show_interface_overview(void);
int show_current_overlap_warnings(void);
bool approve_candidate_network(const char *selected_iface,
                               const char *ip,
                               int prefix);
bool interface_has_default_route(const char *iface);
bool interface_is_ssh_path(const char *iface);
bool ipv4_networks_overlap(const char *left_ip, int left_prefix,
                           const char *right_ip, int right_prefix);

#endif
