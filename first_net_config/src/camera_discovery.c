#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "camera_discovery.h"
#include "cli_io.h"
#include "ipv4_utils.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif

#define SADP_PORT 37020
#define ONVIF_PORT 3702
#define DISCOVERY_GROUP "239.255.255.250"

static const char *SADP_PROBE =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<Probe><Uuid>FNC-CAMERA-DISCOVERY</Uuid><Types>inquiry</Types></Probe>";

static const char *ONVIF_PROBE =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
    "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
    "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
    "<e:Header><w:MessageID>uuid:FNC-CAMERA-DISCOVERY</w:MessageID>"
    "<w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
    "<w:Action e:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
    "</e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types>"
    "</d:Probe></e:Body></e:Envelope>";

static bool tag_name_matches(const char *start, const char *end,
                             const char *wanted)
{
    const char *local = start;
    const char *cursor;
    size_t length;

    for (cursor = start; cursor < end; ++cursor)
    {
        if (*cursor == ':')
        {
            local = cursor + 1;
        }
        if (isspace((unsigned char)*cursor) || *cursor == '/' || *cursor == '>')
        {
            end = cursor;
            break;
        }
    }
    length = (size_t)(end - local);
    return strlen(wanted) == length && strncasecmp(local, wanted, length) == 0;
}

static bool xml_has_element(const char *xml, const char *wanted)
{
    const char *cursor = xml;

    if (!xml || !wanted)
    {
        return false;
    }
    while ((cursor = strchr(cursor, '<')) != NULL)
    {
        const char *name = cursor + 1;
        const char *tag_end = strchr(name, '>');

        if (!tag_end)
        {
            return false;
        }
        if (*name != '/' && *name != '?' && *name != '!' &&
            tag_name_matches(name, tag_end, wanted))
        {
            return true;
        }
        cursor = tag_end + 1;
    }
    return false;
}

static bool xml_value(const char *xml, const char *wanted,
                      char *out, size_t out_size)
{
    const char *cursor = xml;

    if (!xml || !wanted || !out || out_size == 0)
    {
        return false;
    }
    out[0] = '\0';
    while ((cursor = strchr(cursor, '<')) != NULL)
    {
        const char *name = cursor + 1;
        const char *tag_end = strchr(name, '>');
        const char *value_end;
        size_t length;

        if (!tag_end)
        {
            return false;
        }
        if (*name == '/' || *name == '?' || *name == '!' ||
            !tag_name_matches(name, tag_end, wanted))
        {
            cursor = tag_end + 1;
            continue;
        }
        value_end = strchr(tag_end + 1, '<');
        if (!value_end)
        {
            return false;
        }
        length = (size_t)(value_end - (tag_end + 1));
        while (length > 0 && isspace((unsigned char)tag_end[1]))
        {
            ++tag_end;
            --length;
        }
        while (length > 0 && isspace((unsigned char)tag_end[length]))
        {
            --length;
        }
        if (length >= out_size)
        {
            length = out_size - 1;
        }
        memcpy(out, tag_end + 1, length);
        out[length] = '\0';
        return length > 0;
    }
    return false;
}

static void first_xml_value(const char *xml, const char *const names[],
                            char *out, size_t out_size)
{
    out[0] = '\0';
    for (int index = 0; names[index]; ++index)
    {
        if (xml_value(xml, names[index], out, out_size))
        {
            return;
        }
    }
}

static void ip_from_xaddrs(const char *xaddrs, char *ip, size_t ip_size,
                           int *port)
{
    const char *scheme;
    const char *host;
    const char *end;
    char candidate[64];

    if (!xaddrs || !(scheme = strstr(xaddrs, "://")))
    {
        return;
    }
    host = scheme + 3;
    end = host;
    while (*end && *end != ':' && *end != '/' && !isspace((unsigned char)*end))
    {
        ++end;
    }
    if ((size_t)(end - host) >= sizeof(candidate))
    {
        return;
    }
    memcpy(candidate, host, (size_t)(end - host));
    candidate[end - host] = '\0';
    if (valid_ipv4(candidate))
    {
        size_t length = strlen(candidate);
        if (length < ip_size)
        {
            memcpy(ip, candidate, length + 1);
        }
    }
    if (*end == ':')
    {
        long parsed = strtol(end + 1, NULL, 10);
        if (parsed >= 1 && parsed <= 65535)
        {
            *port = (int)parsed;
        }
    }
}

