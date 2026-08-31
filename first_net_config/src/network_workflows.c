#define _POSIX_C_SOURCE 200809L

#include "network_workflows.h"
#include "cli_io.h"
#include "command_runner.h"
#include "ipv4_utils.h"
#include "interface_inspector.h"
#include "camera_discovery.h"
#include "netconfig_types.h"
#include "network_safety.h"
#include "network_state.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool valid_wifi_password(const char *password)
{
    size_t length = password ? strlen(password) : 0;

    if (length >= 8 && length <= 63)
    {
        return true;
    }
    if (length == 64)
    {
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
    return false;
}

static bool recommend_camera_local_ip(const char *camera_ip, int prefix,
                                      char *out, size_t out_size)
{
    struct in_addr parsed;
    uint32_t camera;
    uint32_t mask;
    uint32_t network;
    uint32_t broadcast;
    uint32_t candidate;

    if (!camera_ip || prefix < 1 || prefix > 30 ||
        inet_pton(AF_INET, camera_ip, &parsed) != 1)
    {
        return false;
    }
    camera = ntohl(parsed.s_addr);
    mask = 0xffffffffU << (32 - prefix);
    network = camera & mask;
    broadcast = network | ~mask;
    candidate = camera > network + 1 ? camera - 1 : camera + 1;
    if (candidate <= network || candidate >= broadcast || candidate == camera)
    {
        return false;
    }
    parsed.s_addr = htonl(candidate);
    return inet_ntop(AF_INET, &parsed, out, (socklen_t)out_size) != NULL;
}

void configure_camera_network(void)
{
    char iface[IF_NAMESIZE];
    char mask[INET_ADDRSTRLEN];
    char local_ip[INET_ADDRSTRLEN];
    char recommendation[INET_ADDRSTRLEN] = {0};
    char prompt[160];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    DiscoveredCamera camera;
    IPv4Config cfg;
    ConfigLifetime lifetime;

    printf("\n============================================================\n");
    printf("                搜索摄像头并配置所选网口\n");
    printf("============================================================\n");
    show_interface_overview();
    show_current_overlap_warnings();
    if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }
    printf("本次只把 %s 用于摄像头操作；程序没有固定绑定任何网口角色。\n",
           iface);
    if (!choose_or_enter_camera(iface, &camera))
    {
        return;
    }

    if (camera.prefix >= 1 && camera.prefix <= 30)
    {
        snprintf(mask, sizeof(mask), "%s", camera.subnet_mask);
        printf("摄像头广播的网络：%s/%d（掩码 %s）\n",
               camera.ip, camera.prefix, mask);
    }
    else
    {
        for (;;)
        {
            read_line("摄像头子网掩码（例如 255.255.255.0）: ",
                      mask, sizeof(mask));
            trim_space(mask);
            camera.prefix = netmask_to_prefix(mask);
            if (camera.prefix >= 1 && camera.prefix <= 30)
            {
                break;
            }
            printf("摄像头网络需要 1 到 30 位的连续子网掩码。\n");
        }
    }
    if (!recommend_camera_local_ip(camera.ip, camera.prefix,
                                   recommendation, sizeof(recommendation)))
    {
        printf("无法自动推荐本地地址，请手动填写。\n");
    }
    for (;;)
    {
        if (recommendation[0])
        {
            snprintf(prompt, sizeof(prompt),
                     "RK3588 在 %s 上的本地 IP（回车使用 %s）: ",
                     iface, recommendation);
        }
        else
        {
            snprintf(prompt, sizeof(prompt),
                     "RK3588 在 %s 上的本地 IP: ", iface);
        }
        read_line(prompt, local_ip, sizeof(local_ip));
        trim_space(local_ip);
        if (local_ip[0] == '\0' && recommendation[0])
        {
            snprintf(local_ip, sizeof(local_ip), "%s", recommendation);
        }
        if (!valid_ipv4(local_ip) ||
            is_network_or_broadcast_address(local_ip, camera.prefix) ||
            strcmp(local_ip, camera.ip) == 0 ||
            !ipv4_same_subnet(local_ip, camera.ip, camera.prefix))
        {
            printf("本地 IP 必须与摄像头同网段、不能相同，也不能是网段/广播地址。\n");
            continue;
        }
        break;
    }
    if (!approve_candidate_network(iface, local_ip, camera.prefix))
    {
        printf("网段检查未通过，本次设置已取消。\n");
        return;
    }
    lifetime = read_config_lifetime();
    if (!build_temp_profile(iface, &temp_profile))
    {
        printf("无法生成测试连接。\n");
        return;
    }
    {
        const char *add[] = {
            "nmcli", "connection", "add", "type", "ethernet",
            "ifname", iface, "con-name", temp_profile.name,
            "autoconnect", "no",
            "save", lifetime == CONFIG_LIFETIME_TEMPORARY ? "no" : "yes",
            NULL};
        if (run_cmd(add) != 0 || !bind_created_profile_uuid(&temp_profile))
        {
            printf("创建摄像头测试连接失败。\n");
            return;
        }
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.is_static = true;
    cfg.prefix = camera.prefix;
    snprintf(cfg.ip, sizeof(cfg.ip), "%s", local_ip);
    {
        char cidr[32];
        snprintf(cidr, sizeof(cidr), "%s/%d", local_ip, camera.prefix);
        const char *configure[] = {
            "nmcli", "connection", "modify", "uuid", temp_profile.uuid,
            "ipv4.method", "manual", "ipv4.addresses", cidr,
            "ipv4.gateway", "", "ipv4.dns", "",
            "ipv4.never-default", "yes", "ipv4.ignore-auto-dns", "yes",
            "ipv4.dad-timeout", IPV4_DAD_TIMEOUT_MS,
            NULL};
        if (run_cmd(configure) != 0)
        {
            cleanup_temp_profile(&temp_profile);
            return;
        }
    }
    snprintf(final_profile, sizeof(final_profile), "摄像头-%s", iface);
    printf("\n摄像头：%s  掩码：%s  网关广播值：%s\n",
           camera.ip, mask, camera.gateway[0] ? camera.gateway : "未广播");
    printf("RK3588：%s/%d  网卡：%s\n", local_ip, camera.prefix, iface);
    printf("摄像头连接不会设置默认网关或 DNS。\n");
    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);
        if (result != NETWORK_ACTIVATION_CONFIRMED)
        {
            if (result == NETWORK_ACTIVATION_FAILED)
            {
                cleanup_temp_profile(&temp_profile);
            }
            return;
        }
    }
    printf("\n正在通过 %s 参考测试摄像头 %s（摄像头可能禁用 ICMP）...\n",
           iface, camera.ip);
    {
        const char *ping[] = {
            "ping", "-I", iface, "-c", "2", "-W", "1", camera.ip, NULL};
        (void)run_cmd(ping);
    }
}

