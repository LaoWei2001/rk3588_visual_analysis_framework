#define _POSIX_C_SOURCE 200809L

#include "network_health.h"

#include "cli_io.h"
#include "command_runner.h"
#include "interface_inspector.h"
#include "netconfig_types.h"
#include "network_state.h"
#include "saved_connections.h"

#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEFAULT_ROUTES 32
#define ROUTE_OUTPUT_SIZE 16384

typedef enum
{
    CARRIER_UNKNOWN = -1,
    CARRIER_DOWN = 0,
    CARRIER_UP = 1
} CarrierState;

typedef struct
{
    char gateway[64];
    char iface[IF_NAMESIZE];
    int metric;
    bool linkdown;
    CarrierState carrier;
} DefaultRoute;

static bool parse_nonnegative_int(const char *text, int *out)
{
    char *end = NULL;
    long value;

    if (!text || !out)
    {
        return false;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > INT_MAX)
    {
        return false;
    }
    *out = (int)value;
    return true;
}

static CarrierState read_carrier(const char *iface)
{
    char path[256];
    char value[16] = {0};
    FILE *handle;

    if (!iface || iface[0] == '\0')
    {
        return CARRIER_UNKNOWN;
    }
    if (snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", iface) >=
        (int)sizeof(path))
    {
        return CARRIER_UNKNOWN;
    }
    handle = fopen(path, "r");
    if (!handle)
    {
        return CARRIER_UNKNOWN;
    }
    if (!fgets(value, sizeof(value), handle))
    {
        fclose(handle);
        return CARRIER_UNKNOWN;
    }
    fclose(handle);
    if (value[0] == '1')
    {
        return CARRIER_UP;
    }
    if (value[0] == '0')
    {
        return CARRIER_DOWN;
    }
    return CARRIER_UNKNOWN;
}

static bool parse_default_route_line(char *line, DefaultRoute *route)
{
    char *saveptr = NULL;
    char *token;

    if (!line || !route)
    {
        return false;
    }
    memset(route, 0, sizeof(*route));
    route->carrier = CARRIER_UNKNOWN;
    token = strtok_r(line, " \t", &saveptr);
    if (!token || strcmp(token, "default") != 0)
    {
        return false;
    }
    while ((token = strtok_r(NULL, " \t", &saveptr)) != NULL)
    {
        if (strcmp(token, "via") == 0)
        {
            token = strtok_r(NULL, " \t", &saveptr);
            if (!token)
            {
                return false;
            }
            snprintf(route->gateway, sizeof(route->gateway), "%s", token);
        }
        else if (strcmp(token, "dev") == 0)
        {
            token = strtok_r(NULL, " \t", &saveptr);
            if (!token || strlen(token) >= sizeof(route->iface))
            {
                return false;
            }
            snprintf(route->iface, sizeof(route->iface), "%s", token);
        }
        else if (strcmp(token, "metric") == 0)
        {
            token = strtok_r(NULL, " \t", &saveptr);
            if (!token || !parse_nonnegative_int(token, &route->metric))
            {
                return false;
            }
        }
        else if (strcmp(token, "linkdown") == 0)
        {
            route->linkdown = true;
        }
    }
    if (route->iface[0] == '\0')
    {
        return false;
    }
    route->carrier = read_carrier(route->iface);
    if (route->carrier == CARRIER_DOWN)
    {
        route->linkdown = true;
    }
    return true;
}