static int parsed_port(const char *text, int fallback)
{
    char *end = NULL;
    long value;

    if (!text || !*text)
    {
        return fallback;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' &&
                   value >= 1 && value <= 65535
               ? (int)value
               : fallback;
}

int parse_camera_discovery_payload(const char *payload, const char *sender_ip,
                                   DiscoveredCamera *camera)
{
    static const char *const ip_names[] = {
        "IPv4Address", "IPAddress", "IPv4", NULL};
    static const char *const mask_names[] = {
        "IPv4SubnetMask", "SubnetMask", NULL};
    static const char *const gateway_names[] = {
        "IPv4Gateway", "Gateway", "DefaultGateway", NULL};
    static const char *const model_names[] = {
        "DeviceDescription", "DeviceModel", "Model", "DeviceType", NULL};
    static const char *const serial_names[] = {
        "DeviceSN", "SerialNumber", NULL};
    static const char *const mac_names[] = {"MAC", "MACAddress", NULL};
    char xaddrs[512] = {0};
    char port_text[32] = {0};

    if (!payload || !camera)
    {
        return 0;
    }

    /*
     * 本机发送的 SADP/ONVIF 探测请求也可能经组播或广播回到 socket。
     * 请求只有 Probe，真正的发现响应使用 ProbeMatch。不能把请求报文的
     * 本机源 IP 当成摄像头地址。
     */
    if (xml_has_element(payload, "Probe") &&
        !xml_has_element(payload, "ProbeMatch"))
    {
        return 0;
    }

    memset(camera, 0, sizeof(*camera));
    camera->http_port = 80;
    camera->rtsp_port = 554;
    camera->prefix = -1;
    first_xml_value(payload, ip_names, camera->ip, sizeof(camera->ip));
    (void)xml_value(payload, "XAddrs", xaddrs, sizeof(xaddrs));
    if (camera->ip[0] == '\0' && xaddrs[0] != '\0')
    {
        ip_from_xaddrs(xaddrs, camera->ip, sizeof(camera->ip),
                       &camera->http_port);
    }
    if (camera->ip[0] == '\0' && valid_ipv4(sender_ip))
    {
        snprintf(camera->ip, sizeof(camera->ip), "%s", sender_ip);
    }
    if (!valid_ipv4(camera->ip))
    {
        return 0;
    }

    first_xml_value(payload, mask_names, camera->subnet_mask,
                    sizeof(camera->subnet_mask));
    first_xml_value(payload, gateway_names, camera->gateway,
                    sizeof(camera->gateway));
    first_xml_value(payload, model_names, camera->model,
                    sizeof(camera->model));
    first_xml_value(payload, serial_names, camera->serial,
                    sizeof(camera->serial));
    first_xml_value(payload, mac_names, camera->mac, sizeof(camera->mac));
    if (camera->subnet_mask[0])
    {
        camera->prefix = netmask_to_prefix(camera->subnet_mask);
    }
    if (xml_value(payload, "HttpPort", port_text, sizeof(port_text)))
    {
        camera->http_port = parsed_port(port_text, 80);
    }
    if (xml_value(payload, "RtspPort", port_text, sizeof(port_text)))
    {
        camera->rtsp_port = parsed_port(port_text, 554);
    }
    return 1;
}

static void merge_camera(DiscoveredCamera *target,
                         const DiscoveredCamera *incoming)
{
#define COPY_IF_EMPTY(field)                                                   \
    do                                                                         \
    {                                                                          \
        if (target->field[0] == '\0' && incoming->field[0] != '\0')            \
            snprintf(target->field, sizeof(target->field), "%s",              \
                     incoming->field);                                         \
    } while (0)
    COPY_IF_EMPTY(subnet_mask);
    COPY_IF_EMPTY(gateway);
    COPY_IF_EMPTY(mac);
    COPY_IF_EMPTY(model);
    COPY_IF_EMPTY(serial);
#undef COPY_IF_EMPTY
    if (target->prefix < 0 && incoming->prefix >= 0)
    {
        target->prefix = incoming->prefix;
    }
}

static bool ipv4_is_local_address(const char *ip)
{
    struct ifaddrs *interfaces = NULL;
    bool found = false;

    if (!valid_ipv4(ip) || getifaddrs(&interfaces) != 0)
    {
        return false;
    }
    for (struct ifaddrs *item = interfaces;
         item != NULL; item = item->ifa_next)
    {
        char address[INET_ADDRSTRLEN];

        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !inet_ntop(AF_INET,
                       &((struct sockaddr_in *)item->ifa_addr)->sin_addr,
                       address, (socklen_t)sizeof(address)))
        {
            continue;
        }
        if (strcmp(address, ip) == 0)
        {
            found = true;
            break;
        }
    }
    freeifaddrs(interfaces);
    return found;
}

static int send_probe(int socket_fd, const char *payload,
                      const char *address, int port)
{
    struct sockaddr_in destination = {0};

    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, address, &destination.sin_addr) != 1)
    {
        return -1;
    }
    return sendto(socket_fd, payload, strlen(payload), 0,
                  (struct sockaddr *)&destination, sizeof(destination)) >= 0
               ? 0
               : -1;
}

