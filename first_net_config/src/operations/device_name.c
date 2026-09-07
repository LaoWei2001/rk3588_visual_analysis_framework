#define _POSIX_C_SOURCE 200809L

#include "operations/device_name.h"

#include "common/cli_io.h"
#include "common/command_runner.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_NAME_SIZE 254

static bool valid_device_name(const char *value)
{
    const char *label = value;
    size_t total;

    if (!value || (total = strlen(value)) == 0 || total > 253 ||
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
    char requested[DEVICE_NAME_SIZE];

    if (gethostname(current, sizeof(current) - 1) != 0)
    {
        snprintf(current, sizeof(current), "未读取到");
    }
    printf("\n========== 设备名称 ==========\n");
    printf("当前名称：%s\n", current);
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
        printf("名称只能包含字母、数字、短横线和点，短横线不能放在每段的开头或结尾。\n");
    }

    if (strcmp(current, requested) == 0)
    {
        printf("设备名称没有变化。\n");
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
        if (run_cmd_silent(argv) == 0)
        {
            printf("[完成] 设备名称已更新。\n");
        }
        else
        {
            printf("[失败] 无法更新设备名称，请确认系统提供 hostnamectl。\n");
        }
    }
}