static int collect_default_routes(DefaultRoute *routes, int capacity)
{
    char output[ROUTE_OUTPUT_SIZE];
    const char *argv[] = {
        "ip", "-4", "route", "show", "default", NULL};
    int count = 0;
    char *saveptr = NULL;
    char *line;

    if (!routes || capacity <= 0 ||
        capture_cmd(argv, output, sizeof(output)) != 0)
    {
        return -1;
    }
    line = strtok_r(output, "\n", &saveptr);
    while (line)
    {
        if (count >= capacity)
        {
            return -1;
        }
        if (!parse_default_route_line(line, &routes[count]))
        {
            return -1;
        }
        ++count;
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

static bool route_is_usable(const DefaultRoute *route)
{
    return route && !route->linkdown && route->carrier != CARRIER_DOWN;
}

static const char *carrier_text(const DefaultRoute *route)
{
    if (route->linkdown || route->carrier == CARRIER_DOWN)
    {
        return "无链路";
    }
    if (route->carrier == CARRIER_UP)
    {
        return "有链路";
    }
    return "链路未知";
}

static void print_route(const char *prefix, const DefaultRoute *route)
{
    printf("%svia %s dev %s metric %d（%s）\n",
           prefix,
           route->gateway[0] ? route->gateway : "无显式网关",
           route->iface,
           route->metric,
           carrier_text(route));
}

static int remove_runtime_route(const DefaultRoute *route)
{
    char metric[32];

    snprintf(metric, sizeof(metric), "%d", route->metric);
    if (route->gateway[0] && route->metric > 0)
    {
        const char *argv[] = {
            "ip", "-4", "route", "del", "default",
            "via", route->gateway, "dev", route->iface,
            "metric", metric, NULL};
        return run_cmd(argv);
    }
    if (route->gateway[0])
    {
        const char *argv[] = {
            "ip", "-4", "route", "del", "default",
            "via", route->gateway, "dev", route->iface, NULL};
        return run_cmd(argv);
    }
    if (route->metric > 0)
    {
        const char *argv[] = {
            "ip", "-4", "route", "del", "default",
            "dev", route->iface, "metric", metric, NULL};
        return run_cmd(argv);
    }
    {
        const char *argv[] = {
            "ip", "-4", "route", "del", "default",
            "dev", route->iface, NULL};
        return run_cmd(argv);
    }
}

static void show_selected_route(void)
{
    char output[4096];
    const char *argv[] = {
        "ip", "-4", "route", "get", "1.1.1.1", NULL};

    if (capture_cmd(argv, output, sizeof(output)) == 0 && output[0])
    {
        printf("当前内核路由选择：%s\n", output);
    }
}

static bool reapply_connection(const char *iface)
{
    const char *argv[] = {
        "nmcli", "device", "reapply", iface, NULL};

    return run_cmd(argv) == 0;
}

static void repair_route(const DefaultRoute *broken,
                         const DefaultRoute *fallback)
{
    char uuid[UUID_SIZE] = {0};
    bool has_profile = get_active_connection_uuid(
        broken->iface, uuid, sizeof(uuid));
    int choice;

    printf("\n修复方式：\n");
    printf("  1. 临时移除无链路默认路由（不改保存的连接）\n");
    if (has_profile)
    {
        printf("  2. 永久设为仅局域网，不再提供默认路由\n");
        printf("  3. 永久降低优先级，让 %s 优先\n", fallback->iface);
    }
    printf("  0. 暂不修复\n");
    choice = read_int(has_profile ? "请选择 [0-3]: " : "请选择 [0-1]: ",
                      0, has_profile ? 3 : 1);
    if (choice == 0)
    {
        printf("已保持当前路由。\n");
        return;
    }
    if (choice == 1)
    {
        if (!read_yes_no("确定临时移除这条无链路默认路由吗？[y/N]: ",
                         false))
        {
            printf("已取消。\n");
            return;
        }
        if (remove_runtime_route(broken) != 0)
        {
            printf("[失败] 无法移除运行时默认路由。\n");
            return;
        }
        printf("[完成] 已移除无链路默认路由；连接重新启用或重启后可能再出现。\n");
        show_selected_route();
        return;
    }
    if (choice == 2)
    {
        const char *modify[] = {
            "nmcli", "connection", "modify", "uuid", uuid,
            "ipv4.never-default", "yes", NULL};

        printf("适用场景：%s 只连摄像头或局域网，不用于访问其他网段。\n",
               broken->iface);
        if (!read_exact_word("确认永久禁用默认路由请输入 NO DEFAULT: ",
                             "NO DEFAULT"))
        {
            printf("已取消。\n");
            return;
        }
        if (run_cmd(modify) != 0)
        {
            printf("[失败] 无法修改 NetworkManager 连接。\n");
            return;
        }
        (void)remove_runtime_route(broken);
        (void)reapply_connection(broken->iface);
        printf("[完成] %s 已永久设为仅局域网，不再抢占 IPv4 默认路由。\n",
               broken->iface);
        show_selected_route();
        return;
    }
    {
        char metric[32];
        int new_metric = fallback->metric <= INT_MAX - 100
                             ? fallback->metric + 100
                             : INT_MAX;
        const char *modify[] = {
            "nmcli", "connection", "modify", "uuid", uuid,
            "ipv4.never-default", "no",
            "ipv4.route-metric", metric, NULL};

        snprintf(metric, sizeof(metric), "%d", new_metric);
        printf("将把 %s 的 metric 改为 %d；%s 的 metric 是 %d，"
               "因此将优先使用 %s。\n",
               broken->iface, new_metric, fallback->iface,
               fallback->metric, fallback->iface);
        if (!read_exact_word("确认永久降低优先级请输入 LOWER PRIORITY: ",
                             "LOWER PRIORITY"))
        {
            printf("已取消。\n");
            return;
        }
        if (run_cmd(modify) != 0)
        {
            printf("[失败] 无法修改 NetworkManager 连接。\n");
            return;
        }
        (void)remove_runtime_route(broken);
        if (!reapply_connection(broken->iface))
        {
            printf("[提醒] 永久 metric 已保存，将在连接下次启用时生效。\n");
        }
        printf("[完成] 无链路网卡已不再优先于当前可用出口。\n");
        show_selected_route();
    }
}

void manage_default_route_health(bool quiet_when_healthy)
{
    DefaultRoute routes[MAX_DEFAULT_ROUTES] = {0};
    int count = collect_default_routes(routes, MAX_DEFAULT_ROUTES);
    int best_usable = -1;
    int broken = -1;

    if (count < 0)
    {
        if (!quiet_when_healthy)
        {
            printf("[失败] 无法读取完整 IPv4 默认路由清单。\n");
        }
        return;
    }
    for (int index = 0; index < count; ++index)
    {
        if (route_is_usable(&routes[index]) &&
            (best_usable < 0 ||
             routes[index].metric < routes[best_usable].metric))
        {
            best_usable = index;
        }
    }
    if (best_usable >= 0)
    {
        for (int index = 0; index < count; ++index)
        {
            if (!route_is_usable(&routes[index]) &&
                routes[index].metric <= routes[best_usable].metric &&
                (broken < 0 || routes[index].metric < routes[broken].metric))
            {
                broken = index;
            }
        }
    }

    if (broken < 0)
    {
        if (!quiet_when_healthy)
        {
            printf("\n========== IPv4 默认路由健康检查 ==========\n");
            if (count == 0)
            {
                printf("[提醒] 当前没有 IPv4 默认路由。\n");
            }
            else
            {
                for (int index = 0; index < count; ++index)
                {
                    print_route("  ", &routes[index]);
                }
                if (best_usable >= 0)
                {
                    printf("[正常] 没有无链路路由抢占有链路出口。\n");
                }
                else
                {
                    printf("[警告] 所有 IPv4 默认路由都没有可确认的物理链路。\n");
                }
            }
        }
        return;
    }

    printf("\n============================================================\n");
    printf("[严重路由警告] 内核会优先选择一条无链路的默认路由。\n");
    print_route("  问题路由：", &routes[broken]);
    print_route("  有链路备用：", &routes[best_usable]);
    printf("因为 metric %d 小于或等于 %d，访问其他网段时仍可能走 %s，\n",
           routes[broken].metric, routes[best_usable].metric,
           routes[broken].iface);
    printf("并出现 Destination Host Unreachable。\n");
    printf("============================================================\n");
    repair_route(&routes[broken], &routes[best_usable]);
}

void manage_network_health(void)
{
    printf("\n============================================================\n");
    printf("                    统一网络健康检查\n");
    printf("============================================================\n");
    printf("检查范围：IPv4 默认路由抢占、活动网卡 IPv4 网段冲突。\n");

    printf("\n[1/2] 检查 IPv4 默认路由\n");
    manage_default_route_health(false);

    printf("\n[2/2] 检查活动网卡 IPv4 网段冲突\n");
    manage_conflicting_connections();

    printf("\n[完成] 本轮网络健康检查结束。\n");
}
