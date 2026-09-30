#ifndef FIRST_NET_CONFIG_NETWORK_STATE_H
#define FIRST_NET_CONFIG_NETWORK_STATE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    ETHERNET_CARRIER_UNKNOWN = -1,
    ETHERNET_CARRIER_DOWN = 0,
    ETHERNET_CARRIER_UP = 1
} EthernetCarrierState;

typedef enum
{
    NEARBY_WIFI_CHOICE_CANCELLED = -1,
    NEARBY_WIFI_CHOICE_MANUAL = 0,
    NEARBY_WIFI_CHOICE_SELECTED = 1
} NearbyWifiChoiceResult;

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
EthernetCarrierState get_ethernet_carrier_state(const char *ifname);
bool choose_ethernet_interface(char *iface, size_t iface_size);
bool choose_wifi_interface(char *iface, size_t iface_size);
void ask_interface(char *iface, size_t size, const char *default_iface);
void show_devices(void);
void show_network_state(void);
void scan_wifi(void);
NearbyWifiChoiceResult choose_nearby_wifi_ssid(const char *ifname,
                                               char *ssid,
                                               size_t ssid_size);
bool nmcli_available(void);

#endif
