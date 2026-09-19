#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "operations/device_name.h"

#include "common/cli_io.h"
#include "common/command_runner.h"
#include "operations/device_name_store.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_NAME_SIZE 254
#define HOSTNAME_FILE "/etc/hostname"
#define HOSTS_FILE "/etc/hosts"
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif

static bool valid_device_name(const char *value)
{
    const char *label = value;
    size_t total;

    if (!value || (total = strlen(value)) == 0 || total > HOST_NAME_MAX ||
        value[total - 1] == '.')
    {
        return false;
    }

    while (*label)
    {
        const char *end = strchr(label, '.');
        size_t length = end ? (size_t)(end - label) : strlen(label);

        if (length == 0 || length > 63 || label[0] == '-' ||
            label[length - 1] == '-')
        {
            return false;
        }
        for (size_t index = 0; index < length; ++index)
        {
            unsigned char character = (unsigned char)label[index];
            if (!isalnum(character) && character != '-')
            {
                return false;
            }
        }
        if (!end)
        {
            break;
        }
        label = end + 1;
    }
    return true;
}

void manage_device_name(void)
{
    char current[DEVICE_NAME_SIZE] = {0};
    char persistent[DEVICE_NAME_SIZE] = {0};
    char requested[DEVICE_NAME_SIZE];
    bool current_known;
    bool persistent_known;

    current_known = gethostname(current, sizeof(current) - 1) == 0;
    persistent_known = device_name_read_file(
        HOSTNAME_FILE, persistent, sizeof(persistent));
    if (!current_known)
        snprintf(current, sizeof(current), "未读取到");
    printf("\n========== 设备名称 ==========\n");
    printf("当前运行名称：%s\n", current);
    printf("开机持久名称：%s\n",
           persistent_known ? persistent : "未读取到");
    if (current_known && persistent_known &&
        strcmp(current, persistent) != 0)
        printf("[提醒] 两个名称不一致；上次修改可能只完成了一部分。\n");
    printf("这个名称用于在局域网和系统记录中识别本设备。\n");

name_step:
    terminal_ui_set_step("填写设备名称",
                         "使用字母、数字、短横线或点");
    for (;;)
    {
        terminal_ui_prepare_step(false);
        if (!read_line("请输入新名称（直接回车取消）: ",
                       requested, sizeof(requested)))
        {
            return;
        }
        trim_space(requested);
        if (requested[0] == '\0')
        {
            printf("已取消。\n");
            return;
        }
        for (char *cursor = requested; *cursor; ++cursor)
        {
            *cursor = (char)tolower((unsigned char)*cursor);
        }
        if (valid_device_name(requested))
        {
            break;
        }
        printf("名称最长 %d 个字符，只能包含字母、数字、短横线和点，"
               "短横线不能放在每段的开头或结尾。\n", HOST_NAME_MAX);
    }

    if (current_known && persistent_known &&
        strcmp(current, requested) == 0 &&
        strcmp(persistent, requested) == 0)
    {
        if (!device_name_update_hosts(HOSTS_FILE, requested))
            printf("[提醒] 设备名称没有变化，但无法同步 /etc/hosts：%s\n",
                   strerror(errno));
        else
            printf("设备名称没有变化，本机名称解析记录已经同步。\n");
        return;
    }
    printf("设备名称将改为“%s”。\n", requested);
    terminal_ui_set_step("确认设备名称",
                         "保存后局域网和系统记录将使用新名称");
    terminal_ui_prepare_step(true);
    if (!read_yes_no("确定保存吗？[y/N]: ", false))
    {
        if (terminal_ui_back_requested())
        {
            goto name_step;
        }
        printf("已取消。\n");
        return;
    }
    {
        const char *argv[] = {
            "hostnamectl", "set-hostname", requested, NULL};
        int hostnamectl_result = run_cmd(argv);

        if (hostnamectl_result != 0)
        {
            printf("[提示] hostnamectl 未能完成持久化，正在使用兼容方式直接更新。\n");
            if (!device_name_write_file(HOSTNAME_FILE, requested))
            {
                printf("[失败] 无法写入 %s：%s\n", HOSTNAME_FILE,
                       strerror(errno));
                return;
            }
            if (sethostname(requested, strlen(requested)) != 0)
            {
                printf("[失败] 已写入开机名称，但无法更新当前运行名称：%s\n",
                       strerror(errno));
                return;
            }
        }
    }
    memset(current, 0, sizeof(current));
    memset(persistent, 0, sizeof(persistent));
    current_known = gethostname(current, sizeof(current) - 1) == 0;
    persistent_known = device_name_read_file(
        HOSTNAME_FILE, persistent, sizeof(persistent));
    if (!current_known || !persistent_known ||
        strcmp(current, requested) != 0 || strcmp(persistent, requested) != 0)
    {
        printf("[失败] 系统返回成功后校验不一致：运行名称=%s，开机名称=%s。\n",
               current_known ? current : "未读取到",
               persistent_known ? persistent : "未读取到");
        return;
    }
    if (!device_name_update_hosts(HOSTS_FILE, requested))
    {
        printf("[部分完成] 设备名称已更新，但无法同步 %s：%s\n",
               HOSTS_FILE, strerror(errno));
        printf("请检查该文件权限，否则本机可能无法用新名称解析自己。\n");
    }
    else
    {
        printf("[完成] 设备名称已更新，开机名称和本机解析记录均已同步。\n");
    }
}
