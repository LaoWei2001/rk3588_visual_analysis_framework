#define _POSIX_C_SOURCE 200809L

#include "operations/saved_connections.h"

#include "common/cli_io.h"
#include "common/command_runner.h"
#include "inspection/interface_inspector.h"
#include "common/ipv4_utils.h"
#include "common/netconfig_types.h"
#include "common/nmcli_parser.h"
#include "operations/network_safety.h"
#include "inspection/network_state.h"

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
    char interface_name[IF_NAMESIZE];
    bool active;
    EthernetCarrierState carrier;
    bool autoconnect;
    char method[32];
    char addresses[DETAIL_SIZE];
    char gateway[64];
    char dns[DETAIL_SIZE];
    char route_metric[32];
    char effective_dns[DETAIL_SIZE];
    char ssid[BUF_SIZE];
    char security[96];
    char hidden[16];
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
    read_connection_value(connection->uuid, "ipv4.route-metric",
                          connection->route_metric,
                          sizeof(connection->route_metric));
    read_connection_value(connection->uuid, "connection.interface-name",
                          connection->interface_name,
                          sizeof(connection->interface_name));
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
        read_connection_value(connection->uuid, "802-11-wireless.hidden",
                              connection->hidden, sizeof(connection->hidden));
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
                item->carrier = ETHERNET_CARRIER_UNKNOWN;
                snprintf(item->name, sizeof(item->name), "%s", fields[0]);
                snprintf(item->uuid, sizeof(item->uuid), "%s", fields[1]);
                item->kind = kind;
                item->active = fields[3][0] != '\0' &&
                               strcmp(fields[3], "--") != 0;
                if (item->active)
                {
                    snprintf(item->device, sizeof(item->device), "%s", fields[3]);
                    if (item->kind == SAVED_CONNECTION_WIRED)
                    {
                        item->carrier =
                            get_ethernet_carrier_state(item->device);
                    }
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

static const char *connection_status_text(const SavedConnection *connection)
{
    if (!connection->active)
    {
        return "未连接";
    }
    if (connection->kind != SAVED_CONNECTION_WIRED)
    {
        return "已连接";
    }
    if (connection->carrier == ETHERNET_CARRIER_UP)
    {
        return "已连接";
    }
    if (connection->carrier == ETHERNET_CARRIER_DOWN)
    {
        return "网线未接";
    }
    return "已启用";
}

static void print_connection_card(const SavedConnection *connection, int number)
{
    printf("\n  %d. %s  [%s]\n", number, connection->name,
           connection_status_text(connection));
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
    printf("     开机自动连接：%s\n",
           connection->autoconnect ? "是" : "否");
    printf("     路由优先级：%s\n",
           connection->route_metric[0] &&
                   strcmp(connection->route_metric, "-1") != 0
               ? connection->route_metric
               : "自动");
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
    printf("当前状态：%s\n", connection_status_text(connection));
    if (connection->active)
    {
        printf("使用网卡：%s\n", connection->device);
    }
    if (connection->kind == SAVED_CONNECTION_WIFI)
    {
        printf("无线名称：%s\n", connection->ssid[0] ? connection->ssid : "未读取到");
        printf("密码方式：%s\n", security_text(connection->security));
        printf("隐藏网络：%s\n", text_is_yes(connection->hidden) ? "是" : "否");
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
    printf("开机自动连接：%s\n", connection->autoconnect ? "是" : "否");
    printf("路由优先级：%s%s\n",
           connection->route_metric[0] &&
                   strcmp(connection->route_metric, "-1") != 0
               ? connection->route_metric
               : "自动",
           connection->route_metric[0] &&
                   strcmp(connection->route_metric, "-1") != 0
               ? "（数值越小越优先）"
               : "");
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
    {
        size_t gateway_length = strnlen(connection->gateway,
                                        sizeof(cfg->gateway) - 1);
        size_t dns_length = strnlen(connection->dns,
                                    sizeof(cfg->dns) - 1);

        memcpy(cfg->gateway, connection->gateway, gateway_length);
        cfg->gateway[gateway_length] = '\0';
        memcpy(cfg->dns, connection->dns, dns_length);
        cfg->dns[dns_length] = '\0';
    }
}

static bool valid_wifi_password(const char *password)
{
    size_t length = password ? strlen(password) : 0;

    if (length >= 8 && length <= 63)
    {
        return true;
    }
    if (length != 64)
    {
        return false;
    }
    for (size_t index = 0; index < length; ++index)
    {
        char character = password[index];

        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F')))
        {
            return false;
        }
    }
    return true;
}

static bool saved_connection_profile(const SavedConnection *connection,
                                     ConnectionProfile *profile)
{
    size_t name_length;

    if (!connection || !profile || connection->name[0] == '\0' ||
        (name_length = strlen(connection->name)) >= sizeof(profile->name))
    {
        printf("[失败] 连接名称过长，无法建立安全修改事务。\n");
        return false;
    }

    memset(profile, 0, sizeof(*profile));
    memcpy(profile->name, connection->name, name_length + 1);
    snprintf(profile->uuid, sizeof(profile->uuid), "%s", connection->uuid);
    return true;
}

static bool create_edit_profile(const SavedConnection *connection,
                                ConnectionProfile *profile)
{
    const char *name_part;

    if (!connection || !profile)
    {
        return false;
    }
    name_part = connection->active && connection->device[0]
        ? connection->device
        : "saved-edit";
    if (!build_temp_profile(name_part, profile))
    {
        printf("[失败] 无法生成临时修改连接。\n");
        return false;
    }

    {
        const char *clone[] = {
            "nmcli", "connection", "clone", "uuid", connection->uuid,
            profile->name, NULL};

        if (run_cmd(clone) != 0)
        {
            printf("[失败] 无法复制原连接，未修改任何设置。\n");
            return false;
        }
    }
    if (!bind_created_profile_uuid(profile))
    {
        return false;
    }

    {
        const char *disable[] = {
            "nmcli", "connection", "modify", "uuid", profile->uuid,
            "connection.autoconnect", "no", NULL};

        if (run_cmd(disable) != 0)
        {
            printf("[失败] 无法关闭临时连接的自动启用。\n");
            cleanup_temp_profile(profile);
            return false;
        }
    }
    return true;
}

typedef enum
{
    EDIT_FINISH_FAILED = 0,
    EDIT_FINISH_DONE,
    EDIT_FINISH_BACK
} EditFinishResult;

static EditFinishResult finish_staged_edit(
    const SavedConnection *connection,
    ConnectionProfile *edited_profile,
    const IPv4Config *cfg)
{
    ConnectionProfile original;
    bool active_now;

    if (!connection || !edited_profile || !cfg ||
        !saved_connection_profile(connection, &original))
    {
        cleanup_temp_profile(edited_profile);
        return EDIT_FINISH_FAILED;
    }

    active_now = connection_is_active(connection->uuid);
    if (active_now != connection->active)
    {
        printf("[提醒] 连接状态已变化，为避免改错，本次修改已取消，请重新进入列表。\n");
        cleanup_temp_profile(edited_profile);
        return EDIT_FINISH_FAILED;
    }

    if (!connection->active)
    {
        if (!finalize_edited_profile(edited_profile,
                                     connection->name,
                                     &original))
        {
            cleanup_temp_profile(edited_profile);
            return EDIT_FINISH_FAILED;
        }
        printf("[完成] 修改已保存；该连接下次启用时使用新设置。\n");
        return EDIT_FINISH_DONE;
    }

    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            connection->device,
            edited_profile,
            cfg,
            connection->name,
            NETWORK_PROFILE_EDIT);

        if (result == NETWORK_ACTIVATION_FAILED)
        {
            if (terminal_ui_back_requested())
            {
                return EDIT_FINISH_BACK;
            }
            cleanup_temp_profile(edited_profile);
            return EDIT_FINISH_FAILED;
        }
        if (result == NETWORK_ACTIVATION_PENDING)
        {
            terminal_ui_shutdown();
            printf("[待确认] 请通过新地址重新登录并再次运行本工具。\n");
            exit(EXIT_SUCCESS);
        }
    }

    printf("[完成] 修改已经应用到当前连接。\n");
    return EDIT_FINISH_DONE;
}

