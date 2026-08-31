#define _POSIX_C_SOURCE 200809L

#include "interface_inspector.h"
#include "cli_io.h"
#include "ipv4_utils.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_IPV4_ROWS 128

typedef struct
{
    char iface[IF_NAMESIZE];
    char ip[INET_ADDRSTRLEN];
    int prefix;
    uint32_t first;
    uint32_t last;
} IPv4Row;

static int prefix_from_sockaddr(const struct sockaddr *mask)
{
    const struct sockaddr_in *value = (const struct sockaddr_in *)mask;
    uint32_t bits;
    int prefix = 0;
    bool zero_seen = false;

    if (!mask || mask->sa_family != AF_INET)
    {
        return -1;
    }
    bits = ntohl(value->sin_addr.s_addr);
    for (int index = 31; index >= 0; --index)
    {
        bool bit = ((bits >> index) & 1U) != 0;
        if (bit && zero_seen)
        {
            return -1;
        }
        if (bit)
        {
            ++prefix;
        }
        else
        {
            zero_seen = true;
        }
    }
    return prefix;
}

static bool ipv4_range(const char *ip, int prefix,
                       uint32_t *first, uint32_t *last)
{
    struct in_addr address;
    uint32_t host;
    uint32_t mask;

    if (!ip || prefix < 1 || prefix > 32 ||
        inet_pton(AF_INET, ip, &address) != 1)
    {
        return false;
    }
    host = ntohl(address.s_addr);
    mask = prefix == 32 ? UINT32_MAX : UINT32_MAX << (32 - prefix);
    *first = host & mask;
    *last = *first | ~mask;
    return true;
}

static int collect_ipv4_rows(IPv4Row *rows, int capacity)
{
    struct ifaddrs *interfaces = NULL;
    int count = 0;

    if (!rows || capacity <= 0 || getifaddrs(&interfaces) != 0)
    {
        return 0;
    }
    for (struct ifaddrs *item = interfaces;
         item && count < capacity; item = item->ifa_next)
    {
        char address[INET_ADDRSTRLEN];
        int prefix;

        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            strcmp(item->ifa_name, "lo") == 0)
        {
            continue;
        }
        if (!inet_ntop(AF_INET,
                       &((struct sockaddr_in *)item->ifa_addr)->sin_addr,
                       address, (socklen_t)sizeof(address)))
        {
            continue;
        }
        prefix = prefix_from_sockaddr(item->ifa_netmask);
        if (prefix < 1 ||
            !ipv4_range(address, prefix, &rows[count].first,
                        &rows[count].last))
        {
            continue;
        }
        snprintf(rows[count].iface, sizeof(rows[count].iface), "%s",
                 item->ifa_name);
        snprintf(rows[count].ip, sizeof(rows[count].ip), "%s", address);
        rows[count].prefix = prefix;
        ++count;
    }
    freeifaddrs(interfaces);
    return count;
}

static bool ranges_overlap(const IPv4Row *left, const IPv4Row *right)
{
    return left->first <= right->last && right->first <= left->last;
}

bool ipv4_networks_overlap(const char *left_ip, int left_prefix,
                           const char *right_ip, int right_prefix)
{
    IPv4Row left = {0};
    IPv4Row right = {0};

    return ipv4_range(left_ip, left_prefix, &left.first, &left.last) &&
           ipv4_range(right_ip, right_prefix, &right.first, &right.last) &&
           ranges_overlap(&left, &right);
}

static void format_network(const IPv4Row *row, char *out, size_t size)
{
    struct in_addr network = {.s_addr = htonl(row->first)};
    char address[INET_ADDRSTRLEN] = {0};

    (void)inet_ntop(AF_INET, &network, address, (socklen_t)sizeof(address));
    snprintf(out, size, "%s/%d", address, row->prefix);
}

static bool sys_path_exists(const char *iface, const char *leaf)
{
    char path[256];
    struct stat info;

    if (snprintf(path, sizeof(path), "/sys/class/net/%s/%s", iface, leaf) < 0)
    {
        return false;
    }
    return stat(path, &info) == 0;
}

static bool interface_has_uevent_value(const char *iface,
                                       const char *expected)
{
    char path[256];
    char line[128];
    FILE *handle;

    if (!iface || !expected ||
        snprintf(path, sizeof(path), "/sys/class/net/%s/uevent", iface) < 0)
    {
        return false;
    }
    handle = fopen(path, "r");
    if (!handle)
    {
        return false;
    }
    while (fgets(line, sizeof(line), handle))
    {
        trim_space(line);
        if (strcmp(line, expected) == 0)
        {
            fclose(handle);
            return true;
        }
    }
    fclose(handle);
    return false;
}

