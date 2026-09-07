#define _POSIX_C_SOURCE 200809L

#include "operations/network_workflows.h"
#include "common/cli_io.h"
#include "common/command_runner.h"
#include "common/ipv4_utils.h"
#include "inspection/interface_inspector.h"
#include "inspection/camera_discovery.h"
#include "common/netconfig_types.h"
#include "operations/network_safety.h"
#include "inspection/network_state.h"

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
    bool camera_mask_is_manual;

    mask[0] = '\0';
    local_ip[0] = '\0';
    printf("\n============================================================\n");
    printf("                搜索摄像头并配置所选网口\n");
    printf("============================================================\n");
    show_devices();
    show_current_overlap_warnings();
camera_interface_step:
    terminal_ui_set_step("选择摄像头网口",
                         "选择用于搜索和连接摄像头的有线网口");
    terminal_ui_prepare_step(false);
    if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }
camera_choice_step:
    terminal_ui_prepare_step(true);
    if (!choose_or_enter_camera(iface, &camera))
    {
        if (terminal_ui_back_requested())
        {
            goto camera_interface_step;
        }
        return;
    }

    camera_mask_is_manual = camera.prefix < 1 || camera.prefix > 30;
camera_mask_step:
    if (!camera_mask_is_manual)
    {
        snprintf(mask, sizeof(mask), "%s", camera.subnet_mask);
        printf("摄像头网段：%s/%d（掩码 %s）\n",
               camera.ip, camera.prefix, mask);
    }
    else
    {
        terminal_ui_set_step("填写摄像头掩码",
                             "用于确定摄像头所在的局域网范围");
        for (;;)
        {
            char entered_mask[INET_ADDRSTRLEN];
            char mask_prompt[192];

            snprintf(mask_prompt, sizeof(mask_prompt),
                     mask[0]
                         ? "摄像头子网掩码（当前 %s，回车保留）: "
                         : "摄像头子网掩码（例如 255.255.255.0）: ",
                     mask);
            terminal_ui_prepare_step(true);
            if (!read_line(mask_prompt,
                           entered_mask, sizeof(entered_mask)))
            {
                if (terminal_ui_back_requested())
                {
                    goto camera_choice_step;
                }
                return;
            }
            trim_space(entered_mask);
            if (entered_mask[0])
            {
                snprintf(mask, sizeof(mask), "%s", entered_mask);
            }
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
camera_local_ip_step:
    terminal_ui_set_step("填写本机摄像头网口 IP",
                         "需与摄像头同网段且不能与摄像头重复");
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
        {
            char entered_ip[INET_ADDRSTRLEN];

            terminal_ui_prepare_step(true);
            if (!read_line(prompt, entered_ip, sizeof(entered_ip)))
            {
                if (terminal_ui_back_requested())
                {
                    if (camera_mask_is_manual)
                    {
                        goto camera_mask_step;
                    }
                    goto camera_choice_step;
                }
                return;
            }
            trim_space(entered_ip);
            if (entered_ip[0])
            {
                snprintf(local_ip, sizeof(local_ip), "%s", entered_ip);
            }
            else if (local_ip[0] == '\0' && recommendation[0])
            {
                snprintf(local_ip, sizeof(local_ip), "%s", recommendation);
            }
        }
        if (!valid_ipv4(local_ip) ||
            is_network_or_broadcast_address(local_ip, camera.prefix) ||
            strcmp(local_ip, camera.ip) == 0 ||
            !ipv4_same_subnet(local_ip, camera.ip, camera.prefix))
        {
            printf("本地 IP 需与摄像头同网段、不重复，且不能是网段/广播地址。\n");
            continue;
        }
        break;
    }
    terminal_ui_set_step("检查摄像头网段",
                         "确认新地址不会与其他网卡产生冲突");
    terminal_ui_prepare_step(true);
    if (!approve_candidate_network(iface, local_ip, camera.prefix,
                                   IPV4_ROUTE_METRIC_AUTO))
    {
        if (terminal_ui_back_requested())
        {
            goto camera_local_ip_step;
        }
        printf("网段检查未通过，本次设置已取消。\n");
        return;
    }
    terminal_ui_prepare_step(true);
    lifetime = read_config_lifetime();
    if (lifetime == CONFIG_LIFETIME_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto camera_local_ip_step;
        }
        return;
    }
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
    snprintf(final_profile, sizeof(final_profile), "Camera-%s", iface);
    printf("\n摄像头：%s  掩码：%s  网关广播值：%s\n",
           camera.ip, mask, camera.gateway[0] ? camera.gateway : "未广播");
    printf("RK3588：%s/%d  网卡：%s\n", local_ip, camera.prefix, iface);
    printf("摄像头连接不会设置默认网关或 DNS。\n");
    {
        terminal_ui_prepare_step(true);
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);
        if (result != NETWORK_ACTIVATION_CONFIRMED)
        {
            bool go_back = terminal_ui_back_requested();

            if (result == NETWORK_ACTIVATION_FAILED)
            {
                cleanup_temp_profile(&temp_profile);
            }
            if (go_back)
            {
                goto camera_local_ip_step;
            }
            return;
        }
    }
    printf("\n正在通过 %s 测试摄像头 %s（摄像头可能禁用 ICMP）...\n",
           iface, camera.ip);
    {
        const char *ping[] = {
            "ping", "-I", iface, "-c", "2", "-W", "1", camera.ip, NULL};
        printf("%s\n", run_cmd_silent(ping) == 0
                           ? "[正常] 摄像头有响应。"
                           : "[提醒] 摄像头未响应；它也可能禁用了 Ping。");
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

    ip[0] = '\0';
    mask[0] = '\0';
    printf("\n========== 配置笔记本直连维护网络 ==========\n");
    show_devices();
    show_current_overlap_warnings();
maintenance_interface_step:
    terminal_ui_set_step("选择维护网口",
                         "选择与笔记本直连的有线网口");
    terminal_ui_prepare_step(false);
    if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }
    printf("本次选择 %s；程序不固定网口角色。\n", iface);

