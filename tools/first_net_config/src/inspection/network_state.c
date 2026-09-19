#define _POSIX_C_SOURCE 200809L

#include "inspection/network_state.h"
#include "common/cli_io.h"
#include "common/command_runner.h"
#include "inspection/interface_inspector.h"
#include "common/netconfig_types.h"
#include "common/nmcli_parser.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_NETWORK_SUMMARY_ROWS 64
#define NETWORK_SUMMARY_OUTPUT_SIZE 65536
#define NETWORK_SUMMARY_NAME_SIZE 512
#define NETWORK_SUMMARY_VALUE_SIZE 1024

static bool interface_exists(const char *ifname)
{
    return ifname && if_nametoindex(ifname) != 0;
}

static const char *friendly_device_state(const char *state)
{
    if (!state || state[0] == '\0')
    {
        return "未知";
    }
    if (strstr(state, "disconnected") != NULL)
    {
        return "未连接";
    }
    if (strstr(state, "connecting") != NULL)
    {
        return "正在连接";
    }
    if (strstr(state, "connected") != NULL)
    {
        return "已连接";
    }
    if (strstr(state, "unavailable") != NULL)
    {
        return "暂不可用";
    }
    if (strstr(state, "unmanaged") != NULL)
    {
        return "未由系统管理";
    }
    return state;
}

static const char *friendly_network_config_state(const char *state)
{
    if (!state || state[0] == '\0')
    {
        return "未知";
    }
    if (strstr(state, "disconnected") != NULL)
    {
        return "未启用";
    }
    if (strstr(state, "connecting") != NULL)
    {
        return "启用中";
    }
    if (strstr(state, "connected") != NULL)
    {
        return "已启用";
    }
    if (strstr(state, "unavailable") != NULL)
    {
        return "暂不可用";
    }
    if (strstr(state, "unmanaged") != NULL)
    {
        return "未由系统管理";
    }
    return state;
}

EthernetCarrierState get_ethernet_carrier_state(const char *iface)
{
    char path[256];
    char value[16] = {0};
    FILE *handle;
    int written;

    if (!iface || iface[0] == '\0')
    {
        return ETHERNET_CARRIER_UNKNOWN;
    }
    written = snprintf(path, sizeof(path),
                       "/sys/class/net/%s/carrier", iface);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
        return ETHERNET_CARRIER_UNKNOWN;
    }
    handle = fopen(path, "r");
    if (!handle)
    {
        return ETHERNET_CARRIER_UNKNOWN;
    }
    if (!fgets(value, sizeof(value), handle))
    {
        fclose(handle);
        return ETHERNET_CARRIER_UNKNOWN;
    }
    fclose(handle);

    if (value[0] == '1')
    {
        return ETHERNET_CARRIER_UP;
    }
    if (value[0] == '0')
    {
        return ETHERNET_CARRIER_DOWN;
    }
    return ETHERNET_CARRIER_UNKNOWN;
}

static const char *ethernet_carrier_state(const char *iface)
{
    EthernetCarrierState state = get_ethernet_carrier_state(iface);

    if (state == ETHERNET_CARRIER_UP)
    {
        return "已接";
    }
    if (state == ETHERNET_CARRIER_DOWN)
    {
        return "未接";
    }
    return "未知";
}

static bool valid_uuid_text(const char *uuid)
{
    if (!uuid || strlen(uuid) != UUID_SIZE - 1)
    {
        return false;
    }

    for (size_t i = 0; i < UUID_SIZE - 1; ++i)
    {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;

        if ((dash && uuid[i] != '-') ||
            (!dash && !((uuid[i] >= '0' && uuid[i] <= '9') ||
                        (uuid[i] >= 'a' && uuid[i] <= 'f') ||
                        (uuid[i] >= 'A' && uuid[i] <= 'F'))))
        {
            return false;
        }
    }

    return true;
}

bool generate_connection_uuid(char *out, size_t out_size)
{
    FILE *fp;

    if (!out || out_size < UUID_SIZE)
    {
        return false;
    }

    fp = fopen("/proc/sys/kernel/random/uuid", "r");
    if (!fp)
    {
        return false;
    }

    if (!fgets(out, (int)out_size, fp))
    {
        fclose(fp);
        return false;
    }

    fclose(fp);
    trim_space(out);
    return valid_uuid_text(out);
}

