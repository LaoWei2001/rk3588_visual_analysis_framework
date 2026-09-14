#define _POSIX_C_SOURCE 200809L

#include "common/cli_io.h"

#include <errno.h>
#include <limits.h>
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

static void discard_line_remainder(void)
{
    int character;

    do
    {
        character = getchar();
    } while (character != '\n' && character != EOF);
}

static bool input_line_complete(const char *text)
{
    return text && (strchr(text, '\n') != NULL || feof(stdin));
}

static bool utf8_continuation_byte(unsigned char value)
{
    return (value & 0xc0U) == 0x80U;
}

static bool remove_last_utf8_character(char *buf, size_t *length)
{
    if (!buf || !length || *length == 0)
    {
        return false;
    }

    --(*length);
    while (*length > 0 &&
           utf8_continuation_byte((unsigned char)buf[*length]))
    {
        --(*length);
    }
    buf[*length] = '\0';
    return true;
}

static void erase_masked_characters(size_t count)
{
    while (count-- > 0)
    {
        fputs("\b \b", stdout);
    }
    fflush(stdout);
}

void clear_terminal_screen(void)
{
    if (!isatty(STDOUT_FILENO))
    {
        return;
    }

    fputs("\033[2J\033[H", stdout);
    fflush(stdout);
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

bool read_line(const char *prompt, char *buf, size_t size)
{
    if (terminal_ui_enabled())
        return terminal_ui_read_text(prompt, buf, size, false);

    if (!buf || size < 2 || size > INT_MAX)
    {
        return false;
    }

    for (;;)
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
        if (input_line_complete(buf))
        {
            trim_newline(buf);
            return true;
        }

        discard_line_remainder();
        buf[0] = '\0';
        printf("输入内容过长，请缩短后重新输入。\n");
    }
}

int read_int(const char *prompt, int min_value, int max_value)
{
    char buf[64];

    if (terminal_ui_enabled())
    {
        return terminal_ui_read_int(prompt, min_value, max_value);
    }

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

    if (terminal_ui_enabled())
    {
        return terminal_ui_confirm(prompt, default_yes);
    }

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

    if (terminal_ui_enabled())
    {
        if (!terminal_ui_read_text(prompt, buf, sizeof(buf), false))
        {
            return false;
        }
        trim_space(buf);
        return strcmp(buf, "YES") == 0;
    }

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

    if (!input_line_complete(buf))
    {
        discard_line_remainder();
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

    if (terminal_ui_enabled())
    {
        if (!terminal_ui_read_text(prompt, buf, sizeof(buf), false))
        {
            return false;
        }
        trim_space(buf);
        return strcmp(buf, expected) == 0;
    }

    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin))
    {
        return false;
    }
    if (!input_line_complete(buf))
    {
        discard_line_remainder();
        return false;
    }

    trim_newline(buf);
    trim_space(buf);
    return strcmp(buf, expected) == 0;
}

ConfigLifetime read_config_lifetime(void)
{
    terminal_ui_set_step("选择保存方式",
                         "临时配置重启失效，永久配置重启保留");
    printf("\n请选择本次配置的保存方式（必须明确选择）：\n");
    printf("  1. 临时配置：不写入磁盘；设备重启或网络服务重启后消失\n");
    printf("  2. 永久配置：保存到系统；重启后自动恢复\n");
    printf("  0. 取消本次配置\n");

    int choice = read_int("请选择 [0-2]: ", 0, 2);

    return choice == 0 || choice == TERMINAL_UI_INPUT_CANCELLED
               ? CONFIG_LIFETIME_CANCELLED
               : (ConfigLifetime)choice;
}

bool read_password(const char *prompt, char *buf, size_t size)
{
    struct termios oldt;
    struct termios newt;
    bool tty_ok = (tcgetattr(STDIN_FILENO, &oldt) == 0);

    if (terminal_ui_enabled())
        return terminal_ui_read_text(prompt, buf, size, true);
    if (!buf || size < 2 || size > INT_MAX)
    {
        return false;
    }

    if (tty_ok)
    {
        newt = oldt;
        newt.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        newt.c_iflag &= (tcflag_t)~(IXON | ICRNL);
        newt.c_cc[VMIN] = 1;
        newt.c_cc[VTIME] = 0;
        tty_ok = (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == 0);
    }

    for (;;)
    {
        size_t length = 0;
        size_t mask_count = 0;
        bool overflow = false;

        buf[0] = '\0';
        printf("%s", prompt ? prompt : "");
        fflush(stdout);
        if (tty_ok)
        {
            (void)tcdrain(STDOUT_FILENO);
            for (;;)
            {
                unsigned char character;
                ssize_t count;

                do
                {
                    count = read(STDIN_FILENO, &character, 1);
                } while (count < 0 && errno == EINTR);
                if (count != 1)
                {
                    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                    buf[0] = '\0';
                    printf("\n输入已结束，程序退出。\n");
                    exit(EXIT_SUCCESS);
                }
                if (character == '\r' || character == '\n')
                {
                    printf("\n");
                    break;
                }
                if (character == 8 || character == 127)
                {
                    if (remove_last_utf8_character(buf, &length))
                    {
                        if (mask_count > 0)
                        {
                            --mask_count;
                        }
                        erase_masked_characters(1);
                    }
                    continue;
                }
                if (character == 21)
                {
                    length = 0;
                    buf[0] = '\0';
                    overflow = false;
                    erase_masked_characters(mask_count);
                    mask_count = 0;
                    continue;
                }
                if (character < 32)
                {
                    continue;
                }
                if (length + 1 >= size)
                {
                    overflow = true;
                    fputc('\a', stdout);
                    fflush(stdout);
                    continue;
                }

                buf[length++] = (char)character;
                buf[length] = '\0';
                if (!utf8_continuation_byte(character))
                {
                    fputc('*', stdout);
                    fflush(stdout);
                    (void)tcdrain(STDOUT_FILENO);
                    ++mask_count;
                }
            }
        }
        else
        {
            if (!fgets(buf, (int)size, stdin))
            {
                buf[0] = '\0';
                printf("\n输入已结束，程序退出。\n");
                exit(EXIT_SUCCESS);
            }
            if (input_line_complete(buf))
            {
                trim_newline(buf);
                return true;
            }
            overflow = true;
            discard_line_remainder();
        }

        if (!overflow)
        {
            if (tty_ok)
            {
                (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            }
            return true;
        }
        buf[0] = '\0';
        printf("密码过长，请缩短后重新输入。\n");
    }
}
