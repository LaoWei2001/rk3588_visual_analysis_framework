#define _POSIX_C_SOURCE 200809L

#include "cli_io.h"
#include "network_safety.h"
#include "network_state.h"
#include "network_workflows.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_menu(void)
{
    printf("\n");
    printf("====================================================\n");
    printf("                 网络配置工具 v1.6\n");
    printf("====================================================\n");
    printf("1. 查看网卡状态\n");
    printf("2. 配置有线网络\n");
    printf("3. 有线网络详细设置\n");
    printf("4. 配置无线网络\n");
    printf("5. 查看当前网络信息\n");
    printf("6. 搜索附近无线网络\n");
    printf("7. 测试指定地址能否连接\n");
    printf("8. 查看已保存的网络配置\n");
    printf("9. 删除已保存的网络配置\n");
    printf("10. 测试使用已有网络配置\n");
    printf("0. 退出\n");
    printf("====================================================\n");
}

int main(int argc, char *argv[])
{
    if (argc == 3 && strcmp(argv[1], "--watch-pending") == 0)
    {
        if (geteuid() != 0)
        {
            return 1;
        }
        return run_pending_network_watchdog(argv[2]);
    }

    if (argc != 1)
    {
        fprintf(stderr, "用法：%s\n", argv[0]);
        return 2;
    }

    if (geteuid() != 0)
    {
        fprintf(stderr,
                "错误：请使用 root 用户运行本工具。\n"
                "例如：sudo ./first_net_config\n");
        return 1;
    }

    if (!nmcli_available())
    {
        fprintf(stderr,
                "错误：未找到可用的 nmcli。\n"
                "本版本要求系统使用 NetworkManager。\n");
        return 1;
    }

    if (handle_pending_network_change())
    {
        return 0;
    }

    printf("[安全保护] 网络切换带有可跨 SSH 会话确认的自动恢复保护。\n");

    for (;;)
    {
        int choice;

        print_menu();
        choice = read_int("请选择功能 [0-10]: ", 0, 10);

        switch (choice)
        {
        case 1:
            show_devices();
            break;
        case 2:
            configure_wired_beginner();
            break;
        case 3:
            configure_wired();
            break;
        case 4:
            configure_wifi();
            break;
        case 5:
            show_network_state();
            break;
        case 6:
            scan_wifi();
            break;
        case 7:
            ping_test();
            break;
        case 8:
            show_connection_detail();
            break;
        case 9:
            delete_connection();
            break;
        case 10:
            activate_saved_connection_safely();
            break;
        case 0:
            printf("退出。\n");
            return 0;
        default:
            break;
        }
    }
}