static void use_saved_connection(const SavedConnection *connection)
{
    char iface[IF_NAMESIZE];
    ConnectionProfile profile = {0};
    IPv4Config cfg;
    NetworkActivationResult result;

    if (connection->active)
    {
        printf("\n这项连接已启用。\n");
        return;
    }

    printf("\n请选择用哪张%s网卡连接。\n",
           connection->kind == SAVED_CONNECTION_WIFI ? "无线" : "有线");
choose_interface_step:
    terminal_ui_set_step("选择连接使用的网卡",
                         "选择用于启用这项保存连接的网卡");
    terminal_ui_prepare_step(true);
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
        snprintf(profile.name, sizeof(profile.name), "Saved-Connection");
    }
    snprintf(profile.uuid, sizeof(profile.uuid), "%s", connection->uuid);
    ipv4_config_from_connection(connection, &cfg);
    terminal_ui_prepare_step(true);
    result = safe_activate_with_reconnect(
        iface, &profile, &cfg, "", NETWORK_PROFILE_EXISTING);
    if (result == NETWORK_ACTIVATION_CONFIRMED)
    {
        printf("\n[完成] 已启用“%s”。\n", connection->name);
    }
    else if (result == NETWORK_ACTIVATION_PENDING)
    {
        terminal_ui_shutdown();
        printf("\n[待确认] 请通过新地址重新登录并再次运行本工具。\n");
        exit(EXIT_SUCCESS);
    }
    else if (terminal_ui_back_requested())
    {
        goto choose_interface_step;
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
        terminal_ui_set_step("确认删除已启用连接",
                             "删除可能立即断网，并且没有自动回滚");
        printf("\n[高风险] “%s”已启用。\n", connection->name);
        printf("删除后对应网卡可能立即断网。\n");
        printf("本操作没有超时回滚，恢复时需要重新创建连接。\n");
        if (ssh_path)
        {
            printf("[当前 SSH] 该连接正在承载本次远程会话，"
                   "确认后 SSH 预计会立即断开。\n");
            printf("请先确认已有串口、HDMI 或其他可用网络。\n");
        }
        terminal_ui_prepare_step(true);
        if (!read_exact_word(
                "确认删除已启用连接请输入 DELETE ACTIVE: ",
                "DELETE ACTIVE"))
        {
            printf("已取消。\n");
            return false;
        }

        /* SSH 断开可能向会话进程发送信号，确保 nmcli 能执行完。 */
        if (signal(SIGHUP, SIG_IGN) == SIG_ERR ||
            signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        {
            printf("[拒绝] 无法启用断线保护，未删除已启用连接。\n");
            return false;
        }
    }

    if (!active_now)
    {
        terminal_ui_set_step("确认删除连接",
                             "删除后需要重新创建才能再次使用");
    }
    printf("\n即将删除“%s”。删除后需要重新填写才能再次使用。\n",
           connection->name);
    terminal_ui_prepare_step(true);
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
            printf("[失败] 无法读取冲突网卡的已启用连接。\n");
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
            printf("\n可处理的冲突连接：\n");
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
                    printf("\n不能由本工具直接修改的冲突网卡：\n");
                    has_unmanaged = true;
                }
                printf("  - %s：没有找到活动的有线/Wi-Fi "
                       "NetworkManager 配置，可能是虚拟网卡或手工地址\n",
                       interfaces[iface_index]);
            }
        }

        if (candidate_count == 0)
        {
            printf("\n[提醒] 没有找到可安全映射并修改的冲突连接。\n");
            return;
        }

        {
            int action;
            int selected;
            SavedConnection *selected_connection;

            printf("\n处理方式（程序不会自动选择连接或优先级）：\n");
            printf("  1. 手动选择连接并设置路由优先级\n");
            printf("  2. 删除确认不再需要的连接\n");
            printf("  0. 暂不处理\n");
            terminal_ui_set_step("选择网段冲突处理方式",
                                 "所有配置变更都需要用户明确选择和确认");
            action = read_int("请选择 [0-2]: ", 0, 2);
            if (action == TERMINAL_UI_INPUT_CANCELLED || action == 0)
            {
                printf("已保持当前网络配置。\n");
                return;
            }

            printf("\n请选择要%s的连接：\n",
                   action == 1 ? "修改优先级" : "删除");
            for (int index = 0; index < candidate_count; ++index)
            {
                print_connection_card(
                    &connections[candidates[index]], index + 1);
            }
            printf("  0. 返回处理方式\n");
            terminal_ui_set_step(action == 1
                                     ? "选择优先级修改对象"
                                     : "选择要删除的冲突连接",
                                 action == 1
                                     ? "只会修改所选连接的路由优先级"
                                     : "只删除确认不再需要的一侧");
            selected = read_int(action == 1
                                    ? "请选择要修改优先级的连接: "
                                    : "请选择要删除的冲突连接: ",
                                0, candidate_count);
            if (selected == TERMINAL_UI_INPUT_CANCELLED || selected == 0)
            {
                printf("已保持当前网络配置。\n");
                return;
            }
            selected_connection = &connections[candidates[selected - 1]];

            if (action == 1)
            {
                if (!change_route_metric_interactively(
                        selected_connection->uuid,
                        selected_connection->name,
                        selected_connection->device,
                        selected_connection->route_metric))
                {
                    return;
                }
                printf("\n正在按用户设置重新检查网段冲突...\n");
                continue;
            }

            printf("\n删除连接是永久操作，不只是临时断开。\n");
            printf("已启用或承载 SSH 的连接会要求高风险确认。\n");
            if (!remove_saved_connection(selected_connection))
            {
                return;
            }

            printf("正在等待系统清理旧 IP 和网络路径...\n");
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

static bool edit_target_interface(const SavedConnection *connection,
                                  char *iface,
                                  size_t iface_size)
{
    if (connection->active && connection->device[0] != '\0')
    {
        snprintf(iface, iface_size, "%s", connection->device);
        return true;
    }
    if (connection->interface_name[0] != '\0' &&
        strcmp(connection->interface_name, "--") != 0)
    {
        snprintf(iface, iface_size, "%s", connection->interface_name);
        return true;
    }

    printf("该连接未绑定网卡，请选择要使用的%s网卡。\n",
           connection->kind == SAVED_CONNECTION_WIFI ? "无线" : "有线");
    terminal_ui_set_step("选择修改使用的网卡",
                         "未绑定网卡的连接需要先选择目标设备");
    terminal_ui_prepare_step(true);
    if (connection->kind == SAVED_CONNECTION_WIFI)
    {
        return choose_wifi_interface(iface, iface_size);
    }
    return choose_ethernet_interface(iface, iface_size);
}

static void configure_saved_connection_ipv4(
    const SavedConnection *connection)
{
    ConnectionProfile edited_profile;
    IPv4Config cfg;
    char iface[IF_NAMESIZE];
    int route_metric;

    printf("\n========== 修改 IPv4 网络参数 =========="
           "\n连接：%s\n", connection->name);
    printf("可以重新设置自动/固定 IP、子网掩码、网关、DNS 和路由优先级。\n");

    if (!edit_target_interface(connection, iface, sizeof(iface)) ||
        !create_edit_profile(connection, &edited_profile))
    {
        return;
    }

    if (!connection->active &&
        (connection->interface_name[0] == '\0' ||
         strcmp(connection->interface_name, "--") == 0))
    {
        const char *bind[] = {
            "nmcli", "connection", "modify", "uuid", edited_profile.uuid,
            "connection.interface-name", iface, NULL};

        if (run_cmd(bind) != 0)
        {
            printf("[失败] 无法把修改后的连接绑定到 %s。\n", iface);
            cleanup_temp_profile(&edited_profile);
            return;
        }
    }

ipv4_settings_step:
    if (configure_ipv4_profile(edited_profile.uuid, &cfg,
                               &route_metric) != 0)
    {
        if (!terminal_ui_back_requested())
        {
            printf("[失败] IPv4 参数没有写入临时修改连接。\n");
        }
        cleanup_temp_profile(&edited_profile);
        return;
    }
    terminal_ui_prepare_step(true);
    if (cfg.is_static &&
        !approve_candidate_network(iface, cfg.ip, cfg.prefix, route_metric))
    {
        if (terminal_ui_back_requested())
        {
            goto ipv4_settings_step;
        }
        printf("网段检查未通过，本次修改已取消。\n");
        cleanup_temp_profile(&edited_profile);
        return;
    }

ipv4_confirmation_step:
    printf("路由优先级: %s",
           route_metric == IPV4_ROUTE_METRIC_AUTO ? "自动" : "metric ");
    if (route_metric != IPV4_ROUTE_METRIC_AUTO)
    {
        printf("%d", route_metric);
    }
    printf("\n");
    if (!confirm_network_parameters(iface, &cfg))
    {
        if (terminal_ui_back_requested())
        {
            goto ipv4_settings_step;
        }
        printf("已取消，原连接没有变化。\n");
        cleanup_temp_profile(&edited_profile);
        return;
    }

    if (finish_staged_edit(connection, &edited_profile, &cfg) ==
        EDIT_FINISH_BACK)
    {
        goto ipv4_confirmation_step;
    }
}

static bool configure_saved_wifi_identity(
    const SavedConnection *connection)
{
    ConnectionProfile edited_profile;
    IPv4Config cfg;
    char ssid[BUF_SIZE];
    char input[BUF_SIZE];
    char password[BUF_SIZE] = {0};
    bool hidden = text_is_yes(connection->hidden);
    int hidden_choice;
    int security_choice;

    snprintf(ssid, sizeof(ssid), "%s", connection->ssid);
ssid_step:
    terminal_ui_set_step("修改 Wi-Fi 名称",
                         "输入新的 SSID，直接确认可保留当前值");
    printf("\n========== 修改无线名称和密码 =========="
           "\n连接：%s\n", connection->name);
    printf("当前 SSID：%s\n", connection->ssid[0] ? connection->ssid : "未读取到");
    printf("当前密码方式：%s\n", security_text(connection->security));

    for (;;)
    {
        terminal_ui_prepare_step(true);
        if (!read_line("新 SSID（直接回车保留当前值）: ",
                       input, sizeof(input)))
        {
            return false;
        }
        trim_space(input);
        if (input[0] == '\0')
        {
            break;
        }
        if (strlen(input) <= 32)
        {
            snprintf(ssid, sizeof(ssid), "%s", input);
            break;
        }
        printf("无线网络名称不能超过 32 个字节。\n");
    }
    if (ssid[0] == '\0')
    {
        printf("[失败] 当前连接没有可保留的 SSID，请输入新的无线名称。\n");
        return false;
    }

hidden_step:
    terminal_ui_set_step("修改隐藏网络设置",
                         "选择是否广播 Wi-Fi 名称");
    printf("\n隐藏网络设置：\n");
    printf("  1. 保持当前设置（%s）\n", hidden ? "是" : "否");
    printf("  2. 是，SSID 不广播\n");
    printf("  3. 否，普通可见网络\n");
    terminal_ui_prepare_step(true);
    hidden_choice = read_int("请选择 [1-3]: ", 1, 3);
    if (hidden_choice == TERMINAL_UI_INPUT_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto ssid_step;
        }
        return false;
    }
    if (hidden_choice == 2)
    {
        hidden = true;
    }
    else if (hidden_choice == 3)
    {
        hidden = false;
    }

security_step:
    terminal_ui_set_step("修改 Wi-Fi 安全方式",
                         "保留现有认证，或选择新的密码方式");
    printf("\n无线密码方式：\n");
    printf("  1. 保留当前密码和认证方式\n");
    printf("  2. WPA/WPA2，填写新密码\n");
    printf("  3. WPA3，填写新密码\n");
    printf("  4. 改为无密码网络\n");
    terminal_ui_prepare_step(true);
    security_choice = read_int("请选择 [1-4]: ", 1, 4);
    if (security_choice == TERMINAL_UI_INPUT_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto hidden_step;
        }
        return false;
    }