void configure_maintenance_link(void)
{
    char iface[IF_NAMESIZE];
    char ip[INET_ADDRSTRLEN];
    char mask[INET_ADDRSTRLEN];
    char cidr[32];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    IPv4Config cfg;
    ConfigLifetime lifetime;

    printf("\n========== 配置笔记本直连维护网络 ==========\n");
    show_interface_overview();
    show_current_overlap_warnings();
    if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }
    printf("本次选择 %s；程序没有把任何网口固定为维护口。\n", iface);
    for (;;)
    {
        char input[INET_ADDRSTRLEN];
        read_line("RK3588 维护 IP（回车使用 192.168.111.111）: ",
                  input, sizeof(input));
        trim_space(input);
        snprintf(ip, sizeof(ip), "%s",
                 input[0] ? input : "192.168.111.111");
        if (valid_ipv4(ip))
        {
            break;
        }
        printf("IPv4 地址格式不正确。\n");
    }
    for (;;)
    {
        read_line("子网掩码（回车使用 255.255.255.0）: ",
                  mask, sizeof(mask));
        trim_space(mask);
        if (mask[0] == '\0')
        {
            snprintf(mask, sizeof(mask), "255.255.255.0");
        }
        cfg.prefix = netmask_to_prefix(mask);
        if (cfg.prefix >= 1 && cfg.prefix <= 30 &&
            !is_network_or_broadcast_address(ip, cfg.prefix))
        {
            break;
        }
        printf("维护网络需要 1 到 30 位的有效掩码，IP 不能是网段或广播地址。\n");
    }
    if (!approve_candidate_network(iface, ip, cfg.prefix))
    {
        printf("网段检查未通过，本次设置已取消。\n");
        return;
    }
    lifetime = read_config_lifetime();
    if (!build_temp_profile(iface, &temp_profile))
    {
        return;
    }
    {
        const char *add[] = {
            "nmcli", "connection", "add", "type", "ethernet",
            "ifname", iface, "con-name", temp_profile.name,
            "autoconnect", "no",
            "save", lifetime == CONFIG_LIFETIME_TEMPORARY ? "no" : "yes",
            NULL};
        if (run_cmd(add) != 0 || !bind_created_profile_uuid(&temp_profile))
        {
            printf("创建维护网络测试连接失败。\n");
            return;
        }
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.is_static = true;
    cfg.prefix = netmask_to_prefix(mask);
    snprintf(cfg.ip, sizeof(cfg.ip), "%s", ip);
    snprintf(cidr, sizeof(cidr), "%s/%d", ip, cfg.prefix);
    {
        const char *configure[] = {
            "nmcli", "connection", "modify", "uuid", temp_profile.uuid,
            "ipv4.method", "manual", "ipv4.addresses", cidr,
            "ipv4.gateway", "", "ipv4.dns", "",
            "ipv4.never-default", "yes", "ipv4.ignore-auto-dns", "yes",
            "ipv4.dad-timeout", IPV4_DAD_TIMEOUT_MS,
            NULL};
        if (run_cmd(configure) != 0)
        {
            cleanup_temp_profile(&temp_profile);
            return;
        }
    }
    snprintf(final_profile, sizeof(final_profile), "直连维护-%s", iface);
    printf("\n维护口不会设置网关或 DNS，也不会成为默认网络出口。\n");
    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);
        if (result == NETWORK_ACTIVATION_FAILED)
        {
            cleanup_temp_profile(&temp_profile);
            return;
        }
        if (result == NETWORK_ACTIVATION_PENDING)
        {
            printf("请使用新地址重新连接后再次运行本工具确认。\n");
            return;
        }
    }
    printf("\n笔记本网口请配置为与 %s/%d 同网段、但不重复的地址。\n",
           ip, cfg.prefix);
    printf("如果设备安装了 Web 平台，可访问：http://%s:8080\n", ip);
    printf("SSH：ssh root@%s\n", ip);
}