int discover_cameras(const char *iface,
                     DiscoveredCamera *cameras,
                     size_t capacity,
                     int timeout_seconds)
{
    int socket_fd;
    int enabled = 1;
    unsigned int ifindex;
    struct sockaddr_in local = {0};
    struct ip_mreqn multicast = {0};
    int sent = 0;
    int found = 0;
    time_t deadline;

    if (!iface || !cameras || capacity == 0 ||
        (ifindex = if_nametoindex(iface)) == 0)
    {
        return -1;
    }
    memset(cameras, 0, capacity * sizeof(*cameras));
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        return -1;
    }
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                     &enabled, sizeof(enabled));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST,
                     &enabled, sizeof(enabled));
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE,
                   iface, (socklen_t)(strlen(iface) + 1)) != 0)
    {
        close(socket_fd);
        return -1;
    }
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(SADP_PORT);
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) != 0)
    {
        local.sin_port = 0;
        if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) != 0)
        {
            close(socket_fd);
            return -1;
        }
    }
    multicast.imr_ifindex = (int)ifindex;
    (void)inet_pton(AF_INET, DISCOVERY_GROUP, &multicast.imr_multiaddr);
    (void)setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF,
                     &multicast, sizeof(multicast));
    (void)setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &multicast, sizeof(multicast));

    sent += send_probe(socket_fd, SADP_PROBE, DISCOVERY_GROUP, SADP_PORT) == 0;
    sent += send_probe(socket_fd, SADP_PROBE, "255.255.255.255", SADP_PORT) == 0;
    sent += send_probe(socket_fd, ONVIF_PROBE, DISCOVERY_GROUP, ONVIF_PORT) == 0;
    if (sent == 0)
    {
        close(socket_fd);
        return -1;
    }

    timeout_seconds = timeout_seconds < 1 ? 1 :
                      timeout_seconds > 15 ? 15 : timeout_seconds;
    deadline = time(NULL) + timeout_seconds;
    while (time(NULL) < deadline)
    {
        fd_set read_set;
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        char payload[65536];
        struct sockaddr_in sender = {0};
        socklen_t sender_size = sizeof(sender);
        ssize_t size;
        char sender_ip[INET_ADDRSTRLEN] = {0};
        DiscoveredCamera parsed;
        int existing = -1;

        FD_ZERO(&read_set);
        FD_SET(socket_fd, &read_set);
        if (select(socket_fd + 1, &read_set, NULL, NULL, &timeout) <= 0)
        {
            continue;
        }
        size = recvfrom(socket_fd, payload, sizeof(payload) - 1, 0,
                        (struct sockaddr *)&sender, &sender_size);
        if (size <= 0)
        {
            continue;
        }
        payload[size] = '\0';
        (void)inet_ntop(AF_INET, &sender.sin_addr, sender_ip,
                        (socklen_t)sizeof(sender_ip));
        if (ipv4_is_local_address(sender_ip))
        {
            continue;
        }
        if (!parse_camera_discovery_payload(payload, sender_ip, &parsed))
        {
            continue;
        }
        for (int index = 0; index < found; ++index)
        {
            if (strcmp(cameras[index].ip, parsed.ip) == 0 ||
                (cameras[index].mac[0] && parsed.mac[0] &&
                 strcasecmp(cameras[index].mac, parsed.mac) == 0))
            {
                existing = index;
                break;
            }
        }
        if (existing >= 0)
        {
            merge_camera(&cameras[existing], &parsed);
        }
        else if ((size_t)found < capacity)
        {
            cameras[found++] = parsed;
        }
    }
    close(socket_fd);
    return found;
}

int choose_or_enter_camera(const char *iface, DiscoveredCamera *camera)
{
    DiscoveredCamera cameras[MAX_DISCOVERED_CAMERAS];
    int count;

    printf("\n正在通过 %s 搜索海康 SADP / ONVIF 摄像头...\n", iface);
    count = discover_cameras(iface, cameras, MAX_DISCOVERED_CAMERAS, 4);
    if (count < 0)
    {
        printf("[提醒] 摄像头搜索失败，请检查 Link、root 权限或端口占用。\n");
        count = 0;
    }
    if (count > 0)
    {
        printf("\n发现摄像头：\n");
        for (int index = 0; index < count; ++index)
        {
            printf("  %d. IP:%-15s 掩码:%-15s MAC:%-20s 型号:%s\n",
                   index + 1, cameras[index].ip,
                   cameras[index].subnet_mask[0] ? cameras[index].subnet_mask : "未广播",
                   cameras[index].mac[0] ? cameras[index].mac : "未广播",
                   cameras[index].model[0] ? cameras[index].model : "未广播");
        }
        printf("  %d. 手动填写\n", count + 1);
        {
            int choice = read_int("请选择摄像头: ", 1, count + 1);
            if (choice <= count)
            {
                *camera = cameras[choice - 1];
                return 1;
            }
        }
    }
    else
    {
        printf("没有发现摄像头。部分固件或交换机可能不转发发现报文，可手动填写。\n");
    }

    memset(camera, 0, sizeof(*camera));
    camera->prefix = -1;
    camera->http_port = 80;
    camera->rtsp_port = 554;
    read_ipv4_required("摄像头 IPv4 地址: ", camera->ip, sizeof(camera->ip));
    return 1;
}
