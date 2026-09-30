#define _POSIX_C_SOURCE 200809L

#include "inspection/network_health.h"

#include "common/cli_io.h"
#include "common/command_runner.h"
#include "inspection/interface_inspector.h"
#include "common/netconfig_types.h"
#include "inspection/network_state.h"
#include "operations/network_safety.h"
#include "operations/saved_connections.h"

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
    printf("%s%s", prefix, route->iface);
    if (route->gateway[0])
    {
        printf("，路由器 %s", route->gateway);
    }
    printf("，优先级 %d（%s）\n", route->metric, carrier_text(route));
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
        char *saveptr = NULL;
        char *token = strtok_r(output, " \t\r\n", &saveptr);
        char device[IF_NAMESIZE] = {0};
        char gateway[64] = {0};

        while (token)
        {
            if (strcmp(token, "dev") == 0)
            {
                token = strtok_r(NULL, " \t\r\n", &saveptr);
                if (token)
                {
                    snprintf(device, sizeof(device), "%s", token);
                }
            }
            else if (strcmp(token, "via") == 0)
            {
                token = strtok_r(NULL, " \t\r\n", &saveptr);
                if (token)
                {
                    snprintf(gateway, sizeof(gateway), "%s", token);
                }
            }
            token = strtok_r(NULL, " \t\r\n", &saveptr);
        }
        if (device[0])
        {
            printf("当前默认联网：%s", device);
            if (gateway[0])
            {
                printf("，路由器 %s", gateway);
            }
            printf("\n");
        }
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
    char broken_uuid[UUID_SIZE] = {0};
    char fallback_uuid[UUID_SIZE] = {0};
    bool broken_has_profile = get_active_connection_uuid(
        broken->iface, broken_uuid, sizeof(broken_uuid));
    bool fallback_has_profile = get_active_connection_uuid(
        fallback->iface, fallback_uuid, sizeof(fallback_uuid));
    int choice;