password_step:
    if (security_choice == 2 || security_choice == 3)
    {
        terminal_ui_set_step("填写新的 Wi-Fi 密码",
                             "密码不会明文显示");
        do
        {
            terminal_ui_prepare_step(true);
            if (!read_password("新 Wi-Fi 密码（输入时不显示）: ",
                               password, sizeof(password)))
            {
                memset(password, 0, sizeof(password));
                if (terminal_ui_back_requested())
                {
                    goto security_step;
                }
                return false;
            }
            if (!valid_wifi_password(password))
            {
                printf("密码需要是 8 到 63 个字符，或 64 位十六进制内容。\n");
            }
        } while (!valid_wifi_password(password));
    }

identity_confirmation_step:
    terminal_ui_set_step("确认 Wi-Fi 修改",
                         "核对名称、隐藏属性和密码方式");
    printf("\n修改后 SSID：%s%s\n", ssid, hidden ? "（隐藏）" : "");
    printf("修改后密码方式：%s\n",
           security_choice == 1
               ? security_text(connection->security)
               : security_choice == 2
                     ? "WPA/WPA2"
                     : security_choice == 3 ? "WPA3" : "无密码");
    terminal_ui_prepare_step(true);
    if (!read_exact_yes("确认修改请输入 YES；其他输入取消: "))
    {
        if (terminal_ui_back_requested())
        {
            if (security_choice == 2 || security_choice == 3)
            {
                goto password_step;
            }
            goto security_step;
        }
        printf("已取消，原连接没有变化。\n");
        memset(password, 0, sizeof(password));
        return false;
    }

    if (!create_edit_profile(connection, &edited_profile))
    {
        memset(password, 0, sizeof(password));
        return true;
    }

    {
        const char *modify_wifi[] = {
            "nmcli", "connection", "modify", "uuid", edited_profile.uuid,
            "802-11-wireless.ssid", ssid,
            "802-11-wireless.hidden", hidden ? "yes" : "no", NULL};

        if (run_cmd(modify_wifi) != 0)
        {
            printf("[失败] 无线名称或隐藏属性修改失败。\n");
            cleanup_temp_profile(&edited_profile);
            memset(password, 0, sizeof(password));
            return true;
        }
    }

    if (security_choice != 1)
    {
        int security_result;

        if (security_choice == 4)
        {
            const char *remove_security[] = {
                "nmcli", "connection", "modify", "uuid", edited_profile.uuid,
                "remove", "802-11-wireless-security", NULL};

            security_result = run_cmd(remove_security);
        }
        else
        {
            const char *key_management = security_choice == 2
                ? "wpa-psk"
                : "sae";
            const char *modify_security[] = {
                "nmcli", "connection", "modify", "uuid", edited_profile.uuid,
                "802-11-wireless-security.key-mgmt", key_management,
                "802-11-wireless-security.psk", password, NULL};

            security_result = run_cmd(modify_security);
        }

        if (security_result != 0)
        {
            printf("[失败] 无线密码方式修改失败。\n");
            cleanup_temp_profile(&edited_profile);
            memset(password, 0, sizeof(password));
            return true;
        }
    }
    ipv4_config_from_connection(connection, &cfg);

    if (!connection->active)
    {
        printf("[说明] 连接当前未启用，因此只能保存参数，不能现场验证密码。\n");
    }
    if (finish_staged_edit(connection, &edited_profile, &cfg) ==
        EDIT_FINISH_BACK)
    {
        cleanup_temp_profile(&edited_profile);
        goto identity_confirmation_step;
    }
    memset(password, 0, sizeof(password));
    return true;
}

