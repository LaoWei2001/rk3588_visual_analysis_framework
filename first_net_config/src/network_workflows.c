#define _POSIX_C_SOURCE 200809L

#include "network_workflows.h"
#include "cli_io.h"
#include "command_runner.h"
#include "ipv4_utils.h"
#include "netconfig_types.h"
#include "network_safety.h"
#include "network_state.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void configure_wired_beginner(void)
{
    char iface[IF_NAMESIZE];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    IPv4Config cfg;

    printf("\n============================================================\n");
    printf("                     配置有线网络\n");
    printf("============================================================\n");
    printf("请按照现场提供的 IP 地址、子网掩码和网关填写。\n");
    printf("程序会先临时测试新网络，确认正常后才会保存，不会直接覆盖当前网络。\n");
    printf("============================================================\n");

    show_devices();

    if (!choose_ethernet_interface(iface, sizeof(iface)))
        return;

    printf("\n请选择 IP 地址的获取方式：\n");
    printf("  1. 自动获取 IP 地址 IP\n");
    printf("  2. 手动填写固定 IP 地址\n");

    {
        int mode = read_int("请选择 [1-2]: ", 1, 2);
        snprintf(final_profile, sizeof(final_profile), "nettool-%s", iface);
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

            read_line("4) DNS（不知道就直接回车）: ", cfg.dns, sizeof(cfg.dns));
            trim_space(cfg.dns);

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
                iface, &temp_profile, &cfg, final_profile, true);

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

        printf("\n[完成] 有线网络设置完成。\n");
        show_network_state();
    }
}

void configure_wired(void)
{
    char iface[IF_NAMESIZE];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    IPv4Config cfg;

    printf("\n========== 有线网络详细设置 ==========\n");
    show_devices();

    if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }

    ask_final_profile_name(iface,
                           final_profile,
                           sizeof(final_profile));
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

    printf("\n测试用网络已经建立，但暂时不会在开机时自动使用。\n");
    printf("保存后的网络名称：%s\n", final_profile);

    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile, true);

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

    printf("\n========== 配置无线网络 ==========\n");
    show_devices();

    ask_interface(iface, sizeof(iface), "wlan0");

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
        read_line("SSID: ", ssid, sizeof(ssid));
        trim_space(ssid);

        if (ssid[0] == '\0')
        {
            printf("SSID 不能为空。\n");
        }
    } while (ssid[0] == '\0');

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

            if (password[0] == '\0')
            {
                printf("密码不能为空。\n");
            }
        } while (password[0] == '\0');
    }
    else
    {
        password[0] = '\0';
    }

    ask_final_profile_name(iface,
                           final_profile,
                           sizeof(final_profile));
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
            printf("设置 WPA3/SAE 密码失败。\n");
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

    printf("\n测试用无线网络已经建立：%s\n", temp_profile.name);
    printf("它暂时不会在开机时自动使用，因此测试失败不会影响下次启动。\n");

    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile, true);

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

void show_connection_detail(void)
{
    char selector[PROFILE_SIZE];
    char uuid[UUID_SIZE];

    {
        const char *list[] = {
            "nmcli", "connection", "show", NULL};
        run_cmd(list);
    }

    read_line("请输入要查看的连接名称或 UUID: ",
              selector, sizeof(selector));
    trim_space(selector);

    if (selector[0] == '\0')
    {
        return;
    }

    if (!resolve_connection_uuid(selector, uuid, sizeof(uuid)))
    {
        printf("找不到唯一连接。若存在重名，请直接输入列表中的 UUID。\n");
        return;
    }

    {
        const char *show[] = {
            "nmcli", "connection", "show", "uuid", uuid, NULL};
        run_cmd(show);
    }
}

void delete_connection(void)
{
    char selector[PROFILE_SIZE];
    char uuid[UUID_SIZE];
    char name[PROFILE_SIZE] = {0};
    char confirm[32];

    {
        const char *list[] = {
            "nmcli", "connection", "show", NULL};
        run_cmd(list);
    }

    read_line("请输入要删除的连接名称或 UUID: ",
              selector, sizeof(selector));
    trim_space(selector);

    if (selector[0] == '\0')
    {
        return;
    }

    if (!resolve_connection_uuid(selector, uuid, sizeof(uuid)))
    {
        printf("找不到唯一连接。若存在重名，请直接输入列表中的 UUID。\n");
        return;
    }

    (void)get_connection_name(uuid, name, sizeof(name));

    printf("即将删除连接 \"%s\"（UUID: %s）。\n",
           name[0] ? name : selector, uuid);
    read_line("确认删除请输入 DELETE；其他输入将取消: ",
              confirm, sizeof(confirm));

    if (strcmp(confirm, "DELETE") != 0)
    {
        printf("已取消。\n");
        return;
    }

    (void)delete_connection_by_uuid(uuid);
}

void activate_saved_connection_safely(void)
{
    char iface[IF_NAMESIZE];
    char selector[PROFILE_SIZE];
    ConnectionProfile profile = {0};
    IPv4Config cfg;

    memset(&cfg, 0, sizeof(cfg));

    printf("\n========== 测试已有网络配置 ==========\n");

    show_devices();

    ask_interface(iface, sizeof(iface), "wlan0");

    {
        const char *list[] = {
            "nmcli", "connection", "show", NULL};
        run_cmd(list);
    }

    read_line("请输入要测试的网络配置名称或 UUID: ",
              selector, sizeof(selector));
    trim_space(selector);

    if (!resolve_connection_uuid(selector,
                                 profile.uuid,
                                 sizeof(profile.uuid)))
    {
        printf("找不到唯一连接。若存在重名，请直接输入列表中的 UUID。\n");
        return;
    }

    (void)get_connection_name(profile.uuid,
                              profile.name,
                              sizeof(profile.name));

    printf("\n这个功能不会修改其中的参数，只会临时尝试使用这项网络配置。\n");
    printf("程序会检查它是否真的在 %s 上启用，以及是否获得了 IP 地址。\n",
           iface);

    {
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &profile, &cfg, "", false);

        if (result == NETWORK_ACTIVATION_FAILED)
        {
            return;
        }
        if (result == NETWORK_ACTIVATION_PENDING)
        {
            printf("[待确认] 请通过新 IP 重新登录并再次运行本工具。\n");
            exit(EXIT_SUCCESS);
        }

        printf("[完成] 这项网络配置已经通过测试。\n");
    }
}
