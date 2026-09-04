#define _POSIX_C_SOURCE 200809L

#include "saved_connections.h"

#include "cli_io.h"
#include "command_runner.h"
#include "interface_inspector.h"
#include "ipv4_utils.h"
#include "netconfig_types.h"
#include "nmcli_parser.h"
#include "network_safety.h"
#include "network_state.h"

#include <limits.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SAVED_CONNECTIONS 128
#define CONNECTION_LIST_SIZE 65536
#define DETAIL_SIZE 1024
#define SAVED_NAME_SIZE 512
#define MAX_CONFLICT_INTERFACES 128
#define CONFLICT_SETTLE_ATTEMPTS 20
#define CONFLICT_SETTLE_INTERVAL_NS 250000000L

typedef enum
{
    SAVED_CONNECTION_WIRED = 1,
    SAVED_CONNECTION_WIFI = 2
} SavedConnectionKind;

typedef struct
{
    char name[SAVED_NAME_SIZE];
    char uuid[UUID_SIZE];
    SavedConnectionKind kind;
    char device[IF_NAMESIZE];
    bool active;
    bool autoconnect;
    char method[32];
    char addresses[DETAIL_SIZE];
    char gateway[64];
    char dns[DETAIL_SIZE];
    char effective_dns[DETAIL_SIZE];
    char ssid[BUF_SIZE];
    char security[96];
} SavedConnection;

static SavedConnectionKind connection_kind(const char *type)
{
    if (type &&
        (strcmp(type, "802-3-ethernet") == 0 ||
         strcmp(type, "ethernet") == 0))
    {
        return SAVED_CONNECTION_WIRED;
    }
    if (type &&
        (strcmp(type, "802-11-wireless") == 0 ||
         strcmp(type, "wifi") == 0))
    {
        return SAVED_CONNECTION_WIFI;
    }
    return 0;
}

static bool text_is_yes(const char *value)
{
    return value &&
           (strcmp(value, "yes") == 0 || strcmp(value, "true") == 0 ||
            strcmp(value, "1") == 0);
}

static void replace_newlines(char *value)
{
    char *source = value;
    char *destination = value;
    bool last_was_separator = false;

    while (source && *source)
    {
        if (*source == '\r' || *source == '\n')
        {
            if (destination != value && !last_was_separator)
            {
                *destination++ = ',';
                last_was_separator = true;
            }
            ++source;
            continue;
        }
        *destination++ = *source++;
        last_was_separator = false;
    }
    if (destination != value && destination[-1] == ',')
    {
        --destination;
    }
    *destination = '\0';
}

static void read_connection_value(const char *uuid, const char *field,
                                  char *out, size_t out_size)
{
    const char *argv[] = {
        "nmcli", "--escape", "no", "-g", field,
        "connection", "show", "uuid", uuid, NULL};

    if (capture_cmd(argv, out, out_size) != 0)
    {
        out[0] = '\0';
        return;
    }
    replace_newlines(out);
}

static void read_device_value(const char *device, const char *field,
                              char *out, size_t out_size)
{
    const char *argv[] = {
        "nmcli", "--escape", "no", "-g", field,
        "device", "show", device, NULL};

    if (!device || device[0] == '\0' ||
        capture_cmd(argv, out, out_size) != 0)
    {
        out[0] = '\0';
        return;
    }
    replace_newlines(out);
}

static void load_connection_detail(SavedConnection *connection)
{
    if (!connection)
    {
        return;
    }

    read_connection_value(connection->uuid, "ipv4.method",
                          connection->method, sizeof(connection->method));
    read_connection_value(connection->uuid, "ipv4.addresses",
                          connection->addresses, sizeof(connection->addresses));
    read_connection_value(connection->uuid, "ipv4.gateway",
                          connection->gateway, sizeof(connection->gateway));
    read_connection_value(connection->uuid, "ipv4.dns",
                          connection->dns, sizeof(connection->dns));
    if (connection->active)
    {
        read_device_value(connection->device, "IP4.DNS",
                          connection->effective_dns,
                          sizeof(connection->effective_dns));
    }
    if (connection->kind == SAVED_CONNECTION_WIFI)
    {
        read_connection_value(connection->uuid, "802-11-wireless.ssid",
                              connection->ssid, sizeof(connection->ssid));
        read_connection_value(connection->uuid,
                              "802-11-wireless-security.key-mgmt",
                              connection->security,
                              sizeof(connection->security));
    }
}