static void configure_saved_connection_name(
    const SavedConnection *connection)
{
    char new_name[PROFILE_SIZE];
    char existing_uuid[UUID_SIZE] = {0};
    int matches;

name_step:
    terminal_ui_set_step("修改连接名称",
                         "名称仅用于系统中识别，不改变网络参数");
    if (!ask_final_profile_name(connection->name,
                                new_name,
                                sizeof(new_name)))
    {
        return;
    }
    if (strcmp(new_name, connection->name) == 0)
    {
        printf("连接名称没有变化。\n");
        return;
    }
    matches = find_connection_uuid_by_name(new_name,
                                            existing_uuid,
                                            sizeof(existing_uuid));
    if (matches < 0)
    {
        printf("[失败] 无法检查重名连接。\n");
        return;
    }
    if (matches > 0)
    {
        printf("[失败] 已存在名为“%s”的连接，请换一个名称。\n", new_name);
        return;
    }
    printf("将把“%s”改名为“%s”。\n", connection->name, new_name);
    terminal_ui_prepare_step(true);
    if (!read_yes_no("确定改名吗？[y/N]: ", false))
    {
        if (terminal_ui_back_requested())
        {
            goto name_step;
        }
        printf("已取消。\n");
        return;
    }

    {
        const char *rename[] = {
            "nmcli", "connection", "modify", "uuid", connection->uuid,
            "connection.id", new_name, NULL};

        if (run_cmd(rename) == 0)
        {
            printf("[完成] 连接已改名，不影响当前 IP 和网络状态。\n");
        }
        else
        {
            printf("[失败] 连接名称没有修改成功。\n");
        }
    }
}