maintenance_ip_step:
    terminal_ui_set_step("填写维护 IP",
                         "设置 RK3588 在直连维护网络中的地址");
    for (;;)
    {
        char input[INET_ADDRSTRLEN];
        char prompt[160];

        snprintf(prompt, sizeof(prompt),
                 ip[0]
                     ? "RK3588 维护 IP（当前 %s，回车保留）: "
                     : "RK3588 维护 IP（回车使用 192.168.111.111）: ",
                 ip);
        terminal_ui_prepare_step(true);
        if (!read_line(prompt, input, sizeof(input)))
        {
            if (terminal_ui_back_requested())
            {
                goto maintenance_interface_step;
            }
            return;
        }
        trim_space(input);
        if (input[0])
        {
            snprintf(ip, sizeof(ip), "%s", input);
        }
        else if (!ip[0])
        {
            snprintf(ip, sizeof(ip), "192.168.111.111");
        }
        if (valid_ipv4(ip))
        {
            break;
        }
        printf("IPv4 地址格式不正确。\n");
    }

maintenance_mask_step:
    terminal_ui_set_step("填写维护网络掩码",
                         "用于确定笔记本直连网络的地址范围");
    for (;;)
    {
        char input[INET_ADDRSTRLEN];
        char prompt[160];

        snprintf(prompt, sizeof(prompt),
                 mask[0]
                     ? "子网掩码（当前 %s，回车保留）: "
                     : "子网掩码（回车使用 255.255.255.0）: ",
                 mask);
        terminal_ui_prepare_step(true);
        if (!read_line(prompt, input, sizeof(input)))
        {
            if (terminal_ui_back_requested())
            {
                goto maintenance_ip_step;
            }
            return;
        }
        trim_space(input);
        if (input[0])
        {
            snprintf(mask, sizeof(mask), "%s", input);
        }
        else if (mask[0] == '\0')
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
    terminal_ui_set_step("检查维护网段",
                         "确认维护地址不会与其他网卡产生冲突");
    terminal_ui_prepare_step(true);
    if (!approve_candidate_network(iface, ip, cfg.prefix,
                                   IPV4_ROUTE_METRIC_AUTO))
    {
        if (terminal_ui_back_requested())
        {
            goto maintenance_mask_step;
        }
        printf("网段检查未通过，本次设置已取消。\n");
        return;
    }
    terminal_ui_prepare_step(true);
    lifetime = read_config_lifetime();
    if (lifetime == CONFIG_LIFETIME_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto maintenance_mask_step;
        }
        return;
    }
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
    snprintf(final_profile, sizeof(final_profile), "Maintenance-%s", iface);
    printf("\n维护口不设置路由器或 DNS，也不负责默认联网。\n");
    {
        terminal_ui_prepare_step(true);
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);
        if (result == NETWORK_ACTIVATION_FAILED)
        {
            bool go_back = terminal_ui_back_requested();

            cleanup_temp_profile(&temp_profile);
            if (go_back)
            {
                goto maintenance_mask_step;
            }
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

void configure_wired(void)
{
    char iface[IF_NAMESIZE];
    char final_profile[PROFILE_SIZE];
    ConnectionProfile temp_profile;
    IPv4Config cfg;
    ConfigLifetime lifetime;
    int route_metric;

    final_profile[0] = '\0';
    printf("\n========== 有线网络详细设置 ==========\n");
    show_devices();
    show_current_overlap_warnings();

interface_step:
    terminal_ui_set_step("选择有线网口",
                         "选择要创建或修改网络配置的物理网口");
    terminal_ui_prepare_step(false);
    if (!choose_ethernet_interface(iface, sizeof(iface)))
    {
        return;
    }

lifetime_step:
    terminal_ui_prepare_step(true);
    lifetime = read_config_lifetime();
    if (lifetime == CONFIG_LIFETIME_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto interface_step;
        }
        return;
    }

profile_name_step:
    {
        char suggested_name[PROFILE_SIZE];

        snprintf(suggested_name, sizeof(suggested_name), "Wired-%s", iface);
        terminal_ui_set_step("设置连接名称",
                             "用于在系统中识别这项有线配置");
        terminal_ui_prepare_step(true);
        if (!ask_final_profile_name(final_profile[0]
                                        ? final_profile
                                        : suggested_name,
                                    final_profile,
                                    sizeof(final_profile)))
        {
            if (terminal_ui_back_requested())
            {
                goto lifetime_step;
            }
            return;
        }
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

ipv4_profile_step:
    if (configure_ipv4_profile(temp_profile.uuid, &cfg,
                               &route_metric) != 0)
    {
        bool go_back = terminal_ui_back_requested();

        if (!go_back)
        {
            printf("IPv4 配置失败。\n");
        }
        cleanup_temp_profile(&temp_profile);
        if (go_back)
        {
            goto profile_name_step;
        }
        return;
    }

    terminal_ui_prepare_step(true);
    if (cfg.is_static &&
        !approve_candidate_network(iface, cfg.ip, cfg.prefix, route_metric))
    {
        if (terminal_ui_back_requested())
        {
            goto ipv4_profile_step;
        }
        printf("网段检查未通过，本次设置已取消。\n");
        cleanup_temp_profile(&temp_profile);
        return;
    }

    printf("\n测试连接已建立，开机不会自动使用；保存名称：%s\n",
           final_profile);

    {
        terminal_ui_prepare_step(true);
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);

        if (result == NETWORK_ACTIVATION_FAILED)
        {
            if (terminal_ui_back_requested())
            {
                goto ipv4_profile_step;
            }
            cleanup_temp_profile(&temp_profile);
            return;
        }
        if (result == NETWORK_ACTIVATION_PENDING)
        {
            terminal_ui_shutdown();
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
    int connection_action;
    bool preconfigure_only;
    bool scan_nearby;
    bool hidden;
    IPv4Config cfg;
    ConfigLifetime lifetime;
    int route_metric;

    ssid[0] = '\0';
    password[0] = '\0';
    final_profile[0] = '\0';

    printf("\n========== 配置无线网络 ==========\n");
    show_devices();
    show_current_overlap_warnings();

wifi_interface_step:
    terminal_ui_set_step("选择 Wi-Fi 网卡",
                         "选择要配置的无线网卡");
    terminal_ui_prepare_step(false);
    if (!choose_wifi_interface(iface, sizeof(iface)))
    {
        memset(password, 0, sizeof(password));
        return;
    }

connection_action_step:
    terminal_ui_set_step("选择配置用途",
                         "立即连接测试，或只保存供以后使用");
    printf("\n请选择配置后的处理方式：\n");
    printf("  1. 立即连接并测试：适合目标 Wi-Fi 当前就在附近\n");
    printf("  2. 仅保存，暂不连接：适合提前配置异地 Wi-Fi，进入覆盖范围后自动连接\n");
    terminal_ui_prepare_step(true);
    connection_action = read_int("请选择 [1-2]: ", 1, 2);
    if (connection_action == TERMINAL_UI_INPUT_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto wifi_interface_step;
        }
        memset(password, 0, sizeof(password));
        return;
    }
    preconfigure_only = connection_action == 2;

    if (preconfigure_only)
    {
        lifetime = CONFIG_LIFETIME_PERMANENT;
        printf("[说明] 离线预配置永久写入系统；本工具不会验证名称、密码或现场参数。\n");
    }
    else
    {
lifetime_step:
        terminal_ui_set_step("选择保存方式",
                             "临时配置重启失效，永久配置重启保留");
        terminal_ui_prepare_step(true);
        lifetime = read_config_lifetime();
        if (lifetime == CONFIG_LIFETIME_CANCELLED)
        {
            if (terminal_ui_back_requested())
            {
                goto connection_action_step;
            }
            memset(password, 0, sizeof(password));
            return;
        }
    }

scan_question_step:
    terminal_ui_set_step("选择 SSID 来源",
                         "扫描附近 Wi-Fi，或手工填写网络名称");
    terminal_ui_prepare_step(true);
    scan_nearby = read_yes_no(
        preconfigure_only
            ? "是否仍要扫描当前附近的 Wi-Fi？[y/N]: "
            : "是否扫描附近 Wi-Fi？[Y/n]: ",
        !preconfigure_only);
    if (terminal_ui_input_cancelled())
    {
        if (terminal_ui_back_requested())
        {
            if (preconfigure_only)
            {
                goto connection_action_step;
            }
            goto lifetime_step;
        }
        memset(password, 0, sizeof(password));
        return;
    }

    if (!preconfigure_only || scan_nearby)
    {
        const char *on[] = {
            "nmcli", "radio", "wifi", "on", NULL};
        run_cmd(on);
    }

wifi_name_step:
    if (scan_nearby)
    {
        terminal_ui_set_step("选择目标 Wi-Fi",
                             "已重新扫描；选择网络或使用手工输入");
        terminal_ui_prepare_step(true);
        NearbyWifiChoiceResult wifi_choice = choose_nearby_wifi_ssid(
            iface, ssid, sizeof(ssid));

        if (wifi_choice == NEARBY_WIFI_CHOICE_CANCELLED)
        {
            if (terminal_ui_back_requested())
            {
                goto scan_question_step;
            }
            memset(password, 0, sizeof(password));
            return;
        }
    }

    while (ssid[0] == '\0')
    {
        char entered_ssid[BUF_SIZE];

        terminal_ui_set_step("填写 Wi-Fi 名称",
                             "输入 SSID，区分大小写");
        terminal_ui_prepare_step(true);
        if (!read_line("无线网络名称: ",
                       entered_ssid, sizeof(entered_ssid)))
        {
            if (terminal_ui_back_requested())
            {
                goto scan_question_step;
            }
            memset(password, 0, sizeof(password));
            return;
        }
        trim_space(entered_ssid);
        snprintf(ssid, sizeof(ssid), "%s", entered_ssid);

        if (ssid[0] == '\0')
        {
            printf("无线网络名称不能为空。\n");
        }
        else if (strlen(ssid) > 32)
        {
            printf("无线网络名称不能超过 32 个字节。\n");
        }
        if (strlen(ssid) > 32)
        {
            ssid[0] = '\0';
        }
    }

hidden_step:
    terminal_ui_set_step("设置隐藏网络",
                         "只有 SSID 不对外广播时才选“是”");
    terminal_ui_prepare_step(true);
    hidden = read_yes_no("这个无线网络是否隐藏名称（SSID）？[y/N]: ", false);
    if (terminal_ui_input_cancelled())
    {
        if (terminal_ui_back_requested())
        {
            if (!scan_nearby)
            {
                ssid[0] = '\0';
            }
            goto wifi_name_step;
        }
        memset(password, 0, sizeof(password));
        return;
    }

security_step:
    terminal_ui_set_step("选择安全方式",
                         "按路由器的加密类型选择");
    printf("\n请选择无线安全方式：\n");
    printf("  1. 普通密码（WPA/WPA2，家用与现场常见）\n");
    printf("  2. WPA3 密码\n");
    printf("  3. 无密码\n");
    printf("  账号、证书等企业认证暂不支持。\n");

    terminal_ui_prepare_step(true);
    security = read_int("请选择 [1-3]: ", 1, 3);
    if (security == TERMINAL_UI_INPUT_CANCELLED)
    {
        if (terminal_ui_back_requested())
        {
            goto hidden_step;
        }
        memset(password, 0, sizeof(password));
        return;
    }

password_step:
    if (security == 1 || security == 2)
    {
        do
        {
            char entered_password[BUF_SIZE];
            const char *password_prompt = password[0]
                ? "Wi-Fi 密码（已填写，回车保留）: "
                : "Wi-Fi 密码（输入时不显示）: ";

            terminal_ui_set_step("填写 Wi-Fi 密码",
                                 "密码仅用星号显示，不会明文回显");
            terminal_ui_prepare_step(true);
            if (!read_password(password_prompt,
                               entered_password, sizeof(entered_password)))
            {
                memset(entered_password, 0, sizeof(entered_password));
                if (terminal_ui_back_requested())
                {
                    goto security_step;
                }
                memset(password, 0, sizeof(password));
                return;
            }
            if (entered_password[0])
            {
                snprintf(password, sizeof(password), "%s", entered_password);
            }
            memset(entered_password, 0, sizeof(entered_password));

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

profile_name_step:
    {
        char suggested_name[PROFILE_SIZE];

        snprintf(suggested_name, sizeof(suggested_name), "WiFi-%s", iface);
        terminal_ui_set_step("设置连接名称",
                             "用于在系统中识别这项 Wi-Fi 配置");
        terminal_ui_prepare_step(true);
        if (!ask_final_profile_name(final_profile[0]
                                        ? final_profile
                                        : suggested_name,
                                    final_profile,
                                    sizeof(final_profile)))
        {
            if (terminal_ui_back_requested())
            {
                if (security == 1 || security == 2)
                {
                    goto password_step;
                }
                goto security_step;
            }
            memset(password, 0, sizeof(password));
            return;
        }
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

    if (hidden)
    {
        const char *hidden_network[] = {
            "nmcli", "connection", "modify",
            "uuid", temp_profile.uuid,
            "802-11-wireless.hidden", "yes",
            NULL};

        if (run_cmd(hidden_network) != 0)
        {
            printf("设置隐藏无线网络失败。\n");
            cleanup_temp_profile(&temp_profile);
            return;
        }
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

wifi_ipv4_profile_step:
    if (configure_ipv4_profile(temp_profile.uuid, &cfg,
                               &route_metric) != 0)
    {
        bool go_back = terminal_ui_back_requested();

        if (!go_back)
        {
            printf("IPv4 配置失败。\n");
        }
        cleanup_temp_profile(&temp_profile);
        if (go_back)
        {
            goto profile_name_step;
        }
        memset(password, 0, sizeof(password));
        return;
    }
    terminal_ui_prepare_step(true);
    if (cfg.is_static &&
        !approve_candidate_network(iface, cfg.ip, cfg.prefix, route_metric))
    {
        if (terminal_ui_back_requested())
        {
            goto wifi_ipv4_profile_step;
        }
        printf("网段检查未通过，本次设置已取消。\n");
        cleanup_temp_profile(&temp_profile);
        return;
    }

    if (preconfigure_only)
    {
        terminal_ui_set_step("确认保存 Wi-Fi",
                             "最后核对网卡、SSID 和连接名称");
        printf("\n========== 保存异地 Wi-Fi 预配置 ==========\n");
        printf("无线网卡：%s\n", iface);
        printf("无线名称：%s%s\n", ssid, hidden ? "（隐藏）" : "");
        printf("连接名称：%s\n", final_profile);
        printf("保存后会开启自动连接，并设置较高的自动连接优先级。\n");
        printf("本次不尝试连接，无法提前发现名称、密码或 IP 填写错误；\n");
        printf("已连其他 Wi-Fi 时不会强制切换，系统会在启动或重连时再选择。\n");

        terminal_ui_prepare_step(true);
        if (!read_exact_word("确认仅保存供以后自动连接请输入 SAVE: ", "SAVE"))
        {
            if (terminal_ui_back_requested())
            {
                goto wifi_ipv4_profile_step;
            }
            printf("已取消保存。\n");
            cleanup_temp_profile(&temp_profile);
            return;
        }

        if (!finalize_profile(&temp_profile, final_profile, NULL))
        {
            printf("[失败] 无线预配置未能完整保存。\n");
            cleanup_temp_profile(&temp_profile);
            return;
        }

        printf("[完成] 未切换或测试当前网络；当 Wi-Fi \"%s\" 可用且网卡空闲时自动连接。\n",
               ssid);
        return;
    }

    printf("\n测试连接已建立：%s（开机不会自动使用，测试失败不影响下次启动）\n",
           temp_profile.name);

    {
        terminal_ui_prepare_step(true);
        NetworkActivationResult result = safe_activate_with_reconnect(
            iface, &temp_profile, &cfg, final_profile,
            lifetime == CONFIG_LIFETIME_PERMANENT
                ? NETWORK_PROFILE_PERMANENT
                : NETWORK_PROFILE_TEMPORARY);

        if (result == NETWORK_ACTIVATION_FAILED)
        {
            if (terminal_ui_back_requested())
            {
                goto wifi_ipv4_profile_step;
            }
            cleanup_temp_profile(&temp_profile);
            return;
        }
        if (result == NETWORK_ACTIVATION_PENDING)
        {
            terminal_ui_shutdown();
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
    terminal_ui_set_step("填写测试地址",
                         "输入需要检查连通性的 IPv4 地址");
    terminal_ui_prepare_step(false);

    for (;;)
    {
        if (!read_line("目标 IPv4 地址（直接回车取消）: ",
                       target, sizeof(target)))
        {
            return;
        }
        trim_space(target);

        if (target[0] == '\0')
        {
            printf("已取消。\n");
            return;
        }

        if (valid_ipv4(target))
        {
            break;
        }

        printf("IP 地址格式不正确。\n");
    }

    {
        const char *argv[] = {
            "ping", "-c", "4", target, NULL};
        printf("%s %s\n",
               run_cmd_silent(argv) == 0
                   ? "[正常] 可以连接："
                   : "[失败] 无法连接：",
               target);
    }
}