static int collect_saved_connections(SavedConnection *connections, int capacity)
{
    char output[CONNECTION_LIST_SIZE];
    int count = 0;
    const char *argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "NAME,UUID,TYPE,DEVICE,AUTOCONNECT",
        "connection", "show", NULL};

    if (!connections || capacity <= 0 ||
        capture_cmd(argv, output, sizeof(output)) != 0)
    {
        return -1;
    }

    {
        char *saveptr = NULL;
        char *line = strtok_r(output, "\n", &saveptr);

        while (line && count < capacity)
        {
            char *fields[5] = {0};
            int field_count = split_nmcli_escaped_fields(line, fields, 5);
            SavedConnectionKind kind =
                field_count >= 3 ? connection_kind(fields[2]) : 0;

            if (field_count == 5 && kind != 0 &&
                strlen(fields[1]) == UUID_SIZE - 1)
            {
                SavedConnection *item = &connections[count];

                memset(item, 0, sizeof(*item));
                snprintf(item->name, sizeof(item->name), "%s", fields[0]);
                snprintf(item->uuid, sizeof(item->uuid), "%s", fields[1]);
                item->kind = kind;
                item->active = fields[3][0] != '\0' &&
                               strcmp(fields[3], "--") != 0;
                if (item->active)
                {
                    snprintf(item->device, sizeof(item->device), "%s", fields[3]);
                }
                item->autoconnect = text_is_yes(fields[4]);
                load_connection_detail(item);
                ++count;
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    return count;
}

static const char *kind_text(const SavedConnection *connection)
{
    return connection->kind == SAVED_CONNECTION_WIFI ? "无线网络" : "有线网络";
}

static const char *security_text(const char *security)
{
    if (!security || security[0] == '\0' || strcmp(security, "--") == 0)
    {
        return "无密码";
    }
    if (strstr(security, "sae") != NULL)
    {
        return "WPA3";
    }
    if (strstr(security, "wpa-psk") != NULL)
    {
        return "WPA/WPA2";
    }
    return "需要认证";
}

static void print_connection_card(const SavedConnection *connection, int number)
{
    printf("\n  %d. %s%s\n", number, connection->name,
           connection->active ? "  [正在使用]" : "");
    printf("     类型：%s", kind_text(connection));
    if (connection->kind == SAVED_CONNECTION_WIFI && connection->ssid[0])
    {
        printf(" · %s · %s", connection->ssid,
               security_text(connection->security));
    }
    printf("\n");
    if (connection->active)
    {
        printf("     网卡：%s\n", connection->device);
    }
    if (strcmp(connection->method, "manual") == 0)
    {
        printf("     地址：%s\n",
               connection->addresses[0] ? connection->addresses : "固定地址");
    }
    else
    {
        printf("     地址：自动获取\n");
    }
    printf("     开机自动使用：%s\n",
           connection->autoconnect ? "是" : "否");
    if (connection->dns[0])
    {
        printf("     DNS：%s（手工配置）\n", connection->dns);
    }
    else if (connection->effective_dns[0])
    {
        printf("     DNS：%s（DHCP/当前有效，未手工配置）\n",
               connection->effective_dns);
    }
    else if (connection->active)
    {
        printf("     DNS：[未配置且未获得]\n");
    }
}

static void print_selected_detail(const SavedConnection *connection)
{
    printf("\n====================================================\n");
    printf("连接名称：%s\n", connection->name);
    printf("连接类型：%s\n", kind_text(connection));
    printf("当前状态：%s\n", connection->active ? "正在使用" : "未使用");
    if (connection->active)
    {
        printf("使用网卡：%s\n", connection->device);
    }
    if (connection->kind == SAVED_CONNECTION_WIFI)
    {
        printf("无线名称：%s\n", connection->ssid[0] ? connection->ssid : "未读取到");
        printf("密码方式：%s\n", security_text(connection->security));
    }
    printf("地址设置：%s\n",
           strcmp(connection->method, "manual") == 0 ? "固定地址" : "自动获取");
    if (connection->addresses[0])
    {
        printf("IP 地址：%s\n", connection->addresses);
    }
    if (connection->gateway[0])
    {
        printf("网关：%s\n", connection->gateway);
    }
    printf("手工 DNS：%s\n",
           connection->dns[0] ? connection->dns : "未配置");
    if (connection->active)
    {
        printf("当前有效 DNS：%s\n",
               connection->effective_dns[0]
                   ? connection->effective_dns
                   : "未获得（域名解析可能失败）");
    }
    printf("开机自动使用：%s\n", connection->autoconnect ? "是" : "否");
    printf("====================================================\n");
}

static void ipv4_config_from_connection(const SavedConnection *connection,
                                        IPv4Config *cfg)
{
    char first_address[64];
    char *slash;
    char *end = NULL;
    long prefix;
    size_t length;

    memset(cfg, 0, sizeof(*cfg));
    if (strcmp(connection->method, "manual") != 0 ||
        connection->addresses[0] == '\0')
    {
        return;
    }

    length = strcspn(connection->addresses, ", ");
    if (length == 0 || length >= sizeof(first_address))
    {
        return;
    }
    memcpy(first_address, connection->addresses, length);
    first_address[length] = '\0';
    slash = strchr(first_address, '/');
    if (!slash)
    {
        return;
    }
    *slash++ = '\0';
    prefix = strtol(slash, &end, 10);
    if (!end || *end != '\0' || prefix < 0 || prefix > 32 ||
        !valid_ipv4(first_address) ||
        strlen(first_address) >= sizeof(cfg->ip))
    {
        return;
    }

    cfg->is_static = true;
    cfg->prefix = (int)prefix;
    memcpy(cfg->ip, first_address, strlen(first_address) + 1);
    snprintf(cfg->gateway, sizeof(cfg->gateway), "%s", connection->gateway);
    snprintf(cfg->dns, sizeof(cfg->dns), "%s", connection->dns);
}

static void use_saved_connection(const SavedConnection *connection)
{
    char iface[IF_NAMESIZE];
    ConnectionProfile profile = {0};
    IPv4Config cfg;
    NetworkActivationResult result;

    if (connection->active)
    {
        printf("\n这项连接已经在使用，不需要再次启用。\n");
        return;
    }

    printf("\n请选择用哪张%s网卡连接。\n",
           connection->kind == SAVED_CONNECTION_WIFI ? "无线" : "有线");
    if (connection->kind == SAVED_CONNECTION_WIFI)
    {
        if (!choose_wifi_interface(iface, sizeof(iface)))
        {
            return;
        }
    }
    else if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }

    if (strlen(connection->name) < sizeof(profile.name))
    {
        memcpy(profile.name, connection->name, strlen(connection->name) + 1);
    }
    else
    {
        snprintf(profile.name, sizeof(profile.name), "已保存的连接");
    }
    snprintf(profile.uuid, sizeof(profile.uuid), "%s", connection->uuid);
    ipv4_config_from_connection(connection, &cfg);
    result = safe_activate_with_reconnect(
        iface, &profile, &cfg, "", NETWORK_PROFILE_EXISTING);
    if (result == NETWORK_ACTIVATION_CONFIRMED)
    {
        printf("\n[完成] 已开始使用“%s”。\n", connection->name);
    }
    else if (result == NETWORK_ACTIVATION_PENDING)
    {
        printf("\n请通过新的 IP 地址重新登录，然后再次运行本工具确认。\n");
        exit(EXIT_SUCCESS);
    }
}

static bool remove_saved_connection(const SavedConnection *connection)
{
    bool queried_active;
    bool active_now;
    bool ssh_path;

    if (!connection)
    {
        return false;
    }

    /* 列表展示后连接状态可能发生变化，删除前必须再查一次。 */
    queried_active = connection_is_active(connection->uuid);
    /* 查询失败时 connection_is_active 会返回 false，保留列表中的
     * active 结果，避免因一次瞬时失败降级为普通确认。 */
    active_now = connection->active || queried_active;
    ssh_path = active_now && connection->device[0] != '\0' &&
               interface_is_ssh_path(connection->device);

    if (active_now)
    {
        printf("\n[高风险] “%s”正在使用。\n", connection->name);
        printf("删除会让 NetworkManager 立即移除该 profile，"
               "对应网卡可能立即断网。\n");
        printf("本操作没有超时回滚，恢复时需要重新创建连接。\n");
        if (ssh_path)
        {
            printf("[当前 SSH] 该连接正在承载本次远程会话，"
                   "确认后 SSH 预计会立即断开。\n");
            printf("请先确认已有串口、HDMI 或其他可用网络。\n");
        }
        if (!read_exact_word(
                "确认删除正在使用的连接请输入 DELETE ACTIVE: ",
                "DELETE ACTIVE"))
        {
            printf("已取消。\n");
            return false;
        }

        /* SSH 断开可能向会话进程发送信号，确保 nmcli 能执行完。 */
        if (signal(SIGHUP, SIG_IGN) == SIG_ERR ||
            signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        {
            printf("[拒绝] 无法启用断线保护，未删除正在使用的连接。\n");
            return false;
        }
    }

    printf("\n即将删除“%s”。删除后需要重新填写才能再次使用。\n",
           connection->name);
    if (!active_now && !read_yes_no("确定删除吗？[y/N]: ", false))
    {
        printf("已取消。\n");
        return false;
    }
    if (delete_connection_by_uuid(connection->uuid) == 0)
    {
        printf("[完成] 已删除“%s”。\n", connection->name);
        return true;
    }

    printf("[失败] 没有删除成功，请稍后重试。\n");
    return false;
}

static bool interface_is_conflicting(
    char interfaces[][IF_NAMESIZE],
    int count,
    const char *iface)
{
    if (!interfaces || !iface)
    {
        return false;
    }
    for (int index = 0; index < count; ++index)
    {
        if (strcmp(interfaces[index], iface) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool wait_for_deleted_connection_to_settle(
    const SavedConnection *connection)
{
    const struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = CONFLICT_SETTLE_INTERVAL_NS};

    if (!connection || connection->device[0] == '\0')
    {
        return false;
    }

    for (int attempt = 0; attempt < CONFLICT_SETTLE_ATTEMPTS; ++attempt)
    {
        char interfaces[MAX_CONFLICT_INTERFACES][IF_NAMESIZE] = {{0}};
        int interface_count = collect_current_overlap_interfaces(
            interfaces, MAX_CONFLICT_INTERFACES);

        if (interface_count < 0)
        {
            return false;
        }

        if (!interface_is_conflicting(
                interfaces, interface_count, connection->device))
        {
            /*
             * nmcli 已经删除 profile，而且旧 IPv4 地址也已从内核消失。
             * 只有这时才重新输出冲突检查，避免展示删除前的陈旧地址。
             *
             * 如果同一网卡自动启用了另一个仍然冲突的 profile，这里会
             * 等到超时，再由下一轮把真实存在的新冲突显示出来。
             */
            return true;
        }

        if (attempt + 1 < CONFLICT_SETTLE_ATTEMPTS)
        {
            (void)nanosleep(&interval, NULL);
        }
    }

    return false;
}

void manage_conflicting_connections(void)
{
    for (;;)
    {
        char interfaces[MAX_CONFLICT_INTERFACES][IF_NAMESIZE] = {{0}};
        SavedConnection connections[MAX_SAVED_CONNECTIONS] = {0};
        int candidates[MAX_SAVED_CONNECTIONS] = {0};
        int warnings;
        int interface_count;
        int connection_count;
        int candidate_count = 0;
        bool has_unmanaged = false;

        printf("\n========== IPv4 网段冲突健康检查 ==========\n");
        warnings = show_current_overlap_warnings();
        if (warnings <= 0)
        {
            return;
        }

        interface_count = collect_current_overlap_interfaces(
            interfaces, MAX_CONFLICT_INTERFACES);
        if (interface_count <= 0)
        {
            printf("[失败] 无法确定冲突涉及的网卡，未提供删除操作。\n");
            return;
        }

        connection_count = collect_saved_connections(
            connections, MAX_SAVED_CONNECTIONS);
        if (connection_count < 0)
        {
            printf("[失败] 无法读取冲突网卡正在使用的连接。\n");
            return;
        }

        for (int index = 0; index < connection_count; ++index)
        {
            if (connections[index].active &&
                interface_is_conflicting(interfaces, interface_count,
                                         connections[index].device))
            {
                candidates[candidate_count++] = index;
            }
        }

        printf("\n冲突涉及的网卡：");
        for (int index = 0; index < interface_count; ++index)
        {
            printf("%s%s", index == 0 ? "" : "、", interfaces[index]);
        }
        printf("\n");

        if (candidate_count > 0)
        {
            printf("\n可删除的冲突连接：\n");
            for (int index = 0; index < candidate_count; ++index)
            {
                print_connection_card(
                    &connections[candidates[index]], index + 1);
            }
        }

        for (int iface_index = 0;
             iface_index < interface_count; ++iface_index)
        {
            bool represented = false;

            for (int candidate = 0;
                 candidate < candidate_count; ++candidate)
            {
                if (strcmp(
                        connections[candidates[candidate]].device,
                        interfaces[iface_index]) == 0)
                {
                    represented = true;
                    break;
                }
            }
            if (!represented)
            {
                if (!has_unmanaged)
                {
                    printf("\n不能由本工具直接删除的冲突网卡：\n");
                    has_unmanaged = true;
                }
                printf("  - %s：没有找到活动的有线/Wi-Fi "
                       "NetworkManager 配置，可能是虚拟网卡或手工地址\n",
                       interfaces[iface_index]);
            }
        }

        if (candidate_count == 0)
        {
            printf("\n[提醒] 没有找到可安全映射并删除的冲突连接。\n");
            return;
        }

        printf("\n删除连接是永久操作，不只是临时断开。\n");
        printf("请选择确认不再需要的一侧；正在使用或承载 SSH 的连接"
               "会要求高风险确认。\n");
        printf("  0. 暂不删除\n");

        {
            int selected = read_int("请选择要删除的冲突连接: ",
                                    0, candidate_count);
            SavedConnection *selected_connection;

            if (selected == 0)
            {
                printf("已保留当前连接。\n");
                return;
            }
            selected_connection = &connections[candidates[selected - 1]];
            if (!remove_saved_connection(selected_connection))
            {
                return;
            }

            printf("正在等待 NetworkManager 清理旧 IP 和路由...\n");
            if (!wait_for_deleted_connection_to_settle(selected_connection))
            {
                printf("[提醒] 等待约 5 秒后，%s 仍参与网段冲突。\n",
                       selected_connection->device);
                printf("可能仍有手工 IP 或其他网络状态，下面按实时状态重新检查。\n");
            }
        }

        printf("\n正在重新检查剩余网段冲突...\n");
    }
}

static void configure_saved_connection_dns(const SavedConnection *connection)
{
    char input[BUF_SIZE];
    char dns[BUF_SIZE];

    if (!connection)
    {
        return;
    }
    printf("\n========== 单独补充 DNS ==========\n");
    printf("连接：%s\n", connection->name);
    printf("已手工配置：%s\n",
           connection->dns[0] ? connection->dns : "无");
    if (connection->active)
    {
        printf("当前有效 DNS：%s\n",
               connection->effective_dns[0] ? connection->effective_dns : "无");
    }
    printf("新 DNS 会作为手工地址加入该连接，不会删除 DHCP 提供的 DNS。\n");
    printf("如果当前没有任何 DNS，可填写现场内网 DNS 或可访问的公共 DNS。\n");

    for (;;)
    {
        read_line("请输入 DNS（IPv4，最多 4 个，逗号分隔；回车取消）: ",
                  input, sizeof(input));
        trim_space(input);
        if (input[0] == '\0')
        {
            printf("已取消。\n");
            return;
        }
        if (normalize_ipv4_list(input, dns, sizeof(dns), 4))
        {
            break;
        }
        printf("DNS 格式不正确；例如 10.0.0.53,223.5.5.5。\n");
    }

    printf("程序解析为：%s\n", dns);
    if (!read_yes_no("确定保存到这项连接吗？[y/N]: ", false))
    {
        printf("已取消。\n");
        return;
    }
    {
        const char *modify[] = {
            "nmcli", "connection", "modify", "uuid", connection->uuid,
            "ipv4.dns", dns,
            "ipv4.ignore-auto-dns", "no", NULL};

        if (run_cmd(modify) != 0)
        {
            printf("[失败] DNS 没有保存成功。\n");
            return;
        }
    }
    if (!connection->active)
    {
        printf("[完成] DNS 已保存，这项连接下次启用时生效。\n");
        return;
    }
    {
        char effective[DETAIL_SIZE];
        const char *reapply[] = {
            "nmcli", "device", "reapply", connection->device, NULL};

        if (run_cmd(reapply) != 0)
        {
            printf("[已保存] 当前网卡无法无断线重新应用；"
                   "重新连接或重启后生效。\n");
            return;
        }
        read_device_value(connection->device, "IP4.DNS",
                          effective, sizeof(effective));
        if (effective[0])
        {
            printf("[完成] DNS 已无断线应用；当前有效 DNS：%s\n",
                   effective);
        }
        else
        {
            printf("[已保存] 系统尚未报告有效 DNS，"
                   "请重新连接后再检查。\n");
        }
    }
}

void manage_saved_connections(void)
{
    for (;;)
    {
        SavedConnection connections[MAX_SAVED_CONNECTIONS] = {0};
        int count;
        int selected;

        printf("\n====================================================\n");
        printf("                    已保存的连接\n");
        printf("====================================================\n");
        count = collect_saved_connections(connections,
                                          MAX_SAVED_CONNECTIONS);
        if (count < 0)
        {
            printf("无法读取已保存的连接。\n");
            return;
        }
        if (count == 0)
        {
            printf("目前没有已保存的有线或无线连接。\n");
            return;
        }

        for (int index = 0; index < count; ++index)
        {
            print_connection_card(&connections[index], index + 1);
        }
        printf("\n  0. 返回主菜单\n");
        printf("====================================================\n");
        selected = read_int("请选择一项连接: ", 0, count);
        if (selected == 0)
        {
            return;
        }

        for (;;)
        {
            int action;
            SavedConnection *connection = &connections[selected - 1];

            print_selected_detail(connection);
            if (connection->active)
            {
                printf("1. 单独补充/修改 DNS（尽量无断线生效）\n");
                printf("2. 删除这项正在使用的连接（可能立即断网）\n");
                printf("0. 返回连接列表\n");
                action = read_int("请选择操作 [0-2]: ", 0, 2);
                if (action == 1)
                {
                    configure_saved_connection_dns(connection);
                }
                else if (action == 2)
                {
                    remove_saved_connection(connection);
                }
                break;
            }
            printf("1. 使用这项连接\n");
            printf("2. 单独补充/修改 DNS\n");
            printf("3. 删除这项连接\n");
            printf("0. 返回连接列表\n");
            action = read_int("请选择操作 [0-3]: ", 0, 3);
            if (action == 0)
            {
                break;
            }
            if (action == 1)
            {
                use_saved_connection(connection);
                break;
            }
            if (action == 2)
            {
                configure_saved_connection_dns(connection);
                break;
            }
            remove_saved_connection(connection);
            break;
        }
    }
}
