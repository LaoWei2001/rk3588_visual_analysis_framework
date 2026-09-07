#ifndef FIRST_NET_CONFIG_CAMERA_DISCOVERY_H
#define FIRST_NET_CONFIG_CAMERA_DISCOVERY_H

#include <stddef.h>

#define MAX_DISCOVERED_CAMERAS 32

typedef struct
{
    char ip[16];
    char subnet_mask[16];
    char gateway[16];
    char mac[32];
    char model[128];
    char serial[96];
    int http_port;
    int rtsp_port;
    int prefix;
} DiscoveredCamera;

int discover_cameras(const char *iface,
                     DiscoveredCamera *cameras,
                     size_t capacity,
                     int timeout_seconds);
int parse_camera_discovery_payload(const char *payload,
                                   const char *sender_ip,
                                   DiscoveredCamera *camera);
int choose_or_enter_camera(const char *iface, DiscoveredCamera *camera);

#endif