repair_choice:
    terminal_ui_set_step("选择默认路由修复方式",
                         "选择临时处理或永久修改连接策略");
    printf("\n修复方式：\n");
    printf("  1. 临时移除失效的默认联网设置\n");
    if (broken_has_profile)
    {
        printf("  2. 永久设为仅局域网\n");
    }
    else
    {
        printf("  2. [不可用] 问题网卡没有活动的保存连接\n");
    }
    if (broken_has_profile || fallback_has_profile)
    {
        printf("  3. 手动选择连接并设置路由优先级\n");
    }
    else
    {
        printf("  3. [不可用] 两个网卡都没有活动的保存连接\n");
    }
    printf("  0. 暂不修复\n");
    choice = read_int("请选择 [0-3]: ", 0, 3);
    if (choice == TERMINAL_UI_INPUT_CANCELLED)
    {
        return;
    }
    if (choice == 0)
    {
        printf("已保持当前路由。\n");
        return;
    }
    if (choice == 1)
    {
        terminal_ui_set_step("确认临时移除默认路由",
                             "只修改当前运行状态，重启后可能恢复");
        terminal_ui_prepare_step(true);
        if (!read_yes_no("确定临时移除失效设置吗？[y/N]: ",
                         false))
        {
            if (terminal_ui_back_requested())
            {
                goto repair_choice;
            }
            printf("已取消。\n");
            return;
        }
        if (remove_runtime_route(broken) != 0)
        {
            printf("[失败] 无法移除失效设置。\n");
            return;
        }
        printf("[完成] 已临时移除；重启后可能再次出现。\n");
        show_selected_route();
        return;
    }
    if (choice == 2)
    {
        if (!broken_has_profile)
        {
            printf("[提醒] 问题网卡没有可修改的活动保存连接。\n");
            goto repair_choice;
        }
        const char *modify[] = {
            "nmcli", "connection", "modify", "uuid", broken_uuid,
            "ipv4.never-default", "yes", NULL};

        printf("适用场景：%s 只连摄像头或局域网，不用于访问其他网段。\n",
               broken->iface);
        terminal_ui_set_step("确认设为仅局域网",
                             "此连接将不再提供默认联网");
        terminal_ui_prepare_step(true);
        if (!read_exact_word("确认设为仅局域网请输入 NO DEFAULT: ",
                             "NO DEFAULT"))
        {
            if (terminal_ui_back_requested())
            {
                goto repair_choice;
            }
            printf("已取消。\n");
            return;
        }
        if (run_cmd(modify) != 0)
        {
            printf("[失败] 无法修改保存连接。\n");
            return;
        }
        (void)remove_runtime_route(broken);
        (void)reapply_connection(broken->iface);
        printf("[完成] %s 已设为仅局域网，不再用于默认联网。\n",
               broken->iface);
        show_selected_route();
        return;
    }
    {
        const DefaultRoute *targets[2];
        const char *uuids[2];
        int target_count = 0;
        int selected;
        char profile_name[PROFILE_SIZE] = {0};
        char current_metric[32];

        if (!broken_has_profile && !fallback_has_profile)
        {
            printf("[提醒] 没有可修改的活动保存连接。\n");
            goto repair_choice;
        }
        if (broken_has_profile)
        {
            targets[target_count] = broken;
            uuids[target_count++] = broken_uuid;
        }
        if (fallback_has_profile)
        {
            targets[target_count] = fallback;
            uuids[target_count++] = fallback_uuid;
        }

priority_target_choice:
        terminal_ui_set_step("选择优先级修改对象",
                             "问题路由和可用路由均由用户明确选择");
        printf("\n请选择要修改优先级的连接：\n");
        for (int index = 0; index < target_count; ++index)
        {
            printf("  %d. %s（当前优先级 %d，%s）\n",
                   index + 1, targets[index]->iface,
                   targets[index]->metric, carrier_text(targets[index]));
        }
        printf("  0. 返回修复方式\n");
        selected = read_int("请选择要修改的连接: ", 0, target_count);
        if (selected == TERMINAL_UI_INPUT_CANCELLED || selected == 0)
        {
            goto repair_choice;
        }
        --selected;
        profile_name[0] = '\0';
        (void)get_connection_name(uuids[selected], profile_name,
                                  sizeof(profile_name));
        snprintf(current_metric, sizeof(current_metric), "%d",
                 targets[selected]->metric);
        if (!change_route_metric_interactively(
                uuids[selected], profile_name, targets[selected]->iface,
                current_metric))
        {
            if (terminal_ui_back_requested())
            {
                goto priority_target_choice;
            }
            return;
        }
        show_selected_route();
        return;
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
            printf("[失败] 无法读取默认联网设置。\n");
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
            printf("\n========== 默认联网检查 ==========\n");
            if (count == 0)
            {
                printf("[提醒] 当前没有默认联网方式。\n");
            }
            else
            {
                for (int index = 0; index < count; ++index)
                {
                    print_route("  ", &routes[index]);
                }
                if (best_usable >= 0)
                {
                    printf("[正常] 默认联网设置正常。\n");
                }
                else
                {
                    printf("[警告] 默认联网使用的网卡均未接通。\n");
                }
            }
        }
        return;
    }

    printf("\n============================================================\n");
    printf("[严重警告] 系统优先选择了未接通的网卡。\n");
    print_route("  问题路由：", &routes[broken]);
    print_route("  有链路备用：", &routes[best_usable]);
    printf("其优先级数值 %d 更小，联网仍可能走 %s 并报 Destination Host Unreachable。\n",
           routes[broken].metric, routes[broken].iface);
    printf("============================================================\n");
    repair_route(&routes[broken], &routes[best_usable]);
}

void manage_network_health(void)
{
    printf("\n============================================================\n");
    printf("                    统一网络健康检查\n");
    printf("============================================================\n");
    printf("检查范围：默认联网、网卡 IP 冲突。所有修改均由用户选择。\n");

    printf("\n[1/2] 检查默认联网\n");
    manage_default_route_health(false);

    printf("\n[2/2] 检查网卡 IP 冲突\n");
    manage_conflicting_connections();

    printf("\n[完成] 本轮网络健康检查结束。\n");
}
