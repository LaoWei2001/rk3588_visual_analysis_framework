#define _XOPEN_SOURCE 700

#include "common/intro_notice.h"

#include <curses.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define INTRO_NOTICE_LINE_COUNT 7
#define INTRO_NOTICE_COLOR 10

static const char *const notice_lines[INTRO_NOTICE_LINE_COUNT] = {
    "Copyright (C) 2026 JNU IOT C301 Sunny_Wei",
    "许可证 GPLv3+：GNU 通用公共许可证",
    "第 3 版或更新版本",
    "<https://gnu.org/licenses/gpl.html>",
    "本软件是自由软件：您可以自由修改和重新发布它。",
    "在法律允许的范围内不提供任何其他保证。",
    "由 Sunny_Wei 编写。"};

static int text_width(const char *text)
{
    mbstate_t state;
    const char *source = text;
    wchar_t value;
    size_t length;
    int width = 0;

    memset(&state, 0, sizeof(state));
    while (*source)
    {
        length = mbrtowc(&value, source, MB_CUR_MAX, &state);
        if (length == (size_t)-1 || length == (size_t)-2)
        {
            memset(&state, 0, sizeof(state));
            ++source;
            ++width;
            continue;
        }
        if (length == 0)
        {
            break;
        }
        {
            int cell_width = wcwidth(value);
            width += cell_width > 0 ? cell_width : 1;
        }
        source += length;
    }
    return width;
}

int intro_notice_reserved_rows(void)
{
    return INTRO_NOTICE_LINE_COUNT + 1;
}

void intro_notice_draw(int first_row)
{
    /* 启动声明需要在深色背景上保持亮白，不能使用会显示成灰色的 A_DIM。 */
    int attributes = A_BOLD;

    if (has_colors() && COLOR_PAIRS > INTRO_NOTICE_COLOR)
    {
        init_pair(INTRO_NOTICE_COLOR, COLOR_WHITE, COLOR_BLACK);
        attributes |= COLOR_PAIR(INTRO_NOTICE_COLOR);
    }
    attron(attributes);
    for (int index = 0; index < INTRO_NOTICE_LINE_COUNT; ++index)
    {
        int row = first_row + index;
        int width;
        int column;

        if (row < 0 || row >= LINES)
        {
            continue;
        }
        width = text_width(notice_lines[index]);
        column = (COLS - width) / 2;
        if (column < 0)
        {
            column = 0;
        }
        mvaddstr(row, column, notice_lines[index]);
    }
    attroff(attributes);
}

void intro_notice_print(FILE *stream)
{
    if (!stream)
    {
        return;
    }
    for (int index = 0; index < INTRO_NOTICE_LINE_COUNT; ++index)
    {
        fprintf(stream, "%s\n", notice_lines[index]);
    }
    fputc('\n', stream);
}