static void nmcli_unescape(char *text)
{
    char *src = text;
    char *dst = text;

    while (*src)
    {
        if (*src == '\\' && src[1] != '\0')
        {
            ++src;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

int find_connection_uuid_by_name(const char *name, char *out, size_t out_size)
{
    char result[CMD_OUT_SIZE];
    int matches = 0;
    const char *argv[] = {
        "nmcli", "-t", "-f", "UUID,NAME",
        "connection", "show", NULL};

    if (!name || !out || out_size < UUID_SIZE ||
        capture_cmd(argv, result, sizeof(result)) != 0)
    {
        return -1;
    }

    {
        char *saveptr = NULL;
        char *line = strtok_r(result, "\n", &saveptr);

        while (line)
        {
            if (strlen(line) > UUID_SIZE - 1 &&
                line[UUID_SIZE - 1] == ':')
            {
                char uuid[UUID_SIZE];
                char *candidate_name = line + UUID_SIZE;

                memcpy(uuid, line, UUID_SIZE - 1);
                uuid[UUID_SIZE - 1] = '\0';
                nmcli_unescape(candidate_name);

                if (valid_uuid_text(uuid) && strcmp(candidate_name, name) == 0)
                {
                    ++matches;
                    if (matches == 1)
                    {
                        snprintf(out, out_size, "%s", uuid);
                    }
                }
            }

            line = strtok_r(NULL, "\n", &saveptr);
        }
    }

    if (matches != 1)
    {
        out[0] = '\0';
    }

    return matches;
}

bool resolve_connection_uuid(const char *selector, char *out, size_t out_size)
{
    const char *argv[] = {
        "nmcli", "-g", "connection.uuid",
        "connection", "show", "uuid", selector, NULL};

    if (!selector || selector[0] == '\0' || !out || out_size < UUID_SIZE)
    {
        return false;
    }

    if (valid_uuid_text(selector))
    {
        if (capture_cmd(argv, out, out_size) == 0 &&
            valid_uuid_text(out) && strcmp(out, selector) == 0)
        {
            return true;
        }
        return false;
    }

    return find_connection_uuid_by_name(selector, out, out_size) == 1;
}

bool get_connection_name(const char *uuid, char *out, size_t out_size)
{
    const char *argv[] = {
        "nmcli", "-g", "connection.id",
        "connection", "show", "uuid", uuid, NULL};

    return capture_cmd(argv, out, out_size) == 0 && out[0] != '\0';
}

bool connection_is_active(const char *uuid)
{
    char out[CMD_OUT_SIZE];
    const char *argv[] = {
        "nmcli", "-t", "-f", "UUID",
        "connection", "show", "--active", NULL};

    if (!uuid || capture_cmd(argv, out, sizeof(out)) != 0)
    {
        return false;
    }

    {
        char *saveptr = NULL;
        char *line = strtok_r(out, "\n", &saveptr);

        while (line)
        {
            trim_space(line);
            if (strcmp(line, uuid) == 0)
            {
                return true;
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }

    return false;
}

int delete_connection_by_uuid(const char *uuid)
{
    const char *argv[] = {
        "nmcli", "connection", "delete", "uuid", uuid, NULL};

    return run_cmd(argv);
}

bool has_any_ipv4(const char *ifname)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    bool found = false;

    if (getifaddrs(&ifaddr) != 0)
    {
        return false;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr ||
            strcmp(ifa->ifa_name, ifname) != 0 ||
            ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }

        found = true;
        break;
    }

    freeifaddrs(ifaddr);
    return found;
}

bool interface_has_ipv4(const char *ifname, const char *expected_ip)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    bool found = false;

    if (getifaddrs(&ifaddr) != 0)
    {
        return false;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        char addr[INET_ADDRSTRLEN];

        if (!ifa->ifa_addr ||
            strcmp(ifa->ifa_name, ifname) != 0 ||
            ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }

        if (!inet_ntop(AF_INET,
                       &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                       addr,
                       sizeof(addr)))
        {
            continue;
        }

        if (strcmp(addr, expected_ip) == 0)
        {
            found = true;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

bool get_active_connection_uuid(const char *ifname, char *out, size_t out_size)
{
    const char *argv[] = {
        "nmcli", "-g", "GENERAL.CON-UUID",
        "device", "show", ifname, NULL};

    if (capture_cmd(argv, out, out_size) != 0)
    {
        return false;
    }

    return out[0] != '\0' && strcmp(out, "--") != 0;
}

bool default_route_matches(const char *ifname, const char *gateway)
{
    char out[CMD_OUT_SIZE];
    const char *argv[] = {
        "ip", "route", "show", "default", "dev", ifname, NULL};

    if (!ifname || ifname[0] == '\0' ||
        !gateway || gateway[0] == '\0' ||
        capture_cmd(argv, out, sizeof(out)) != 0)
    {
        return false;
    }
    {
        char *line_save = NULL;
        char *line = strtok_r(out, "\n", &line_save);

        while (line)
        {
            char *token_save = NULL;
            char *token = strtok_r(line, " \t", &token_save);

            while (token)
            {
                if (strcmp(token, "via") == 0)
                {
                    char *value = strtok_r(NULL, " \t", &token_save);
                    if (value && strcmp(value, gateway) == 0)
                    {
                        return true;
                    }
                    break;
                }
                token = strtok_r(NULL, " \t", &token_save);
            }
            line = strtok_r(NULL, "\n", &line_save);
        }
    }
    return false;
}

bool choose_ethernet_interface(char *iface, size_t iface_size)
{
    char out[CMD_OUT_SIZE];
    char devices[32][IF_NAMESIZE];
    char states[32][96];
    int count = 0;

    /*
     * nmcli 的 -t (terse) 输出适合程序解析。
     * 格式类似：
     *   eth0:ethernet:connected
     *   eth1:ethernet:unavailable
     *   wlan0:wifi:connected
     */
    {
        const char *argv[] = {
            "nmcli", "-t", "-f", "DEVICE,TYPE,STATE",
            "device", "status", NULL};

        if (capture_cmd(argv, out, sizeof(out)) == 0)
        {
            char *saveptr = NULL;
            char *line = strtok_r(out, "\n", &saveptr);

            while (line && count < 32)
            {
                char *p1 = strchr(line, ':');

                if (p1)
                {
                    char *p2;

                    *p1 = '\0';
                    p2 = strchr(p1 + 1, ':');

                    if (p2)
                    {
                        *p2 = '\0';

                        /*
                         * TYPE 的值是 ethernet / wifi / loopback ...
                         * STATE 可以是中文或英文，我们只负责显示，不依赖它判断类型。
                         */
                        if (strcmp(p1 + 1, "ethernet") == 0 &&
                            interface_is_physical_ethernet(line))
                        {
                            snprintf(devices[count],
                                     sizeof(devices[count]),
                                     "%s", line);

                            snprintf(states[count],
                                     sizeof(states[count]),
                                     "%s", p2 + 1);

                            count++;
                        }
                    }
                }

                line = strtok_r(NULL, "\n", &saveptr);
            }
        }
    }

    /*
     * 某些厂商版 nmcli 输出格式可能有差异。
     * 如果上面的批量解析没有找到 Ethernet，
     * 再逐个检查 Linux 已存在的网络接口。
     */
    if (count == 0)
    {
        struct if_nameindex *ifs = if_nameindex();

        printf("\n[提示] nmcli 列表解析没有找到有线接口，"
               "正在使用兼容检测方式...\n");

        if (ifs)
        {
            for (struct if_nameindex *p = ifs;
                 p->if_index != 0 && p->if_name != NULL && count < 32;
                 ++p)
            {

                char type[64] = {0};
                char state[96] = {0};

                if (strcmp(p->if_name, "lo") == 0)
                {
                    continue;
                }

                {
                    const char *type_argv[] = {
                        "nmcli", "-g", "GENERAL.TYPE",
                        "device", "show", p->if_name, NULL};

                    if (capture_cmd(type_argv,
                                    type,
                                    sizeof(type)) != 0)
                    {
                        continue;
                    }
                }

                trim_space(type);

                if (strcmp(type, "ethernet") != 0 ||
                    !interface_is_physical_ethernet(p->if_name))
                {
                    continue;
                }

                {
                    const char *state_argv[] = {
                        "nmcli", "-g", "GENERAL.STATE",
                        "device", "show", p->if_name, NULL};

                    if (capture_cmd(state_argv,
                                    state,
                                    sizeof(state)) != 0 ||
                        state[0] == '\0')
                    {
                        snprintf(state, sizeof(state), "unknown");
                    }
                }

                snprintf(devices[count],
                         sizeof(devices[count]),
                         "%s", p->if_name);

                snprintf(states[count],
                         sizeof(states[count]),
                         "%s", state);

                count++;
            }

            if_freenameindex(ifs);
        }
    }

    if (count == 0)
    {
        printf("\n没有检测到可用的有线网口。\n");
        printf("如确认盒子有网口，请先执行以下命令排查：\n");
        printf("  ip link\n");
        printf("  nmcli device status\n");
        return false;
    }

    printf("\n请选择有线网口：\n");

    for (int i = 0; i < count; ++i)
    {
        printf("  %d. %s  [网线%s] [配置%s]%s%s\n",
               i + 1,
               devices[i],
               ethernet_carrier_state(devices[i]),
               friendly_network_config_state(states[i]),
               interface_is_ssh_path(devices[i]) ? " [当前 SSH]" : "",
               interface_has_default_route(devices[i]) ? " [默认联网]" : "");
    }
    printf("  0. 取消\n");

    {
        int choice = read_int("请选择要配置的网口: ",
                              0,
                              count);
        if (choice == 0 || choice == TERMINAL_UI_INPUT_CANCELLED)
        {
            return false;
        }

        snprintf(iface,
                 iface_size,
                 "%s",
                 devices[choice - 1]);
    }

    printf("\n你选择的网口是：%s\n", iface);
    if (interface_is_ssh_path(iface))
    {
        printf("[重要提醒] SSH 正通过 %s 连接，修改 IP 或切换后当前终端可能断开。\n",
               iface);
    }
    if (interface_has_default_route(iface))
    {
        printf("[重要提醒] 设备默认通过 %s 联网，修改可能影响访问内网或互联网。\n",
               iface);
    }
    if (!interface_is_ssh_path(iface) &&
        !interface_has_default_route(iface))
    {
        printf("[提示] 这个网口不承载当前 SSH，也不负责默认联网。\n");
    }
    return true;
}

static bool choose_nmcli_interface_type(const char *required_type,
                                        char *iface, size_t iface_size)
{
    char out[CMD_OUT_SIZE];
    char devices[32][IF_NAMESIZE];
    char states[32][96];
    int count = 0;
    const char *argv[] = {
        "nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status", NULL};

    if (!iface || iface_size == 0 ||
        capture_cmd(argv, out, sizeof(out)) != 0)
    {
        return false;
    }
    {
        char *saveptr = NULL;
        char *line = strtok_r(out, "\n", &saveptr);
        while (line && count < 32)
        {
            char *type = strchr(line, ':');
            char *state = type ? strchr(type + 1, ':') : NULL;
            if (type && state)
            {
                *type++ = '\0';
                *state++ = '\0';
                if ((strcmp(type, "ethernet") == 0 || strcmp(type, "wifi") == 0) &&
                    (!required_type || strcmp(type, required_type) == 0) &&
                    line[0] != '\0')
                {
                    snprintf(devices[count], sizeof(devices[count]), "%s", line);
                    snprintf(states[count], sizeof(states[count]), "%s", state);
                    ++count;
                }
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    if (count == 0)
    {
        printf("没有检测到%s网卡。\n",
               required_type && strcmp(required_type, "wifi") == 0 ? "无线" : "可配置");
        return false;
    }
    printf("\n请选择网卡（程序不预设网卡角色）：\n");
    for (int index = 0; index < count; ++index)
    {
        printf("  %d. %-14s 状态: %s\n", index + 1,
               devices[index], friendly_device_state(states[index]));
    }
    printf("  0. 取消\n");
    {
        int choice = read_int("请选择: ", 0, count);
        if (choice == 0 || choice == TERMINAL_UI_INPUT_CANCELLED)
        {
            return false;
        }
        snprintf(iface, iface_size, "%s", devices[choice - 1]);
    }
    return true;
}

bool choose_wifi_interface(char *iface, size_t iface_size)
{
    return choose_nmcli_interface_type("wifi", iface, iface_size);
}

void ask_interface(char *iface, size_t size, const char *default_iface)
{
    for (;;)
    {
        char prompt[128];

        snprintf(prompt, sizeof(prompt),
                 "请输入网络接口名（直接回车默认 %s）: ",
                 default_iface);

        if (!read_line(prompt, iface, size))
        {
            iface[0] = '\0';
            return;
        }
        trim_space(iface);

        if (iface[0] == '\0')
        {
            snprintf(iface, size, "%s", default_iface);
        }

        if (interface_exists(iface))
        {
            return;
        }

        printf("接口 \"%s\" 不存在，请重新输入。\n", iface);
    }
}

typedef struct
{
    char device[IF_NAMESIZE];
    char type[32];
    char state[96];
    char connection[NETWORK_SUMMARY_NAME_SIZE];
    char ipv4[NETWORK_SUMMARY_VALUE_SIZE];
    EthernetCarrierState carrier;
} NetworkSummaryRow;

typedef struct
{
    char device[IF_NAMESIZE];
    char gateway[64];
} NetworkExit;

static bool connection_name_is_present(const char *name)
{
    return name && name[0] != '\0' && strcmp(name, "--") != 0;
}

static bool is_wifi_type(const char *type)
{
    return type &&
           (strcmp(type, "wifi") == 0 ||
            strcmp(type, "802-11-wireless") == 0);
}

static bool is_wired_type(const char *type)
{
    return type &&
           (strcmp(type, "ethernet") == 0 ||
            strcmp(type, "802-3-ethernet") == 0);
}

static void join_output_lines(char *text)
{
    char *source = text;
    char *destination = text;
    bool separator = false;

    while (source && *source)
    {
        if (*source == '\r' || *source == '\n')
        {
            if (destination != text)
            {
                separator = true;
            }
            ++source;
            continue;
        }
        if (separator)
        {
            *destination++ = ',';
            separator = false;
        }
        *destination++ = *source++;
    }
    *destination = '\0';
}

static void read_summary_ipv4(NetworkSummaryRow *row)
{
    const char *argv[] = {
        "nmcli", "--escape", "no", "-g", "IP4.ADDRESS",
        "device", "show", row->device, NULL};

    if (capture_cmd(argv, row->ipv4, sizeof(row->ipv4)) != 0)
    {
        row->ipv4[0] = '\0';
        return;
    }
    join_output_lines(row->ipv4);
}

static int collect_network_summary(NetworkSummaryRow *rows, int capacity)
{
    char output[NETWORK_SUMMARY_OUTPUT_SIZE];
    int count = 0;
    const char *argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "DEVICE,TYPE,STATE,CONNECTION",
        "device", "status", NULL};

    if (!rows || capacity <= 0 ||
        capture_cmd(argv, output, sizeof(output)) != 0)
    {
        return -1;
    }

    {
        char *saveptr = NULL;
        char *line = strtok_r(output, "\n", &saveptr);

        while (line && count < capacity)
        {
            char *fields[4] = {0};
            int field_count = split_nmcli_escaped_fields(line, fields, 4);

            if (field_count == 4 &&
                (is_wifi_type(fields[1]) ||
                 (is_wired_type(fields[1]) &&
                  interface_is_physical_ethernet(fields[0]))))
            {
                NetworkSummaryRow *row = &rows[count];

                memset(row, 0, sizeof(*row));
                row->carrier = ETHERNET_CARRIER_UNKNOWN;
                snprintf(row->device, sizeof(row->device), "%s", fields[0]);
                snprintf(row->type, sizeof(row->type), "%s", fields[1]);
                snprintf(row->state, sizeof(row->state), "%s", fields[2]);
                if (connection_name_is_present(fields[3]))
                {
                    snprintf(row->connection, sizeof(row->connection),
                             "%s", fields[3]);
                }
                if (is_wired_type(row->type))
                {
                    row->carrier = get_ethernet_carrier_state(row->device);
                }
                read_summary_ipv4(row);
                ++count;
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    return count;
}

static const char *summary_type_text(const NetworkSummaryRow *row)
{
    return is_wifi_type(row->type) ? "Wi-Fi" : "有线";
}

static const char *summary_status_text(const NetworkSummaryRow *row)
{
    if (is_wired_type(row->type))
    {
        if (row->carrier == ETHERNET_CARRIER_DOWN)
        {
            return "网线未接";
        }
        if (row->carrier == ETHERNET_CARRIER_UP)
        {
            return connection_name_is_present(row->connection)
                       ? "已连接"
                       : "网线已接";
        }
    }
    return friendly_device_state(row->state);
}

static void print_network_summary_rows(const NetworkSummaryRow *rows,
                                       int count)
{
    if (count < 0)
    {
        printf("无法读取网卡状态。\n");
        return;
    }
    if (count == 0)
    {
        printf("没有检测到有线或 Wi-Fi 网卡。\n");
        return;
    }

    for (int index = 0; index < count; ++index)
    {
        const NetworkSummaryRow *row = &rows[index];

        printf("%d. %s（%s） [%s]\n", index + 1, row->device,
               summary_type_text(row), summary_status_text(row));
        if (connection_name_is_present(row->connection) &&
            strcmp(row->connection, row->device) != 0)
        {
            printf("   网络：%s\n", row->connection);
        }
        printf("   IP：%s\n", row->ipv4[0] ? row->ipv4 : "未分配");
        if (interface_has_default_route(row->device) ||
            interface_is_ssh_path(row->device))
        {
            printf("   用途：%s%s%s\n",
                   interface_has_default_route(row->device)
                       ? "默认联网"
                       : "",
                   interface_has_default_route(row->device) &&
                           interface_is_ssh_path(row->device)
                       ? "、"
                       : "",
                   interface_is_ssh_path(row->device)
                       ? "当前 SSH"
                       : "");
        }
    }
}

static bool read_network_exit(NetworkExit *network_exit)
{
    char output[CMD_OUT_SIZE];
    char *saveptr = NULL;
    char *token;
    const char *argv[] = {
        "ip", "-4", "route", "get", "1.1.1.1", NULL};

    if (!network_exit ||
        capture_cmd(argv, output, sizeof(output)) != 0 || output[0] == '\0')
    {
        return false;
    }
    memset(network_exit, 0, sizeof(*network_exit));
    token = strtok_r(output, " \t\r\n", &saveptr);
    while (token)
    {
        if (strcmp(token, "dev") == 0)
        {
            token = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!token || strlen(token) >= sizeof(network_exit->device))
            {
                return false;
            }
            snprintf(network_exit->device,
                     sizeof(network_exit->device), "%s", token);
        }
        else if (strcmp(token, "via") == 0)
        {
            token = strtok_r(NULL, " \t\r\n", &saveptr);
            if (!token || strlen(token) >= sizeof(network_exit->gateway))
            {
                return false;
            }
            snprintf(network_exit->gateway,
                     sizeof(network_exit->gateway), "%s", token);
        }
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }
    return network_exit->device[0] != '\0';
}

static const NetworkSummaryRow *find_summary_row(
    const NetworkSummaryRow *rows, int count, const char *device)
{
    for (int index = 0; index < count; ++index)
    {
        if (strcmp(rows[index].device, device) == 0)
        {
            return &rows[index];
        }
    }
    return NULL;
}

static void print_network_exit(const NetworkSummaryRow *rows, int count)
{
    NetworkExit network_exit;
    const NetworkSummaryRow *row;

    printf("\n========== 默认联网方式 ==========\n");
    if (!read_network_exit(&network_exit))
    {
        printf("未设置，当前不能访问其他网段。\n");
        return;
    }

    row = find_summary_row(rows, count, network_exit.device);
    if (row && connection_name_is_present(row->connection) &&
        strcmp(row->connection, row->device) != 0)
    {
        printf("%s（%s）", row->connection, row->device);
    }
    else
    {
        printf("%s", network_exit.device);
    }
    if (network_exit.gateway[0])
    {
        printf("，路由器：%s", network_exit.gateway);
    }
    printf("\n");
    if (row && row->carrier == ETHERNET_CARRIER_DOWN)
    {
        printf("[提醒] 网线未接，此方式当前无法联网。\n");
    }
}

static const char *saved_connection_type_text(const char *type)
{
    if (is_wifi_type(type))
    {
        return "Wi-Fi";
    }
    if (is_wired_type(type))
    {
        return "有线";
    }
    return NULL;
}

static void print_saved_connections_summary(void)
{
    char output[NETWORK_SUMMARY_OUTPUT_SIZE];
    int count = 0;
    const char *argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "NAME,TYPE,AUTOCONNECT",
        "connection", "show", NULL};

    printf("\n========== 已保存连接 ==========\n");
    if (capture_cmd(argv, output, sizeof(output)) != 0)
    {
        printf("无法读取保存连接。\n");
        return;
    }

    {
        char *saveptr = NULL;
        char *line = strtok_r(output, "\n", &saveptr);

        while (line)
        {
            char *fields[3] = {0};
            int field_count = split_nmcli_escaped_fields(line, fields, 3);
            const char *type = field_count == 3
                                   ? saved_connection_type_text(fields[1])
                                   : NULL;

            if (type)
            {
                printf("%d. %s（%s，%s）\n", ++count, fields[0], type,
                       strcmp(fields[2], "yes") == 0
                           ? "自动连接"
                           : "不自动连接");
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    if (count == 0)
    {
        printf("没有已保存的有线或 Wi-Fi 连接。\n");
    }
}

void show_devices(void)
{
    NetworkSummaryRow rows[MAX_NETWORK_SUMMARY_ROWS] = {0};
    int count = collect_network_summary(rows, MAX_NETWORK_SUMMARY_ROWS);

    printf("\n========== 网卡状态 ==========\n");
    print_network_summary_rows(rows, count);
}

void show_network_state(void)
{
    NetworkSummaryRow rows[MAX_NETWORK_SUMMARY_ROWS] = {0};
    int count = collect_network_summary(rows, MAX_NETWORK_SUMMARY_ROWS);

    printf("\n========== 当前网卡 ==========\n");
    print_network_summary_rows(rows, count);
    print_network_exit(rows, count > 0 ? count : 0);
    print_saved_connections_summary();
}

typedef struct
{
    char ssid[BUF_SIZE];
    int signal;
    char security[96];
    bool active;
} NearbyWifi;

static const char *nearby_wifi_signal_text(int signal)
{
    if (signal >= 75)
    {
        return "很好";
    }
    if (signal >= 55)
    {
        return "良好";
    }
    if (signal >= 35)
    {
        return "一般";
    }
    return "较弱";
}

static const char *nearby_wifi_security_text(const char *security)
{
    if (!security || security[0] == '\0' || strcmp(security, "--") == 0)
    {
        return "无密码";
    }
    if (strstr(security, "WPA3") || strstr(security, "SAE"))
    {
        return "WPA3";
    }
    if (strstr(security, "WPA"))
    {
        return "有密码";
    }
    if (strstr(security, "WEP"))
    {
        return "旧式密码";
    }
    return "需认证";
}

static int nearby_wifi_index(const NearbyWifi *networks,
                             int count,
                             const char *ssid)
{
    for (int index = 0; index < count; ++index)
    {
        if (strcmp(networks[index].ssid, ssid) == 0)
        {
            return index;
        }
    }
    return -1;
}

static int merge_nearby_wifi_output(char *output,
                                    NearbyWifi *networks,
                                    int count,
                                    int capacity)
{
    char *saveptr = NULL;
    char *line;

    if (!output || !networks || count < 0 || capacity <= 0)
    {
        return count;
    }

    line = strtok_r(output, "\n", &saveptr);
    while (line)
    {
        char *fields[4] = {0};
        int field_count = split_nmcli_escaped_fields(line, fields, 4);

        if (field_count == 4 && fields[1][0] != '\0')
        {
            int existing = nearby_wifi_index(networks, count, fields[1]);
            int signal = atoi(fields[2]);
            bool active = strcmp(fields[0], "*") == 0 ||
                          strcmp(fields[0], "yes") == 0;
            NearbyWifi *network;

            if (signal < 0)
            {
                signal = 0;
            }
            if (signal > 100)
            {
                signal = 100;
            }
            if (existing >= 0)
            {
                network = &networks[existing];
                network->active = network->active || active;
                if (signal <= network->signal)
                {
                    line = strtok_r(NULL, "\n", &saveptr);
                    continue;
                }
            }
            else
            {
                if (count >= capacity)
                {
                    break;
                }
                network = &networks[count++];
                memset(network, 0, sizeof(*network));
            }
            snprintf(network->ssid, sizeof(network->ssid), "%s", fields[1]);
            network->signal = signal;
            snprintf(network->security, sizeof(network->security),
                     "%s", fields[3]);
            network->active = network->active || active;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

static int collect_nearby_wifi(const char *ifname,
                               NearbyWifi *networks,
                               int capacity)
{
    char output[NETWORK_SUMMARY_OUTPUT_SIZE];
    int count = 0;
    bool command_succeeded = false;
    const char *automatic_argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "IN-USE,SSID,SIGNAL,SECURITY",
        "device", "wifi", "list", "ifname", ifname,
        "--rescan", "auto", NULL};
    const char *scan_argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "IN-USE,SSID,SIGNAL,SECURITY",
        "device", "wifi", "list", "ifname", ifname,
        "--rescan", "yes", NULL};
    const char *cached_argv[] = {
        "nmcli", "-t", "--escape", "yes",
        "-f", "IN-USE,SSID,SIGNAL,SECURITY",
        "device", "wifi", "list", "ifname", ifname,
        "--rescan", "no", NULL};

    if (!ifname || !networks || capacity <= 0)
    {
        return -1;
    }
    /*
     * 部分厂商版 bcmdhd 驱动会把一次强制扫描报告为成功，却只返回当前
     * 已连接的 SSID。先保留 NetworkManager 已有缓存，再使用自动扫描；
     * 只有合并后仍不超过两个 SSID 时才强制重试，避免一份稀疏结果覆盖
     * 此前更完整的热点列表。
     */
    terminal_ui_set_step("扫描附近 Wi-Fi",
                         "正在请求无线网卡更新热点列表");
    terminal_ui_show_busy("正在扫描附近 Wi-Fi...",
                          "无线网卡正在更新热点列表，通常需要 3–6 秒。");

    if (capture_cmd(cached_argv, output, sizeof(output)) == 0)
    {
        command_succeeded = true;
        count = merge_nearby_wifi_output(output, networks, count, capacity);
    }
    if (capture_cmd(automatic_argv, output, sizeof(output)) == 0)
    {
        command_succeeded = true;
        count = merge_nearby_wifi_output(output, networks, count, capacity);
    }
    for (int attempt = 0; count <= 2 && attempt < 2; ++attempt)
    {
        if (capture_cmd(scan_argv, output, sizeof(output)) == 0)
        {
            command_succeeded = true;
            count = merge_nearby_wifi_output(output, networks, count,
                                             capacity);
        }
    }
    return command_succeeded ? count : -1;
}

static void print_nearby_wifi(const NearbyWifi *networks, int count)
{
    if (count < 0)
    {
        printf("[失败] 无法读取附近 Wi-Fi。\n");
        return;
    }
    if (count == 0)
    {
        printf("没有发现附近 Wi-Fi。\n");
        return;
    }
    for (int index = 0; index < count; ++index)
    {
        printf("%d. %s%s  · 信号 %d%%（%s） · %s\n",
               index + 1,
               networks[index].ssid,
               networks[index].active ? " [已连接]" : "",
               networks[index].signal,
               nearby_wifi_signal_text(networks[index].signal),
               nearby_wifi_security_text(networks[index].security));
    }
}

NearbyWifiChoiceResult choose_nearby_wifi_ssid(const char *ifname,
                                               char *ssid,
                                               size_t ssid_size)
{
    NearbyWifi networks[MAX_NETWORK_SUMMARY_ROWS] = {0};
    int count = collect_nearby_wifi(ifname, networks,
                                    MAX_NETWORK_SUMMARY_ROWS);

    if (!ssid || ssid_size == 0)
    {
        return NEARBY_WIFI_CHOICE_CANCELLED;
    }
    ssid[0] = '\0';
    terminal_ui_set_step("选择目标 Wi-Fi",
                         "扫描已完成；选择网络或使用手工输入");
    printf("\n========== 附近 Wi-Fi ==========\n");
    print_nearby_wifi(networks, count);
    printf("0. 手工输入或隐藏网络\n");
    {
        int available_count = count > 0 ? count : 0;
        int choice;

        /* 8 个 Wi-Fi 槽位 + 1 个手工输入操作，保持列表高度稳定。 */
        terminal_ui_prepare_choice_rows(9);
        choice = read_int("请选择 Wi-Fi: ", 0, available_count);

        if (choice == TERMINAL_UI_INPUT_CANCELLED)
        {
            return NEARBY_WIFI_CHOICE_CANCELLED;
        }
        if (choice == 0)
        {
            return NEARBY_WIFI_CHOICE_MANUAL;
        }
        snprintf(ssid, ssid_size, "%s", networks[choice - 1].ssid);
        return NEARBY_WIFI_CHOICE_SELECTED;
    }
}

void scan_wifi(void)
{
    char iface[IF_NAMESIZE];
    NearbyWifi networks[MAX_NETWORK_SUMMARY_ROWS] = {0};
    int count;

    terminal_ui_set_step("选择扫描网卡",
                         "选择用于搜索附近 Wi-Fi 的无线网卡");
    terminal_ui_prepare_step(false);
    if (!choose_wifi_interface(iface, sizeof(iface)))
    {
        return;
    }

    {
        const char *on[] = {
            "nmcli", "radio", "wifi", "on", NULL};
        run_cmd(on);
    }

    count = collect_nearby_wifi(iface, networks, MAX_NETWORK_SUMMARY_ROWS);
    printf("\n========== 附近 Wi-Fi ==========\n");
    print_nearby_wifi(networks, count);
}

bool nmcli_available(void)
{
    const char *argv[] = {
        "nmcli", "--version", NULL};

    return run_cmd_silent(argv) == 0;
}