static bool interface_uses_ethernet_frames(const char *iface)
{
    char path[256];
    FILE *handle;
    int type = 0;

    if (!iface ||
        snprintf(path, sizeof(path), "/sys/class/net/%s/type", iface) < 0)
    {
        return false;
    }
    handle = fopen(path, "r");
    if (!handle)
    {
        return false;
    }
    if (fscanf(handle, "%d", &type) != 1)
    {
        fclose(handle);
        return false;
    }
    fclose(handle);
    return type == 1;
}

bool interface_is_physical_ethernet(const char *iface)
{
    char class_path[256];
    char target[512];
    ssize_t target_size;

    if (!iface || iface[0] == '\0' || strcmp(iface, "lo") == 0 ||
        !interface_uses_ethernet_frames(iface) ||
        sys_path_exists(iface, "wireless") ||
        interface_has_uevent_value(iface, "DEVTYPE=gadget") ||
        snprintf(class_path, sizeof(class_path),
                 "/sys/class/net/%s", iface) < 0)
    {
        return false;
    }
    target_size = readlink(class_path, target, sizeof(target) - 1);
    if (target_size <= 0)
    {
        return false;
    }
    target[target_size] = '\0';

    /*
     * dummy、bridge、veth、VLAN 等纯软件接口都位于 virtual/net。
     * 板载网口及插入 USB 主机口的有线网卡会有真实设备路径。
     */
    if (strstr(target, "/virtual/net/") != NULL)
    {
        return false;
    }
    return sys_path_exists(iface, "device");
}

static const char *interface_type_text(const char *iface)
{
    char class_path[256];
    char target[512];
    ssize_t target_size;

    if (sys_path_exists(iface, "wireless") ||
        interface_has_uevent_value(iface, "DEVTYPE=wlan"))
    {
        return "Wi-Fi";
    }
    if (interface_is_physical_ethernet(iface))
    {
        return "有线";
    }
    if (interface_has_uevent_value(iface, "DEVTYPE=gadget"))
    {
        return "USB调试";
    }
    target_size = snprintf(class_path, sizeof(class_path),
                           "/sys/class/net/%s", iface) >= 0
                      ? readlink(class_path, target, sizeof(target) - 1)
                      : -1;
    if (target_size > 0)
    {
        target[target_size] = '\0';
        if (strstr(target, "/virtual/net/") != NULL)
        {
            return "虚拟";
        }
    }
    return "其他";
}

bool interface_has_default_route(const char *iface)
{
    FILE *handle = fopen("/proc/net/route", "r");
    char line[512];

    if (!handle)
    {
        return false;
    }
    (void)fgets(line, sizeof(line), handle);
    while (fgets(line, sizeof(line), handle))
    {
        char device[IF_NAMESIZE];
        unsigned long destination;
        unsigned long gateway;
        unsigned long flags;

        if (sscanf(line, "%15s %lx %lx %lx", device,
                   &destination, &gateway, &flags) == 4 &&
            strcmp(device, iface) == 0 && destination == 0 &&
            (flags & 0x1UL) != 0)
        {
            fclose(handle);
            return true;
        }
    }
    fclose(handle);
    return false;
}

static void ssh_server_ip(char *out, size_t size)
{
    const char *connection = getenv("SSH_CONNECTION");
    char client[INET_ADDRSTRLEN] = {0};
    char client_port[16] = {0};
    char server[INET_ADDRSTRLEN] = {0};

    out[0] = '\0';
    if (connection && sscanf(connection, "%15s %15s %15s",
                             client, client_port, server) == 3)
    {
        snprintf(out, size, "%s", server);
    }
}

bool interface_is_ssh_path(const char *iface)
{
    IPv4Row rows[MAX_IPV4_ROWS];
    char server[INET_ADDRSTRLEN];
    int count;

    ssh_server_ip(server, sizeof(server));
    if (server[0] == '\0')
    {
        return false;
    }
    count = collect_ipv4_rows(rows, MAX_IPV4_ROWS);
    for (int index = 0; index < count; ++index)
    {
        if (strcmp(rows[index].iface, iface) == 0 &&
            strcmp(rows[index].ip, server) == 0)
        {
            return true;
        }
    }
    return false;
}

