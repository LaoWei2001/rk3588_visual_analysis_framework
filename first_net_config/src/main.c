/*
 * Copyright (C) 2026 JNU IOT C301 Sunny_Wei, all rights reserved.
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

#include "common/cli_io.h"
#include "common/intro_notice.h"
#include "common/startup_intro.h"
#include "operations/network_safety.h"
#include "inspection/network_state.h"
#include "inspection/network_health.h"
#include "operations/network_workflows.h"
#include "inspection/interface_inspector.h"
#include "operations/factory_restore.h"
#include "operations/network_cleanup.h"
#include "operations/saved_connections.h"
#include "operations/device_name.h"
#include "inspection/network_quality_monitor.h"

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
    printf("4. 设置有线网络\n");
    printf("5. 设置无线网络\n");
    printf("6. 查看详细网络状态\n");
    printf("7. 搜索附近无线网络\n");
    printf("8. 测试指定地址能否连接\n");
    printf("9. 管理已保存的连接\n");
    printf("10. 查看或修改设备名称\n");
    printf("11. 查看初始网络设置和恢复内容\n"); 
    printf("12. 保存系统初始网络设置（制作镜像）\n");
    printf("13. 恢复系统初始网络设置\n");
    printf("14. 清除所有已保存的连接（高风险）\n");
    printf("15. 检查并修复网络问题\n");
    printf("16. 查看实时网络质量曲线\n");
    printf("0. 退出程序\n");
    printf("====================================================\n");
}

static const char *const tui_menu_labels[] = {
    "查看网卡与 IP",
    "搜索并配置摄像头",
    "配置维护网络",
    "设置有线网络",
    "设置 Wi-Fi",
    "查看网络状态",
    "搜索附近 Wi-Fi",
    "测试网络地址",
    "管理保存连接",
    "修改设备名称",
    "查看初始设置",
    "保存初始设置",
    "恢复初始设置",
    "清除全部连接",
    "网络检查与修复",
    "实时质量曲线",
    "退出程序"};

int main(int argc, char *argv[])
{
    bool plain_mode = false;

    if (argc == 3 && strcmp(argv[1], "--watch-pending") == 0)
    {
        if (geteuid() != 0)
        {
            return 1;
        }
        return run_pending_network_watchdog(argv[2]);
    }

    if (argc == 2 && strcmp(argv[1], "--plain") == 0)
    {
        plain_mode = true;
    }
    else if (argc != 1)
    {
        fprintf(stderr, "用法：%s [--plain]\n", argv[0]);
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

    if (!plain_mode && terminal_ui_start())
    {
        startup_intro_play();
    }
    else
    {
        intro_notice_print(stdout);
    }
    terminal_ui_begin_screen("启动检查");

    if (handle_pending_network_change())
    {
        terminal_ui_wait_for_return();
        return 0;
    }

    printf("[安全保护] 网络切换带有可跨 SSH 会话确认的自动恢复保护。\n");
    manage_default_route_health(true);

    for (;;)
    {
        int choice;

        if (terminal_ui_enabled())
        {
            int selected = terminal_ui_main_menu(
                tui_menu_labels,
                (int)(sizeof(tui_menu_labels) / sizeof(tui_menu_labels[0])));

            choice = selected == 16 ? 0 : selected + 1;
            if (choice != 0)
            {
                terminal_ui_begin_screen(tui_menu_labels[selected]);
            }
        }
        else
        {
            print_menu();
            choice = read_int("请选择功能 [0-16]: ", 0, 16);
        }

        switch (choice)
        {
        case 1:
            show_devices();
            show_current_overlap_warnings();
            break;
        case 2:
            configure_camera_network();
            break;
        case 3:
            configure_maintenance_link();
            break;
        case 4:
            configure_wired();
            break;
        case 5:
            configure_wifi();
            break;
        case 6:
            show_network_state();
            break;
        case 7:
            scan_wifi();
            break;
        case 8:
            ping_test();
            break;
        case 9:
            manage_saved_connections();
            break;
        case 10:
            manage_device_name();
            break;
        case 11:
            preview_factory_restore();
            break;
        case 12:
            capture_factory_baseline();
            break;
        case 13:
            restore_factory_network();
            break;
        case 14:
            clear_all_network_connections();
            break;
        case 15:
            manage_network_health();
            break;
        case 16:
            monitor_active_network_quality();
            break;
        case 0:
            if (!terminal_ui_enabled())
            {
                printf("退出。\n");
            }
            return 0;
        default:
            break;
        }
        /*
         * GUI 中在功能的第一步按 Esc，表示放弃该功能并直接回主菜单。
         * 不再把进入功能时打印的网卡概览当成“操作结果”单独停留一页。
         */
        if (!terminal_ui_consume_return_request() &&
            !terminal_ui_input_cancelled())
        {
            terminal_ui_wait_for_return();
        }
    }
}