void configure_wired_beginner(void)
{
    char iface[IF_NAMESIZE];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    IPv4Config cfg;
    ConfigLifetime lifetime;

    printf("\n============================================================\n");
    printf("                     配置有线网络\n");
    printf("============================================================\n");
    printf("请按照现场提供的 IP 地址、子网掩码和网关填写。\n");
    printf("程序会先临时测试新网络，确认正常后才会保存，不会直接覆盖当前网络。\n");
    printf("============================================================\n");

    show_interface_overview();
    show_current_overlap_warnings();
    show_devices();

    if (!choose_ethernet_interface(iface, sizeof(iface)))
        return;

    lifetime = read_config_lifetime();

    printf("\n请选择 IP 地址的获取方式：\n");
    printf("  1. 自动获取 IP 地址\n");
    printf("  2. 手动填写固定 IP 地址\n");

    {
        int mode = read_int("请选择 [1-2]: ", 1, 2);
        snprintf(final_profile, sizeof(final_profile), "有线-%s", iface);
        if (!build_temp_profile(iface, &temp_profile))
        {
            printf("无法生成唯一的测试连接名称。\n");
            return;
        }

        {
            const char *add[] = {
                "nmcli", "connection", "add",
                "type", "ethernet",
                "ifname", iface,
                "con-name", temp_profile.name,
                "autoconnect", "no",
                "save", lifetime == CONFIG_LIFETIME_TEMPORARY ? "no" : "yes",
                NULL};
            if (run_cmd(add) != 0)
            {
                printf("创建测试用有线网络失败。\n");
                return;
            }
        }

        if (!bind_created_profile_uuid(&temp_profile))
        {
            printf("无法确认测试用有线网络的唯一标识。\n");
            return;
        }

        memset(&cfg, 0, sizeof(cfg));

        if (mode == 1)
        {
            const char *dhcp[] = {
                "nmcli", "connection", "modify", "uuid", temp_profile.uuid,
                "ipv4.method", "auto",
                "ipv4.addresses", "",
                "ipv4.gateway", "",
                "ipv4.dns", "",
                "ipv4.ignore-auto-dns", "no",
                "ipv4.dad-timeout", "-1",
                NULL};
            cfg.is_static = false;
            if (run_cmd(dhcp) != 0)
            {
                printf("自动获取 IP 的设置失败。\n");
                cleanup_temp_profile(&temp_profile);
                return;
            }
        }
        else
        {
            char mask[INET_ADDRSTRLEN];
            cfg.is_static = true;

            printf("\n请把现场提供的网络参数原样输入。\n");
            printf("依次填写 IP 地址、子网掩码和网关；没有提供的项目可以按提示留空。\n\n");

            read_ipv4_required("1) 盒子的固定 IP 地址: ",
                               cfg.ip,
                               sizeof(cfg.ip));

            for (;;)
            {
                read_line("2) 子网掩码: ", mask, sizeof(mask));
                trim_space(mask);
                cfg.prefix = netmask_to_prefix(mask);
                if (cfg.prefix >= 0 &&
                    !is_network_or_broadcast_address(cfg.ip, cfg.prefix))
                    break;
                printf("IP 地址或子网掩码不正确。子网掩码示例：255.255.255.0\n");
            }

            for (;;)
            {
                read_ipv4_optional(
                    "3) 默认网关（现场没给可回车）: ",
                    cfg.gateway,
                    sizeof(cfg.gateway));

                if (cfg.gateway[0] == '\0')
                {
                    break;
                }

                if (!ipv4_same_subnet(cfg.ip,
                                      cfg.gateway,
                                      cfg.prefix))
                {
                    printf("这个网关与盒子的 IP 看起来不属于同一个局域网，请重新核对。\n");
                    printf("请重新输入；如果现场明确要求这样设置，请让网络管理人员确认。\n");
                    continue;
                }

                break;
            }

            for (;;)
            {
                char dns_input[BUF_SIZE];

                read_line("4) DNS（不知道就直接回车，最多 4 个）: ",
                          dns_input, sizeof(dns_input));
                if (normalize_ipv4_list(dns_input, cfg.dns,
                                        sizeof(cfg.dns), 4))
                {
                    break;
                }
                printf("DNS 地址格式不正确，多个地址请用逗号分隔。\n");
            }

            if (!approve_candidate_network(iface, cfg.ip, cfg.prefix))
            {
                printf("网段检查未通过，本次设置已取消。\n");
                cleanup_temp_profile(&temp_profile);
                return;
            }

            if (!confirm_network_parameters(iface, &cfg))
            {
                printf("你没有确认这些参数，本次设置已取消。\n");
                cleanup_temp_profile(&temp_profile);
                return;
            }

            {
                char cidr[32];
                snprintf(cidr, sizeof(cidr), "%s/%d", cfg.ip, cfg.prefix);
                const char *manual[] = {
                    "nmcli", "connection", "modify", "uuid", temp_profile.uuid,
                    "ipv4.method", "manual",
                    "ipv4.addresses", cidr,
                    "ipv4.gateway", cfg.gateway,
                    "ipv4.dns", cfg.dns,
                    "ipv4.ignore-auto-dns", "yes",
                    "ipv4.dad-timeout", IPV4_DAD_TIMEOUT_MS,
                    NULL};
                if (run_cmd(manual) != 0)
                {
                    printf("固定 IP 设置失败。\n");
                    cleanup_temp_profile(&temp_profile);
                    return;
                }
            }

            printf("\n即将测试下面的网络设置：\n");
            printf("  使用网口  : %s\n", iface);
            printf("  IP 地址   : %s/%d\n", cfg.ip, cfg.prefix);
            printf("  网关       : %s\n", cfg.gateway[0] ? cfg.gateway : "未设置");
            printf("  域名服务器 : %s\n", cfg.dns[0] ? cfg.dns : "未设置");
            printf("  IP占用检查: 已开启，最长等待 %s 毫秒\n", IPV4_DAD_TIMEOUT_MS);
        }

        printf("\n保存后的网络名称：%s\n", final_profile);

        {
            NetworkActivationResult result = safe_activate_with_reconnect(
                iface, &temp_profile, &cfg, final_profile,
                lifetime == CONFIG_LIFETIME_PERMANENT
                    ? NETWORK_PROFILE_PERMANENT
                    : NETWORK_PROFILE_TEMPORARY);

            if (result == NETWORK_ACTIVATION_FAILED)
            {
                cleanup_temp_profile(&temp_profile);
                return;
            }
            if (result == NETWORK_ACTIVATION_PENDING)
            {
                printf("[待确认] 请通过新 IP 重新登录并再次运行本工具。\n");
                exit(EXIT_SUCCESS);
            }
        }

        printf("\n[完成] 有线网络设置完成（%s）。\n",
               lifetime == CONFIG_LIFETIME_PERMANENT ? "永久" : "临时");
        show_network_state();
    }
}

