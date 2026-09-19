#ifndef FIRST_NET_CONFIG_NETWORK_HEALTH_H
#define FIRST_NET_CONFIG_NETWORK_HEALTH_H

#include <stdbool.h>

/*
 * 检查 IPv4 默认路由是否被无载波的网卡抢占。
 * quiet_when_healthy 为 true 时仅在发现问题时输出，适合启动自检。
 */
void manage_default_route_health(bool quiet_when_healthy);

/*
 * 菜单中的统一健康检查：检查/修复默认路由，并检查网段冲突；
 * 所有配置变更均由用户选择目标和参数后确认执行。
 */
void manage_network_health(void);

#endif
