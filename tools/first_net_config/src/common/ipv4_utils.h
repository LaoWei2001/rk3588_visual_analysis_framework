#ifndef FIRST_NET_CONFIG_IPV4_UTILS_H
#define FIRST_NET_CONFIG_IPV4_UTILS_H

#include "common/netconfig_types.h"

#include <stdbool.h>
#include <stddef.h>

bool valid_ipv4(const char *s);
bool normalize_ipv4_list(const char *input, char *output, size_t output_size,
                         int max_items);
bool read_ipv4_required(const char *prompt, char *output, size_t output_size);
bool read_ipv4_optional(const char *prompt, char *output, size_t output_size);
bool confirm_network_parameters(const char *iface, const IPv4Config *cfg);
int netmask_to_prefix(const char *mask);
bool ipv4_same_subnet(const char *ip_a, const char *ip_b, int prefix);
bool is_network_or_broadcast_address(const char *ip, int prefix);

#endif