void configure_wired(void)
{
    char iface[IF_NAMESIZE];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    IPv4Config cfg;
    ConfigLifetime lifetime;

    printf("\n========== 有线网络详细设置 ==========\n");
    show_interface_overview();
    show_current_overlap_warnings();
    show_devices();

    if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }

    lifetime = read_config_lifetime();

    {
        char suggested_name[PROFILE_SIZE];

        snprintf(suggested_name, sizeof(suggested_name), "有线-%s", iface);
        ask_final_profile_name(suggested_name,
                               final_profile,
                               sizeof(final_profile));
    }
    if (!build_temp_profile(iface, &temp_profile))
    {
        printf("无法生成唯一的测试连接名称。\n");
        return;
    }

    {
        const char *add[] = {
            "nmcli", "connection", "add",
            "type", "ethernet",
            "ifname", iface,
            "con-name", temp_profile.name,
            "autoconnect", "no",
            "save", lifetime == CONFIG_LIFETIME_TEMPORARY ? "no" : "yes",
            NULL};

        if (run_cmd(add) != 0)
        {
            printf("创建测试用有线网络失败。\n");
            return;
        }
    }

    if (!bind_created_profile_uuid(&temp_profile))
    {
        printf("无法确认测试用有线网络的唯一标识。\n");
        return;
    }

    if (configure_ipv4_profile(temp_profile.uuid, &cfg) != 0)
    {
        printf("IPv4 配置失败。\n");
        cleanup_temp_profile(&temp_profile);
        return;
    }

    if (cfg.is_static && !approve_candidate_network(iface, cfg.ip, cfg.prefix))
    {
        printf("网段检查未通过，本次设置已取消。\n");
        cleanup_temp_profile(&temp_profile);
        return;
    }

    printf("\n测试用网络已经建立，但暂时不会在开机时自动使用。\n");
    printf("保存后的网络名称：%s\n", final_profile);

    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);

        if (result == NETWORK_ACTIVATION_FAILED)
        {
            cleanup_temp_profile(&temp_profile);
            return;
        }
        if (result == NETWORK_ACTIVATION_PENDING)
        {
            printf("[待确认] 请通过新 IP 重新登录并再次运行本工具。\n");
            exit(EXIT_SUCCESS);
        }
    }

    show_network_state();
}