void show_interface_overview(void)
{
    struct if_nameindex *names = if_nameindex();
    IPv4Row rows[MAX_IPV4_ROWS];
    int count = collect_ipv4_rows(rows, MAX_IPV4_ROWS);

    printf("\n================ 网卡总览 ================\n");
    if (!names)
    {
        printf("无法枚举网卡。\n");
        return;
    }
    for (struct if_nameindex *entry = names;
         entry->if_index != 0 && entry->if_name; ++entry)
    {
        char mac_path[256];
        char mac[64] = "-";
        char carrier_path[256];
        char carrier[16] = "未知";
        FILE *handle;

        if (strcmp(entry->if_name, "lo") == 0)
        {
            continue;
        }
        snprintf(mac_path, sizeof(mac_path), "/sys/class/net/%s/address",
                 entry->if_name);
        handle = fopen(mac_path, "r");
        if (handle)
        {
            if (fgets(mac, sizeof(mac), handle))
            {
                trim_space(mac);
            }
            fclose(handle);
        }
        snprintf(carrier_path, sizeof(carrier_path),
                 "/sys/class/net/%s/carrier", entry->if_name);
        handle = fopen(carrier_path, "r");
        if (handle)
        {
            char value[8] = {0};
            if (fgets(value, sizeof(value), handle))
            {
                snprintf(carrier, sizeof(carrier), "%s",
                         value[0] == '1' ? "已连接" : "未连接");
            }
            fclose(handle);
        }

        printf("%-10s 类型:%-6s 连接:%-8s MAC:%s%s%s\n",
               entry->if_name,
               interface_type_text(entry->if_name),
               carrier, mac,
               interface_has_default_route(entry->if_name) ? " [当前网络出口]" : "",
               interface_is_ssh_path(entry->if_name) ? " [当前远程连接]" : "");
        for (int index = 0; index < count; ++index)
        {
            if (strcmp(rows[index].iface, entry->if_name) == 0)
            {
                char network[32];
                format_network(&rows[index], network, sizeof(network));
                printf("           IPv4:%s/%d  网段:%s\n",
                       rows[index].ip, rows[index].prefix, network);
            }
        }
    }
    if_freenameindex(names);
    printf("==========================================\n");
}

int show_current_overlap_warnings(void)
{
    IPv4Row rows[MAX_IPV4_ROWS];
    int count = collect_ipv4_rows(rows, MAX_IPV4_ROWS);
    int warnings = 0;

    for (int left = 0; left < count; ++left)
    {
        for (int right = left + 1; right < count; ++right)
        {
            char left_network[32];
            char right_network[32];

            if (strcmp(rows[left].iface, rows[right].iface) == 0 ||
                !ranges_overlap(&rows[left], &rows[right]))
            {
                continue;
            }
            format_network(&rows[left], left_network, sizeof(left_network));
            format_network(&rows[right], right_network, sizeof(right_network));
            printf("[严重警告] 网段重叠：%s %s（%s） <-> %s %s（%s）\n",
                   rows[left].iface, rows[left].ip, left_network,
                   rows[right].iface, rows[right].ip, right_network);
            ++warnings;
        }
    }
    if (warnings == 0)
    {
        printf("[正常] 当前各网卡 IPv4 网段没有重叠。\n");
    }
    return warnings;
}

bool approve_candidate_network(const char *selected_iface,
                               const char *ip,
                               int prefix)
{
    IPv4Row rows[MAX_IPV4_ROWS];
    IPv4Row candidate = {0};
    int count;
    bool overlap = false;

    if (!selected_iface || !ipv4_range(ip, prefix,
                                       &candidate.first, &candidate.last))
    {
        printf("[失败] 无法检查候选网段。\n");
        return false;
    }
    snprintf(candidate.iface, sizeof(candidate.iface), "%s", selected_iface);
    snprintf(candidate.ip, sizeof(candidate.ip), "%s", ip);
    candidate.prefix = prefix;
    count = collect_ipv4_rows(rows, MAX_IPV4_ROWS);

    for (int index = 0; index < count; ++index)
    {
        char network[32];

        if (strcmp(rows[index].iface, selected_iface) == 0)
        {
            continue;
        }
        if (strcmp(rows[index].ip, ip) == 0)
        {
            printf("[禁止] IP %s 已被网卡 %s 使用，不能继续。\n",
                   ip, rows[index].iface);
            return false;
        }
        if (!ranges_overlap(&candidate, &rows[index]))
        {
            continue;
        }
        format_network(&rows[index], network, sizeof(network));
        printf("[严重警告] 新网段与 %s 重叠：%s/%d <-> %s\n",
               rows[index].iface, ip, prefix, network);
        overlap = true;
    }

    if (!overlap)
    {
        return true;
    }
    printf("同一或相互包含的网段出现在不同网卡上，流量可能走错出口。\n");
    return read_exact_word("确实需要继续请输入 OVERLAP；其他输入取消: ",
                           "OVERLAP");
}
