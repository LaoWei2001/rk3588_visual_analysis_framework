#define _POSIX_C_SOURCE 200809L

#include "network_state.h"
#include "cli_io.h"
#include "command_runner.h"
#include "interface_inspector.h"
#include "netconfig_types.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

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
        return "未激活";
    }
    if (strstr(state, "connecting") != NULL)
    {
        return "正在激活";
    }
    if (strstr(state, "connected") != NULL)
    {
        return "已激活";
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

static const char *ethernet_carrier_state(const char *iface)
{
    char path[256];
    char value[16] = {0};
    FILE *handle;
    int written;

    if (!iface || iface[0] == '\0')
    {
        return "未知";
    }
    written = snprintf(path, sizeof(path),
                       "/sys/class/net/%s/carrier", iface);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
        return "未知";
    }
    handle = fopen(path, "r");
    if (!handle)
    {
        return "未知";
    }
    if (!fgets(value, sizeof(value), handle))
    {
        fclose(handle);
        return "未知";
    }
    fclose(handle);

    if (value[0] == '1')
    {
        return "已接通";
    }
    if (value[0] == '0')
    {
        return "未接通";
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

    if (capture_cmd(argv, out, sizeof(out)) != 0)
    {
        return false;
    }

    return strstr(out, gateway) != NULL;
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
        printf("如果你确定盒子有网口，请先执行下面两个命令查看系统识别情况：\n");
        printf("  ip link\n");
        printf("  nmcli device status\n");
        return false;
    }

    printf("\n以下只列出可以接网线的物理网口：\n");
    printf("  物理链路：网线与交换机、摄像头或其他对端是否真正接通。\n");
    printf("  网络配置：NetworkManager 是否已经在该网口启用 IP 配置。\n");
    printf("  [当前远程连接] 表示你现在的 SSH 终端正通过这个网口。\n");
    printf("  [当前网络出口] 表示设备访问其他网段或互联网时会使用这个网口。\n");

    for (int i = 0; i < count; ++i)
    {
        printf("  %d. %-14s 物理链路: %s  网络配置: %s%s%s\n",
               i + 1,
               devices[i],
               ethernet_carrier_state(devices[i]),
               friendly_network_config_state(states[i]),
               interface_is_ssh_path(devices[i]) ? " [当前远程连接]" : "",
               interface_has_default_route(devices[i]) ? " [当前网络出口]" : "");
    }

    {
        int choice = read_int("请选择要配置的网口: ",
                              1,
                              count);

        snprintf(iface,
                 iface_size,
                 "%s",
                 devices[choice - 1]);
    }

    printf("\n你选择的网口是：%s\n", iface);
    if (interface_is_ssh_path(iface))
    {
        printf("[重要提醒] 你现在的 SSH 终端正通过 %s 连接。\n", iface);
        printf("           修改它的 IP 或切换连接后，当前终端可能断开。\n");
    }
    if (interface_has_default_route(iface))
    {
        printf("[重要提醒] 设备访问其他网段或互联网时会使用 %s。\n", iface);
        printf("           修改它可能影响设备访问内网服务器或互联网。\n");
    }
    if (!interface_is_ssh_path(iface) &&
        !interface_has_default_route(iface))
    {
        printf("[提示] 这个网口目前不承载当前 SSH，也不是当前网络出口。\n");
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
    {
        int choice = read_int("请选择: ", 1, count);
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

        read_line(prompt, iface, size);
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

void show_devices(void)
{
    const char *argv[] = {
        "nmcli", "device", "status", NULL};

    run_cmd(argv);
}

void show_network_state(void)
{
    const char *a1[] = {
        "nmcli", "device", "status", NULL};
    const char *a2[] = {
        "ip", "-4", "addr", "show", NULL};
    const char *a3[] = {
        "ip", "route", NULL};
    const char *a4[] = {
        "nmcli", "connection", "show", NULL};

    printf("\n========== 网卡状态 ==========\n");
    run_cmd(a1);

    printf("\n========== 当前 IP 地址 ==========\n");
    run_cmd(a2);

    printf("\n========== 当前网络出口 ==========\n");
    run_cmd(a3);

    printf("\n========== 已保存的网络配置 ==========\n");
    run_cmd(a4);
}

void scan_wifi(void)
{
    char iface[IF_NAMESIZE];

    if (!choose_wifi_interface(iface, sizeof(iface)))
    {
        return;
    }

    {
        const char *on[] = {
            "nmcli", "radio", "wifi", "on", NULL};
        run_cmd(on);
    }

    {
        const char *list[] = {
            "nmcli",
            "-f", "IN-USE,SSID,SIGNAL,SECURITY",
            "device", "wifi", "list",
            "ifname", iface,
            NULL};
        run_cmd(list);
    }
}

bool nmcli_available(void)
{
    const char *argv[] = {
        "nmcli", "--version", NULL};

    return run_cmd_silent(argv) == 0;
}
