#define _POSIX_C_SOURCE 200809L

#include "saved_connections.h"

#include "cli_io.h"
#include "command_runner.h"
#include "ipv4_utils.h"
#include "netconfig_types.h"
#include "nmcli_parser.h"
#include "network_safety.h"
#include "network_state.h"

#include <limits.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAVED_CONNECTIONS 128
#define CONNECTION_LIST_SIZE 65536
#define DETAIL_SIZE 1024
#define SAVED_NAME_SIZE 512

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
    if (connection->dns[0])
    {
        printf("DNS：%s\n", connection->dns);
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

static void remove_saved_connection(const SavedConnection *connection)
{
    if (connection->active)
    {
        printf("\n这项连接正在使用，不能删除。请先切换到其他连接。\n");
        return;
    }

    printf("\n即将删除“%s”。删除后需要重新填写才能再次使用。\n",
           connection->name);
    if (!read_yes_no("确定删除吗？[y/N]: ", false))
    {
        printf("已取消。\n");
        return;
    }
    if (delete_connection_by_uuid(connection->uuid) == 0)
    {
        printf("[完成] 已删除“%s”。\n", connection->name);
    }
    else
    {
        printf("[失败] 没有删除成功，请稍后重试。\n");
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
                printf("这项连接正在使用，不能重复使用或直接删除。\n");
                printf("0. 返回连接列表\n");
                (void)read_int("请选择操作 [0]: ", 0, 0);
                break;
            }
            printf("1. 使用这项连接\n");
            printf("2. 删除这项连接\n");
            printf("0. 返回连接列表\n");
            action = read_int("请选择操作 [0-2]: ", 0, 2);
            if (action == 0)
            {
                break;
            }
            if (action == 1)
            {
                use_saved_connection(connection);
                break;
            }
            remove_saved_connection(connection);
            break;
        }
    }
}
