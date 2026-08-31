/*
 * Copyright (C) 2026, Sunny_Wei, all rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200809L

#include "cli_io.h"
#include "network_safety.h"
#include "network_state.h"
#include "network_workflows.h"
#include "interface_inspector.h"
#include "factory_restore.h"
#include "network_cleanup.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_menu(void)
{
    printf("\n");
    printf("====================================================\n");
    printf("              RK3588 现场网络配置工具\n");
    printf("====================================================\n");
    printf("1. 查看全部网卡、IP 和网段冲突\n");
    printf("2. 搜索摄像头并配置所选有线网口\n");
    printf("3. 配置笔记本直连维护网络\n");
    printf("4. 配置所选有线网口（向导）\n");
    printf("5. 配置所选有线网口（详细）\n");
    printf("6. 配置所选无线网口\n");
    printf("7. 查看完整 NetworkManager 状态\n");
    printf("8. 搜索附近无线网络\n");
    printf("9. 测试指定地址能否连接\n");
    printf("10. 查看已保存的网络配置\n");
    printf("11. 安全删除未启用的网络配置\n");
    printf("12. 测试使用已有网络配置\n");
    printf("13. 查看初始网络基准及恢复预览\n");
    printf("14. 保存系统初始网络基准（制作镜像）\n");
    printf("15. 恢复系统初始网络配置（永久）\n");
    printf("16. 清除所有 NetworkManager 连接配置（高风险）\n");
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
        choice = read_int("请选择功能 [0-16]: ", 0, 16);

        switch (choice)
        {
        case 1:
            show_interface_overview();
            show_current_overlap_warnings();
            break;
        case 2:
            configure_camera_network();
            break;
        case 3:
            configure_maintenance_link();
            break;
        case 4:
            configure_wired_beginner();
            break;
        case 5:
            configure_wired();
            break;
        case 6:
            configure_wifi();
            break;
        case 7:
            show_network_state();
            break;
        case 8:
            scan_wifi();
            break;
        case 9:
            ping_test();
            break;
        case 10:
            show_connection_detail();
            break;
        case 11:
            delete_connection();
            break;
        case 12:
            activate_saved_connection_safely();
            break;
        case 13:
            preview_factory_restore();
            break;
        case 14:
            capture_factory_baseline();
            break;
        case 15:
            restore_factory_network();
            break;
        case 16:
            clear_all_network_connections();
            break;
        case 0:
            printf("退出。\n");
            return 0;
        default:
            break;
        }
    }
}
