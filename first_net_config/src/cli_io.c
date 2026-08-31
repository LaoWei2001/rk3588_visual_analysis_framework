#define _POSIX_C_SOURCE 200809L

#include "cli_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

static void trim_newline(char *s)
{
    size_t n;

    if (!s)
    {
        return;
    }

    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    {
        s[n - 1] = '\0';
        n--;
    }
}

void trim_space(char *s)
{
    char *start;
    char *end;

    if (!s || s[0] == '\0')
    {
        return;
    }

    start = s;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
    {
        start++;
    }

    if (start != s)
    {
        memmove(s, start, strlen(start) + 1);
    }

    end = s + strlen(s);
    while (end > s &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
    {
        end--;
    }
    *end = '\0';
}

void read_line(const char *prompt, char *buf, size_t size)
{
    if (prompt)
    {
        printf("%s", prompt);
        fflush(stdout);
    }

    if (!fgets(buf, (int)size, stdin))
    {
        buf[0] = '\0';
        printf("\n输入已结束，程序退出。\n");
        exit(EXIT_SUCCESS);
    }

    trim_newline(buf);
}

int read_int(const char *prompt, int min_value, int max_value)
{
    char buf[64];

    for (;;)
    {
        char *end = NULL;
        long value;

        read_line(prompt, buf, sizeof(buf));
        errno = 0;
        value = strtol(buf, &end, 10);

        if (errno == 0 &&
            end != buf &&
            *end == '\0' &&
            value >= min_value &&
            value <= max_value)
        {
            return (int)value;
        }

        printf("输入无效，请输入 %d 到 %d 之间的数字。\n",
               min_value, max_value);
    }
}

bool read_yes_no(const char *prompt, bool default_yes)
{
    char buf[32];

    for (;;)
    {
        read_line(prompt, buf, sizeof(buf));
        trim_space(buf);

        if (buf[0] == '\0')
        {
            return default_yes;
        }

        if (strcasecmp(buf, "y") == 0 ||
            strcasecmp(buf, "yes") == 0)
        {
            return true;
        }

        if (strcasecmp(buf, "n") == 0 ||
            strcasecmp(buf, "no") == 0)
        {
            return false;
        }

        printf("请输入 y/yes 或 n/no。\n");
    }
}

bool read_exact_yes(const char *prompt)
{
    char buf[32];

    printf("%s", prompt);
    fflush(stdout);

    /*
     * 这里不能把 EOF 当作“默认同意”。
     * 如果 SSH 在切网后断开，stdin 可能直接 EOF。
     * 只有用户明确输入 YES 才表示确认。
     */
    if (!fgets(buf, sizeof(buf), stdin))
    {
        return false;
    }

    trim_newline(buf);
    trim_space(buf);

    return strcmp(buf, "YES") == 0;
}

bool read_exact_word(const char *prompt, const char *expected)
{
    char buf[64];

    if (!expected)
    {
        return false;
    }

    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin))
    {
        return false;
    }

    trim_newline(buf);
    trim_space(buf);
    return strcmp(buf, expected) == 0;
}

ConfigLifetime read_config_lifetime(void)
{
    printf("\n请选择本次配置的保存方式（必须明确选择）：\n");
    printf("  1. 临时配置：不写入磁盘；设备重启或网络服务重启后消失\n");
    printf("  2. 永久配置：保存到系统；重启后自动恢复\n");

    return (ConfigLifetime)read_int("请选择 [1-2]: ", 1, 2);
}

void read_password(const char *prompt, char *buf, size_t size)
{
    struct termios oldt;
    struct termios newt;
    bool tty_ok = (tcgetattr(STDIN_FILENO, &oldt) == 0);

    printf("%s", prompt);
    fflush(stdout);

    if (tty_ok)
    {
        newt = oldt;
        newt.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt);
    }

    if (!fgets(buf, (int)size, stdin))
    {
        buf[0] = '\0';
        if (tty_ok)
        {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        }
        printf("\n输入已结束，程序退出。\n");
        exit(EXIT_SUCCESS);
    }
    else
    {
        trim_newline(buf);
    }

    if (tty_ok)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
    }

    printf("\n");
}
