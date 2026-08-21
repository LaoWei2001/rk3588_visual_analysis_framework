#define _POSIX_C_SOURCE 200809L

#include "ipv4_utils.h"
#include "cli_io.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool valid_ipv4(const char *s)
{
    struct in_addr addr;
    return s && inet_pton(AF_INET, s, &addr) == 1;
}

static bool parse_ipv4_canonical(const char *input,
                                 char *output,
                                 size_t output_size)
{
    struct in_addr addr;

    if (!input || !output || output_size == 0)
    {
        return false;
    }

    if (inet_pton(AF_INET, input, &addr) != 1)
    {
        return false;
    }

    if (!inet_ntop(AF_INET, &addr, output, output_size))
    {
        return false;
    }

    return true;
}

void read_ipv4_required(const char *prompt,
                               char *output,
                               size_t output_size)
{
    char raw[128];
    char canonical[INET_ADDRSTRLEN];

    for (;;)
    {
        read_line(prompt, raw, sizeof(raw));
        trim_space(raw);

        if (!parse_ipv4_canonical(raw,
                                  canonical,
                                  sizeof(canonical)))
        {
            printf("IPv4 地址格式无效，例如：10.202.30.101\n");
            continue;
        }

        snprintf(output, output_size, "%s", canonical);

        /*
         * 关键安全检查：
         * 马上把程序真正解析到的地址打印回来。
         * 后续 nmcli 使用的就是这个 output。
         */
        printf("   程序解析为：%s\n", output);
        return;
    }
}

void read_ipv4_optional(const char *prompt,
                               char *output,
                               size_t output_size)
{
    char raw[128];
    char canonical[INET_ADDRSTRLEN];

    for (;;)
    {
        read_line(prompt, raw, sizeof(raw));
        trim_space(raw);

        if (raw[0] == '\0')
        {
            output[0] = '\0';
            return;
        }

        if (!parse_ipv4_canonical(raw,
                                  canonical,
                                  sizeof(canonical)))
        {
            printf("IPv4 地址格式无效，例如：10.202.30.254；不需要时可直接回车。\n");
            continue;
        }

        snprintf(output, output_size, "%s", canonical);
        printf("   程序解析为：%s\n", output);
        return;
    }
}

bool confirm_network_parameters(const char *iface,
                                       const IPv4Config *cfg)
{
    printf("\n");
    printf("============================================================\n");
    printf("                 请确认下面的网络参数\n");
    printf("============================================================\n");
    printf("使用网口  : %s\n", iface);

    if (cfg->is_static)
    {
        printf("IP 地址   : %s/%d\n", cfg->ip, cfg->prefix);
        printf("网关       : %s\n",
               cfg->gateway[0] ? cfg->gateway : "未设置");
        printf("域名服务器 : %s\n",
               cfg->dns[0] ? cfg->dns : "未设置");
    }
    else
    {
        printf("IP 地址   : 自动获取\n");
    }

    printf("============================================================\n");
    printf("请仔细核对。确认无误后再继续。\n");

    return read_exact_yes("确认无误请输入 YES；输入其他内容将取消本次操作: ");
}

int netmask_to_prefix(const char *mask)
{
    struct in_addr addr;
    uint32_t m;
    int prefix = 0;
    bool zero_seen = false;

    if (inet_pton(AF_INET, mask, &addr) != 1)
    {
        return -1;
    }

    m = ntohl(addr.s_addr);

    for (int i = 31; i >= 0; --i)
    {
        bool bit = ((m >> i) & 1U) != 0;

        if (bit)
        {
            if (zero_seen)
            {
                return -1;
            }
            prefix++;
        }
        else
        {
            zero_seen = true;
        }
    }

    return prefix;
}

bool ipv4_same_subnet(const char *ip_a,
                             const char *ip_b,
                             int prefix)
{
    struct in_addr a;
    struct in_addr b;
    uint32_t mask;

    if (!valid_ipv4(ip_a) || !valid_ipv4(ip_b) ||
        prefix < 0 || prefix > 32)
    {
        return false;
    }

    if (inet_pton(AF_INET, ip_a, &a) != 1 ||
        inet_pton(AF_INET, ip_b, &b) != 1)
    {
        return false;
    }

    if (prefix == 0)
    {
        mask = 0;
    }
    else
    {
        mask = 0xffffffffU << (32 - prefix);
    }

    return (ntohl(a.s_addr) & mask) ==
           (ntohl(b.s_addr) & mask);
}

bool is_network_or_broadcast_address(const char *ip,
                                            int prefix)
{
    struct in_addr addr;
    uint32_t host;
    uint32_t host_mask;

    if (!valid_ipv4(ip) || prefix < 0 || prefix > 32)
    {
        return true;
    }

    if (prefix >= 31)
    {
        return false;
    }

    if (inet_pton(AF_INET, ip, &addr) != 1)
    {
        return true;
    }

    host = ntohl(addr.s_addr);
    host_mask = (1U << (32 - prefix)) - 1U;

    return (host & host_mask) == 0 ||
           (host & host_mask) == host_mask;
}

