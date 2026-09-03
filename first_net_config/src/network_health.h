#ifndef FIRST_NET_CONFIG_NETWORK_HEALTH_H
#define FIRST_NET_CONFIG_NETWORK_HEALTH_H

#include <stdbool.h>

/*
 * 检查 IPv4 默认路由是否被无载波的网卡抢占。
 * quiet_when_healthy 为 true 时仅在发现问题时输出，适合启动自检。
 */
void manage_default_route_health(bool quiet_when_healthy);

#endif