static void configure_saved_connection_autoconnect(
    const SavedConnection *connection)
{
    int choice;
    const char *value;

    terminal_ui_set_step("修改自动连接",
                         "选择设备是否在网络可用时自动启用此连接");
    printf("\n当前自动连接：%s\n", connection->autoconnect ? "是" : "否");
    printf("  1. 开启\n");
    printf("  2. 关闭\n");
    printf("  0. 取消\n");
    choice = read_int("请选择 [0-2]: ", 0, 2);
    if (choice == TERMINAL_UI_INPUT_CANCELLED)
    {
        return;
    }
    if (choice == 0)
    {
        printf("已取消。\n");
        return;
    }
    value = choice == 1 ? "yes" : "no";
    if ((choice == 1) == connection->autoconnect)
    {
        printf("设置没有变化。\n");
        return;
    }

    {
        const char *modify[] = {
            "nmcli", "connection", "modify", "uuid", connection->uuid,
            "connection.autoconnect", value, NULL};

        if (run_cmd(modify) == 0)
        {
            printf("[完成] 自动连接已%s；当前连接不会因此立即断开。\n",
                   choice == 1 ? "开启" : "关闭");
        }
        else
        {
            printf("[失败] 自动连接设置没有保存成功。\n");
        }
    }
}

static bool configure_saved_connection_details(
    const SavedConnection *connection)
{
    int choice;
    int maximum = connection->kind == SAVED_CONNECTION_WIFI ? 4 : 3;

details_menu:
    terminal_ui_set_step("选择修改内容",
                         "选择要修改的连接参数");
    printf("\n========== 修改已配置网络 =========="
           "\n连接：%s\n", connection->name);
    printf("  1. 修改 IPv4、网关、DNS 和路由优先级\n");
    if (connection->kind == SAVED_CONNECTION_WIFI)
    {
        printf("  2. 修改无线名称、隐藏属性或密码\n");
        printf("  3. 修改连接显示名称\n");
        printf("  4. 修改自动连接\n");
    }
    else
    {
        printf("  2. 修改连接显示名称\n");
        printf("  3. 修改自动连接\n");
    }
    printf("  0. 返回\n");
    choice = read_int(connection->kind == SAVED_CONNECTION_WIFI
                          ? "请选择 [0-4]: "
                          : "请选择 [0-3]: ",
                      0, maximum);
    if (choice == TERMINAL_UI_INPUT_CANCELLED)
    {
        return false;
    }
    if (choice == 0)
    {
        return false;
    }
    if (choice == 1)
    {
        configure_saved_connection_ipv4(connection);
        if (terminal_ui_back_requested())
        {
            goto details_menu;
        }
        return true;
    }
    if (connection->kind == SAVED_CONNECTION_WIFI && choice == 2)
    {
        if (!configure_saved_wifi_identity(connection))
        {
            goto details_menu;
        }
        return true;
    }
    if (choice == (connection->kind == SAVED_CONNECTION_WIFI ? 3 : 2))
    {
        terminal_ui_prepare_step(true);
        configure_saved_connection_name(connection);
        if (terminal_ui_back_requested())
        {
            goto details_menu;
        }
        return true;
    }
    terminal_ui_prepare_step(true);
    configure_saved_connection_autoconnect(connection);
    if (terminal_ui_back_requested())
    {
        goto details_menu;
    }
    return true;
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
    printf("新 DNS 作为手工地址加入，不影响 DHCP 的 DNS；无 DNS 时可填现场内网或公共 DNS。\n");

dns_step:
    terminal_ui_set_step("填写 DNS 服务器",
                         "最多填写 4 个 IPv4 地址");
    for (;;)
    {
        terminal_ui_prepare_step(true);
        if (!read_line("请输入 DNS（IPv4，最多 4 个，逗号分隔；回车取消）: ",
                       input, sizeof(input)))
        {
            return;
        }
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

    terminal_ui_set_step("确认 DNS 修改",
                         "核对解析后的 DNS 地址");
    printf("程序解析为：%s\n", dns);
    terminal_ui_prepare_step(true);
    if (!read_yes_no("确定保存到这项连接吗？[y/N]: ", false))
    {
        if (terminal_ui_back_requested())
        {
            goto dns_step;
        }
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

static void configure_saved_connection_priority(
    const SavedConnection *connection)
{
    int route_metric;
    char metric_text[16];

    if (!connection)
    {
        return;
    }

    printf("\n========== 修改路由优先级 ==========\n");
    printf("连接：%s\n", connection->name);
    printf("当前优先级：%s\n",
           connection->route_metric[0] &&
                   strcmp(connection->route_metric, "-1") != 0
               ? connection->route_metric
               : "自动");
priority_step:
    terminal_ui_set_step("修改路由优先级",
                         "数值越小，系统越优先使用此连接");
    route_metric = read_ipv4_route_metric();
    if (route_metric == TERMINAL_UI_INPUT_CANCELLED)
    {
        return;
    }
    snprintf(metric_text, sizeof(metric_text), "%d", route_metric);
    printf("将保存为：%s\n",
           route_metric == IPV4_ROUTE_METRIC_AUTO
               ? "自动"
               : metric_text);

    if (connection->active)
    {
        printf("[提醒] 连接已启用，应用后可能改变默认联网方式。\n");
        if (interface_is_ssh_path(connection->device))
        {
            printf("[重要提醒] 当前 SSH 正通过 %s，路由切换可能中断会话。\n",
                   connection->device);
        }
    }
    terminal_ui_set_step("确认路由优先级",
                         "已启用连接可能立即改变默认联网路径");
    terminal_ui_prepare_step(true);
    if (!read_yes_no("确定保存这个优先级吗？[y/N]: ", false))
    {
        if (terminal_ui_back_requested())
        {
            goto priority_step;
        }
        printf("已取消。\n");
        return;
    }

    {
        const char *modify[] = {
            "nmcli", "connection", "modify", "uuid", connection->uuid,
            "ipv4.route-metric", metric_text, NULL};

        if (run_cmd(modify) != 0)
        {
            printf("[失败] 路由优先级没有保存成功。\n");
            return;
        }
    }
    if (!connection->active)
    {
        printf("[完成] 优先级已保存，这项连接下次启用时生效。\n");
        return;
    }

    {
        const char *reapply[] = {
            "nmcli", "device", "reapply", connection->device, NULL};

        if (run_cmd(reapply) != 0)
        {
            printf("[已保存] 当前无法无断线重新应用；重新连接或重启后生效。\n");
            return;
        }
    }
    printf("[完成] 优先级已应用，正在按实时路由重新检查网段。\n");
    (void)show_current_overlap_warnings();
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
        terminal_ui_set_step("选择已保存的连接",
                             "选择连接查看详情或进行管理");
        selected = read_int("请选择一项连接: ", 0, count);
        if (selected == TERMINAL_UI_INPUT_CANCELLED)
        {
            return;
        }
        if (selected == 0)
        {
            return;
        }

        for (;;)
        {
            int action;
            SavedConnection *connection = &connections[selected - 1];

            print_selected_detail(connection);
            terminal_ui_set_step("选择连接操作",
                                 "查看当前状态后选择要执行的操作");
            if (connection->active)
            {
                printf("1. 修改这项连接的网络信息\n");
                printf("2. 单独补充/修改 DNS（尽量无断线生效）\n");
                printf("3. 修改路由优先级\n");
                printf("4. 删除这项已启用的连接（可能断网）\n");
                printf("0. 返回连接列表\n");
                action = read_int("请选择操作 [0-4]: ", 0, 4);
                if (action == TERMINAL_UI_INPUT_CANCELLED)
                {
                    break;
                }
                if (action == 1)
                {
                    if (!configure_saved_connection_details(connection))
                    {
                        continue;
                    }
                }
                else if (action == 2)
                {
                    configure_saved_connection_dns(connection);
                    if (terminal_ui_back_requested())
                    {
                        continue;
                    }
                }
                else if (action == 3)
                {
                    configure_saved_connection_priority(connection);
                    if (terminal_ui_back_requested())
                    {
                        continue;
                    }
                }
                else if (action == 4)
                {
                    remove_saved_connection(connection);
                    if (terminal_ui_back_requested())
                    {
                        continue;
                    }
                }
                break;
            }
            printf("1. 使用这项连接\n");
            printf("2. 修改这项连接的网络信息\n");
            printf("3. 单独补充/修改 DNS\n");
            printf("4. 修改路由优先级\n");
            printf("5. 删除这项连接\n");
            printf("0. 返回连接列表\n");
            action = read_int("请选择操作 [0-5]: ", 0, 5);
            if (action == TERMINAL_UI_INPUT_CANCELLED || action == 0)
            {
                break;
            }
            if (action == 1)
            {
                use_saved_connection(connection);
                if (terminal_ui_back_requested())
                {
                    continue;
                }
                break;
            }
            if (action == 2)
            {
                if (!configure_saved_connection_details(connection))
                {
                    continue;
                }
                break;
            }
            if (action == 3)
            {
                configure_saved_connection_dns(connection);
                if (terminal_ui_back_requested())
                {
                    continue;
                }
                break;
            }
            if (action == 4)
            {
                configure_saved_connection_priority(connection);
                if (terminal_ui_back_requested())
                {
                    continue;
                }
                break;
            }
            remove_saved_connection(connection);
            if (terminal_ui_back_requested())
            {
                continue;
            }
            break;
        }
    }
}