void configure_wifi(void)
{
    char iface[IF_NAMESIZE];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    char ssid[BUF_SIZE];
    char password[BUF_SIZE];
    int security;
    IPv4Config cfg;
    ConfigLifetime lifetime;

    printf("\n========== 配置无线网络 ==========\n");
    show_interface_overview();
    show_current_overlap_warnings();
    show_devices();

    if (!choose_wifi_interface(iface, sizeof(iface)))
    {
        return;
    }
    lifetime = read_config_lifetime();

    {
        const char *on[] = {
            "nmcli", "radio", "wifi", "on", NULL};
        run_cmd(on);
    }

    if (read_yes_no("是否扫描附近 Wi-Fi？[Y/n]: ", true))
    {
        const char *list[] = {
            "nmcli",
            "-f", "IN-USE,SSID,SIGNAL,SECURITY",
            "device", "wifi", "list",
            "ifname", iface,
            NULL};
        run_cmd(list);
    }

    do
    {
        read_line("无线网络名称: ", ssid, sizeof(ssid));
        trim_space(ssid);

        if (ssid[0] == '\0')
        {
            printf("无线网络名称不能为空。\n");
        }
        else if (strlen(ssid) > 32)
        {
            printf("无线网络名称不能超过 32 个字节。\n");
        }
    } while (ssid[0] == '\0' || strlen(ssid) > 32);

    printf("\n无线网络的密码方式：\n");
    printf("  1. 普通密码方式，常见于家用和现场无线网络\n");
    printf("  2. WPA3 密码方式\n");
    printf("  3. 无密码网络\n");
    printf("  如果现场需要账号、证书等企业认证方式，本版本暂不支持。\n");

    security = read_int("请选择 [1-3]: ", 1, 3);

    if (security == 1 || security == 2)
    {
        do
        {
            read_password("Wi-Fi 密码（输入时不显示）: ",
                          password, sizeof(password));

            if (!valid_wifi_password(password))
            {
                printf("密码需要是 8 到 63 个字符，或 64 位十六进制内容。\n");
            }
        } while (!valid_wifi_password(password));
    }
    else
    {
        password[0] = '\0';
    }

    {
        char suggested_name[PROFILE_SIZE];

        snprintf(suggested_name, sizeof(suggested_name), "无线-%s", iface);
        ask_final_profile_name(suggested_name,
                               final_profile,
                               sizeof(final_profile));
    }
    if (!build_temp_profile(iface, &temp_profile))
    {
        printf("无法生成唯一的测试连接名称。\n");
        return;
    }

    {
        const char *add[] = {
            "nmcli", "connection", "add",
            "type", "wifi",
            "ifname", iface,
            "con-name", temp_profile.name,
            "autoconnect", "no",
            "save", lifetime == CONFIG_LIFETIME_TEMPORARY ? "no" : "yes",
            "ssid", ssid,
            NULL};

        if (run_cmd(add) != 0)
        {
            printf("创建测试用无线网络失败。\n");
            return;
        }
    }

    if (!bind_created_profile_uuid(&temp_profile))
    {
        printf("无法确认测试用无线网络的唯一标识。\n");
        return;
    }

    if (security == 1)
    {
        const char *sec[] = {
            "nmcli", "connection", "modify",
            "uuid", temp_profile.uuid,
            "wifi-sec.key-mgmt", "wpa-psk",
            "wifi-sec.psk", password,
            NULL};

        if (run_cmd(sec) != 0)
        {
            printf("设置 WPA/WPA2 密码失败。\n");
            cleanup_temp_profile(&temp_profile);
            return;
        }
    }
    else if (security == 2)
    {
        const char *sec[] = {
            "nmcli", "connection", "modify",
            "uuid", temp_profile.uuid,
            "wifi-sec.key-mgmt", "sae",
            "wifi-sec.psk", password,
            NULL};

        if (run_cmd(sec) != 0)
        {
            printf("设置 WPA3 密码失败。\n");
            cleanup_temp_profile(&temp_profile);
            return;
        }
    }

    if (configure_ipv4_profile(temp_profile.uuid, &cfg) != 0)
    {
        printf("IPv4 配置失败。\n");
        cleanup_temp_profile(&temp_profile);
        return;
    }
    if (cfg.is_static && !approve_candidate_network(iface, cfg.ip, cfg.prefix))
    {
        printf("网段检查未通过，本次设置已取消。\n");
        cleanup_temp_profile(&temp_profile);
        return;
    }

    printf("\n测试用无线网络已经建立：%s\n", temp_profile.name);
    printf("它暂时不会在开机时自动使用，因此测试失败不会影响下次启动。\n");

    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);

        if (result == NETWORK_ACTIVATION_FAILED)
        {
            cleanup_temp_profile(&temp_profile);
            return;
        }
        if (result == NETWORK_ACTIVATION_PENDING)
        {
            printf("[待确认] 请通过新 IP 重新登录并再次运行本工具。\n");
            exit(EXIT_SUCCESS);
        }
    }

    show_network_state();
}

void ping_test(void)
{
    char target[INET_ADDRSTRLEN];

    printf("\n========== 测试指定地址能否连接 ==========\n");

    for (;;)
    {
        read_line("目标 IPv4 地址: ",
                  target, sizeof(target));
        trim_space(target);

        if (valid_ipv4(target))
        {
            break;
        }

        printf("IP 地址格式不正确。\n");
    }

    {
        const char *argv[] = {
            "ping", "-c", "4", target, NULL};
        run_cmd(argv);
    }
}
