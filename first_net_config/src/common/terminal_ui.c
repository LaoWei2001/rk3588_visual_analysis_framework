#define _XOPEN_SOURCE 700

#include "common/terminal_ui.h"

#include <curses.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define UI_MAX_LINES 2048
#define UI_LINE_SIZE 2048
#define UI_FORMAT_STACK_SIZE 8192
#define UI_MAX_CHOICES 256
#define UI_CHOICE_LABEL_SIZE 512
#define UI_DIALOG_CONTEXT_MAX 6
#define UI_PRINT_COALESCE_MS 50
enum
{
    UI_COLOR_BODY = 1,
    UI_COLOR_HEADER,
    UI_COLOR_SELECTED,
    UI_COLOR_SUCCESS,
    UI_COLOR_WARNING,
    UI_COLOR_ERROR,
    UI_COLOR_MUTED
};

typedef struct
{
    char text[UI_LINE_SIZE];
} UiLine;

typedef struct
{
    int value;
    int line_index;
    char label[UI_CHOICE_LABEL_SIZE];
} UiChoice;

static bool ui_active = false;
static bool ui_colors = false;
static bool ui_registered_shutdown = false;
static UiLine ui_lines[UI_MAX_LINES];
static int ui_line_count = 1;
static int ui_view_start = -1;
static int ui_main_selection = 0;
static int ui_live_start = -1;
static int ui_update_depth = 0;
static bool ui_return_requested = false;
static bool ui_input_was_cancelled = false;
static bool ui_back_was_requested = false;
static bool ui_next_step_can_go_back = false;
static int ui_next_choice_minimum_rows = 0;
static char ui_step_name[128];
static char ui_step_description[256];
static bool ui_step_pending = false;
static int ui_step_output_start = -1;
static char ui_title[160] = "RK3588 现场网络配置工具";
static char ui_footer[160] = "↑↓ 选择   Enter 确认   Esc 返回上一级";
static struct timespec ui_last_render_time;
static bool ui_dialog_background_valid = false;
static bool ui_defer_output_until_dialog = false;

static int display_width(const char *text);

static int display_width(const char *text)
{
    mbstate_t state;
    const char *source = text;
    wchar_t value;
    size_t length;
    int width = 0;

    if (!text)
    {
        return 0;
    }
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

static void clip_to_columns(const char *source,
                            char *destination,
                            size_t destination_size,
                            int columns)
{
    mbstate_t state;
    size_t used = 0;
    int width = 0;

    if (!destination || destination_size == 0)
    {
        return;
    }
    destination[0] = '\0';
    if (!source || columns <= 0)
    {
        return;
    }

    memset(&state, 0, sizeof(state));
    while (*source && used + 1 < destination_size)
    {
        wchar_t value;
        size_t length = mbrtowc(&value, source, MB_CUR_MAX, &state);
        int cell_width;

        if (length == (size_t)-1 || length == (size_t)-2)
        {
            memset(&state, 0, sizeof(state));
            length = 1;
            cell_width = 1;
        }
        else if (length == 0)
        {
            break;
        }
        else
        {
            cell_width = wcwidth(value);
            if (cell_width <= 0)
            {
                cell_width = 1;
            }
        }
        if (width + cell_width > columns || used + length >= destination_size)
        {
            break;
        }
        memcpy(destination + used, source, length);
        used += length;
        source += length;
        width += cell_width;
    }
    destination[used] = '\0';
}

static void clip_tail_to_columns(const char *source,
                                 char *destination,
                                 size_t destination_size,
                                 int columns)
{
    const char *visible = source;

    if (!source)
    {
        clip_to_columns("", destination, destination_size, columns);
        return;
    }
    while (*visible && display_width(visible) > columns)
    {
        ++visible;
        while (*visible &&
               (((unsigned char)*visible & 0xc0U) == 0x80U))
        {
            ++visible;
        }
    }
    clip_to_columns(visible, destination, destination_size, columns);
}

static void reset_output(void)
{
    ui_line_count = 1;
    ui_lines[0].text[0] = '\0';
    ui_view_start = -1;
    ui_live_start = -1;
}

static void add_output_line(void)
{
    if (ui_line_count >= UI_MAX_LINES)
    {
        memmove(&ui_lines[0], &ui_lines[1],
                sizeof(ui_lines[0]) * (UI_MAX_LINES - 1));
        ui_line_count = UI_MAX_LINES - 1;
    }
    ui_lines[ui_line_count].text[0] = '\0';
    ++ui_line_count;
}

static void clear_current_line(void)
{
    ui_lines[ui_line_count - 1].text[0] = '\0';
}

static void append_byte(char value)
{
    UiLine *line = &ui_lines[ui_line_count - 1];
    size_t length = strlen(line->text);

    if (length + 1 >= sizeof(line->text))
    {
        add_output_line();
        line = &ui_lines[ui_line_count - 1];
        length = 0;
    }
    line->text[length] = value;
    line->text[length + 1] = '\0';
}

static void append_output(const char *text)
{
    size_t index = 0;

    while (text && text[index])
    {
        unsigned char value = (unsigned char)text[index];

        if (value == '\033' && text[index + 1] == '[')
        {
            size_t end = index + 2;

            while (text[end] &&
                   !((unsigned char)text[end] >= 0x40 &&
                     (unsigned char)text[end] <= 0x7e))
            {
                ++end;
            }
            if (text[end])
            {
                if (text[end] == 'K')
                {
                    clear_current_line();
                }
                else if (text[end] == 'J' &&
                         strstr(text + index, "[2J") == text + index + 1)
                {
                    reset_output();
                }
                index = end + 1;
                continue;
            }
        }
        if (value == '\r')
        {
            clear_current_line();
            ++index;
            continue;
        }
        if (value == '\n')
        {
            add_output_line();
            ++index;
            continue;
        }
        if (value == '\b')
        {
            UiLine *line = &ui_lines[ui_line_count - 1];
            size_t length = strlen(line->text);
            if (length > 0)
            {
                line->text[length - 1] = '\0';
            }
            ++index;
            continue;
        }
        if (value == '\t')
        {
            for (int space = 0; space < 4; ++space)
            {
                append_byte(' ');
            }
            ++index;
            continue;
        }
        append_byte((char)value);
        ++index;
    }
    ui_view_start = -1;
}

static bool separator_line(const char *text)
{
    bool has_separator = false;

    if (!text)
    {
        return false;
    }
    while (*text)
    {
        if (*text == '=' || *text == '-' || *text == ' ' || *text == '\t')
        {
            if (*text == '=' || *text == '-')
            {
                has_separator = true;
            }
            ++text;
            continue;
        }
        return false;
    }
    return has_separator;
}

static bool format_section_heading(const char *source,
                                   char *destination,
                                   size_t size)
{
    const char *start;
    const char *end;
    size_t length;

    if (!source || strncmp(source, "===", 3) != 0)
    {
        return false;
    }
    start = source;
    while (*start == '=' || *start == ' ')
    {
        ++start;
    }
    end = source + strlen(source);
    while (end > start && (end[-1] == '=' || end[-1] == ' '))
    {
        --end;
    }
    if (end == start)
    {
        return false;
    }
    length = (size_t)(end - start);
    if (length >= size)
    {
        length = size - 1;
    }
    memcpy(destination, start, length);
    destination[length] = '\0';
    return true;
}

static void draw_header(const char *title)
{
    char clipped[512];
    int width = COLS > 2 ? COLS - 2 : 1;
    int start;

    if (ui_colors)
    {
        attron(COLOR_PAIR(UI_COLOR_HEADER) | A_BOLD);
    }
    else
    {
        attron(A_REVERSE | A_BOLD);
    }
    mvhline(0, 0, ' ', COLS);
    clip_to_columns(title ? title : "RK3588 网络配置",
                    clipped, sizeof(clipped), width);
    start = (COLS - display_width(clipped)) / 2;
    mvaddstr(0, start > 0 ? start : 0, clipped);
    if (ui_colors)
    {
        attroff(COLOR_PAIR(UI_COLOR_HEADER) | A_BOLD);
    }
    else
    {
        attroff(A_REVERSE | A_BOLD);
    }
    if (LINES > 2)
    {
        mvhline(1, 0, ACS_HLINE, COLS);
        mvhline(LINES - 2, 0, ACS_HLINE, COLS);
    }
}

static void draw_footer(const char *footer)
{
    char clipped[512];
    int start;

    if (LINES < 2)
    {
        return;
    }
    if (ui_colors)
    {
        attron(COLOR_PAIR(UI_COLOR_MUTED) | A_DIM);
    }
    else
    {
        attron(A_REVERSE);
    }
    mvhline(LINES - 1, 0, ' ', COLS);
    clip_to_columns(footer ? footer : "", clipped, sizeof(clipped),
                    COLS > 2 ? COLS - 2 : 1);
    start = (COLS - display_width(clipped)) / 2;
    mvaddstr(LINES - 1, start > 0 ? start : 0, clipped);
    if (ui_colors)
    {
        attroff(COLOR_PAIR(UI_COLOR_MUTED) | A_DIM);
    }
    else
    {
        attroff(A_REVERSE);
    }
}

static bool mouse_left_action(mmask_t state)
{
    return (state & (BUTTON1_CLICKED |
                     BUTTON1_PRESSED |
                     BUTTON1_DOUBLE_CLICKED)) != 0;
}

static bool mouse_inside(int mouse_y,
                         int mouse_x,
                         int top,
                         int left,
                         int height,
                         int width)
{
    return mouse_y >= top && mouse_y < top + height &&
           mouse_x >= left && mouse_x < left + width;
}

static int line_attributes(const char *text)
{
    if (!text)
    {
        return A_NORMAL;
    }
    if (strstr(text, "[失败]") || strstr(text, "[拒绝]") ||
        strstr(text, "[高风险]") || strstr(text, "[严重"))
    {
        return ui_colors ? COLOR_PAIR(UI_COLOR_ERROR) | A_BOLD : A_BOLD;
    }
    if (strstr(text, "[警告]") || strstr(text, "[提醒]") ||
        strstr(text, "[重要提醒]") || strstr(text, "[安全保护]"))
    {
        return ui_colors ? COLOR_PAIR(UI_COLOR_WARNING) | A_BOLD : A_BOLD;
    }
    if (strstr(text, "[完成]") || strstr(text, "[正常]") ||
        strstr(text, "[恢复完成]"))
    {
        return ui_colors ? COLOR_PAIR(UI_COLOR_SUCCESS) | A_BOLD : A_BOLD;
    }
    return A_NORMAL;
}

static int output_height(void)
{
    return LINES > 5 ? LINES - 4 : 1;
}

static void render_output(void)
{
    int height;
    int start;
    int screen_row = 2;

    if (!ui_active || ui_update_depth > 0)
    {
        return;
    }
    ui_defer_output_until_dialog = false;
    ui_dialog_background_valid = false;
    erase();
    draw_header(ui_title);
    height = output_height();
    start = ui_view_start >= 0
                ? ui_view_start
                : ui_line_count > height ? ui_line_count - height : 0;
    if (start < 0)
    {
        start = 0;
    }
    if (start >= ui_line_count)
    {
        start = ui_line_count - 1;
    }

    for (int index = start;
         index < ui_line_count && screen_row < LINES - 2;
         ++index)
    {
        char heading[UI_LINE_SIZE];
        char clipped[UI_LINE_SIZE];
        const char *text = ui_lines[index].text;
        int attributes;

        if (separator_line(text))
        {
            continue;
        }
        if (format_section_heading(text, heading, sizeof(heading)))
        {
            char decorated[UI_LINE_SIZE + 32];
            snprintf(decorated, sizeof(decorated), "── %s ──", heading);
            clip_to_columns(decorated, clipped, sizeof(clipped),
                            COLS > 3 ? COLS - 3 : 1);
            attributes = ui_colors
                             ? COLOR_PAIR(UI_COLOR_HEADER) | A_BOLD
                             : A_BOLD;
        }
        else
        {
            clip_to_columns(text, clipped, sizeof(clipped),
                            COLS > 3 ? COLS - 3 : 1);
            attributes = line_attributes(text);
        }
        attron(attributes);
        mvaddstr(screen_row, 1, clipped);
        attroff(attributes);
        ++screen_row;
    }
    draw_footer(ui_footer);
    refresh();
}

bool terminal_ui_start(void)
{
    const char *term = getenv("TERM");

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        !term || term[0] == '\0' || strcmp(term, "dumb") == 0)
    {
        return false;
    }
    (void)setlocale(LC_ALL, "");
    if (!initscr())
    {
        return false;
    }
    /* 窗口过小时强行画弹窗会遮住按钮，自动使用纯文字界面。 */
    if (COLS < 48 || LINES < 10)
    {
        endwin();
        return false;
    }
    ui_active = true;
    ui_return_requested = false;
    ui_input_was_cancelled = false;
    ui_back_was_requested = false;
    ui_next_step_can_go_back = false;
    ui_next_choice_minimum_rows = 0;
    ui_step_name[0] = '\0';
    ui_step_description[0] = '\0';
    ui_step_pending = false;
    ui_defer_output_until_dialog = false;
    cbreak();
    noecho();
    keypad(stdscr, true);
    (void)curs_set(0);
    set_escdelay(200);
    if (has_colors())
    {
        start_color();
        init_pair(UI_COLOR_BODY, COLOR_GREEN, COLOR_BLACK);
        init_pair(UI_COLOR_HEADER, COLOR_GREEN, COLOR_BLACK);
        init_pair(UI_COLOR_SELECTED, COLOR_BLACK, COLOR_GREEN);
        init_pair(UI_COLOR_SUCCESS, COLOR_GREEN, COLOR_BLACK);
        init_pair(UI_COLOR_WARNING, COLOR_YELLOW, COLOR_BLACK);
        init_pair(UI_COLOR_ERROR, COLOR_RED, COLOR_BLACK);
        init_pair(UI_COLOR_MUTED, COLOR_GREEN, COLOR_BLACK);
        ui_colors = true;
        bkgd(COLOR_PAIR(UI_COLOR_BODY));
    }
    (void)mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    reset_output();
    if (!ui_registered_shutdown)
    {
        (void)atexit(terminal_ui_shutdown);
        ui_registered_shutdown = true;
    }
    return true;
}

void terminal_ui_shutdown(void)
{
    if (ui_active)
    {
        (void)curs_set(1);
        endwin();
        ui_active = false;
        ui_update_depth = 0;
    }
}

void terminal_ui_detach(void)
{
    ui_active = false;
}

bool terminal_ui_enabled(void)
{
    return ui_active;
}

int terminal_ui_content_width(void)
{
    if (!ui_active)
    {
        return 80;
    }
    return COLS > 3 ? COLS - 3 : 1;
}

bool terminal_ui_return_requested(void)
{
    return ui_active && ui_return_requested;
}

bool terminal_ui_consume_return_request(void)
{
    bool requested = ui_return_requested;

    ui_return_requested = false;
    return requested;
}

bool terminal_ui_input_cancelled(void)
{
    return ui_active && ui_input_was_cancelled;
}

bool terminal_ui_back_requested(void)
{
    return ui_active && ui_back_was_requested;
}

void terminal_ui_prepare_step(bool can_go_back)
{
    if (ui_active)
    {
        ui_next_step_can_go_back = can_go_back;
    }
}

void terminal_ui_set_step(const char *name, const char *description)
{
    if (!ui_active)
    {
        return;
    }
    snprintf(ui_step_name, sizeof(ui_step_name), "%s", name ? name : "");
    snprintf(ui_step_description, sizeof(ui_step_description), "%s",
             description ? description : "");
    ui_step_output_start =
        ui_line_count > 0 && ui_lines[ui_line_count - 1].text[0] == '\0'
            ? ui_line_count - 1
            : ui_line_count;
    ui_step_pending = true;
    ui_dialog_background_valid = false;
}

static void rewind_step_output(void)
{
    if (ui_step_output_start >= 0 &&
        ui_step_output_start < ui_line_count)
    {
        ui_line_count = ui_step_output_start + 1;
        ui_lines[ui_step_output_start].text[0] = '\0';
        ui_view_start = -1;
    }
    ui_dialog_background_valid = false;
}

static void prepare_dialog_step_label(void)
{
    if (!ui_step_pending)
    {
        ui_step_name[0] = '\0';
        ui_step_description[0] = '\0';
    }
    ui_step_pending = false;
}

void terminal_ui_prepare_choice_rows(int minimum_rows)
{
    if (ui_active)
    {
        ui_next_choice_minimum_rows = minimum_rows > 0
                                          ? minimum_rows
                                          : 0;
    }
}

void terminal_ui_begin_screen(const char *title)
{
    if (!ui_active)
    {
        return;
    }
    snprintf(ui_title, sizeof(ui_title), "%s",
             title && title[0] ? title : "RK3588 网络配置");
    snprintf(ui_footer, sizeof(ui_footer),
             "↑↓ 滚动   Enter 确认   Esc 返回上一级");
    ui_return_requested = false;
    ui_input_was_cancelled = false;
    ui_back_was_requested = false;
    ui_next_step_can_go_back = false;
    ui_next_choice_minimum_rows = 0;
    ui_step_name[0] = '\0';
    ui_step_description[0] = '\0';
    ui_step_pending = false;
    ui_step_output_start = -1;
    ui_defer_output_until_dialog = false;
    reset_output();
    render_output();
}

void terminal_ui_set_footer(const char *text)
{
    if (!ui_active)
    {
        return;
    }
    snprintf(ui_footer, sizeof(ui_footer), "%s", text ? text : "");
    render_output();
}

void terminal_ui_begin_update(void)
{
    if (ui_active)
    {
        ++ui_update_depth;
    }
}

void terminal_ui_end_update(void)
{
    if (!ui_active || ui_update_depth <= 0)
    {
        return;
    }
    --ui_update_depth;
    if (ui_update_depth == 0)
    {
        render_output();
    }
}

void terminal_ui_mark_live_region(void)
{
    if (!ui_active)
    {
        return;
    }
    if (ui_lines[ui_line_count - 1].text[0] != '\0')
    {
        add_output_line();
    }
    ui_live_start = ui_line_count - 1;
}

void terminal_ui_reset_live_region(void)
{
    if (!ui_active || ui_live_start < 0 || ui_live_start >= ui_line_count)
    {
        return;
    }
    ui_line_count = ui_live_start + 1;
    ui_lines[ui_live_start].text[0] = '\0';
    ui_view_start = -1;
    render_output();
}

int ui_printf(const char *format, ...)
{
    va_list args;
    va_list copy;
    char stack_buffer[UI_FORMAT_STACK_SIZE];
    char *buffer = stack_buffer;
    int written;

    va_start(args, format);
    if (!ui_active)
    {
        written = vprintf(format, args);
        va_end(args);
        return written;
    }

    va_copy(copy, args);
    written = vsnprintf(stack_buffer, sizeof(stack_buffer), format, copy);
    va_end(copy);
    if (written < 0)
    {
        va_end(args);
        return written;
    }
    if ((size_t)written >= sizeof(stack_buffer))
    {
        buffer = malloc((size_t)written + 1);
        if (!buffer)
        {
            va_end(args);
            return -1;
        }
        (void)vsnprintf(buffer, (size_t)written + 1, format, args);
    }
    va_end(args);

    append_output(buffer);
    ui_dialog_background_valid = false;
    if (!ui_defer_output_until_dialog)
    {
        struct timespec now;
        long elapsed_ms;

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms =
            (now.tv_sec - ui_last_render_time.tv_sec) * 1000L +
            (now.tv_nsec - ui_last_render_time.tv_nsec) / 1000000L;
        if ((ui_last_render_time.tv_sec == 0 &&
             ui_last_render_time.tv_nsec == 0) ||
            elapsed_ms >= UI_PRINT_COALESCE_MS)
        {
            /* 连续 printf 时合并全屏重绘，避免逐行擦屏闪烁。 */
            ui_last_render_time = now;
            render_output();
        }
    }
    if (buffer != stack_buffer)
    {
        free(buffer);
    }
    return written;
}

static void draw_button(int row,
                        int column,
                        int width,
                        const char *label,
                        bool selected)
{
    char clipped[UI_LINE_SIZE];
    int text_width;
    int start;
    int attributes = selected
                         ? ui_colors
                               ? COLOR_PAIR(UI_COLOR_SELECTED) | A_BOLD
                               : A_REVERSE | A_BOLD
                         : A_NORMAL;

    if (row < 0 || row >= LINES || column < 0 || width <= 0)
    {
        return;
    }
    clip_to_columns(label, clipped, sizeof(clipped), width - 2);
    text_width = display_width(clipped);
    start = column + (width - text_width) / 2;
    attron(attributes);
    for (int offset = 0; offset < width && column + offset < COLS; ++offset)
    {
        mvaddch(row, column + offset, ' ');
    }
    mvaddstr(row, start > column ? start : column, clipped);
    attroff(attributes);
}

static void render_main_menu(const char *const labels[],
                             int count,
                             int selected)
{
    int regular_count = count > 0 ? count - 1 : 0;
    bool two_columns = COLS >= 72 && LINES >= 17;

    ui_dialog_background_valid = false;
    erase();
    draw_header("RK3588 现场网络配置工具");
    if (LINES > 4)
    {
        int attr = ui_colors ? COLOR_PAIR(UI_COLOR_MUTED) : A_DIM;
        attron(attr);
        mvaddstr(2, 2, "选择要执行的操作");
        attroff(attr);
    }

    if (two_columns)
    {
        int rows = (regular_count + 1) / 2;
        int button_width = (COLS - 7) / 2;
        int start_row = 4;

        for (int index = 0; index < regular_count; ++index)
        {
            char label[UI_CHOICE_LABEL_SIZE];
            int row = start_row + index / 2;
            int column = 2 + (index % 2) * (button_width + 3);

            snprintf(label, sizeof(label), "%d  %s", index + 1,
                     labels[index]);
            draw_button(row, column, button_width, label,
                        index == selected);
        }
        if (count > 0)
        {
            int width = button_width;
            int column = (COLS - width) / 2;
            char label[UI_CHOICE_LABEL_SIZE];

            snprintf(label, sizeof(label), "0  %s", labels[count - 1]);
            draw_button(start_row + rows + 1, column, width, label,
                        selected == count - 1);
        }
    }
    else
    {
        int available = LINES > 7 ? LINES - 7 : 1;
        int first = selected - available / 2;
        int width = COLS > 6 ? COLS - 4 : COLS;

        if (first < 0)
        {
            first = 0;
        }
        if (first + available > count)
        {
            first = count > available ? count - available : 0;
        }
        for (int index = first;
             index < count && index < first + available;
             ++index)
        {
            char label[UI_CHOICE_LABEL_SIZE];
            int value = index == count - 1 ? 0 : index + 1;

            snprintf(label, sizeof(label), "%d  %s", value, labels[index]);
            draw_button(3 + index - first, 2, width, label,
                        index == selected);
        }
    }
    draw_footer("↑↓←→ 选择   Enter 确认   Esc/Q 退出程序");
    refresh();
}

static int main_menu_mouse_choice(int mouse_y,
                                  int mouse_x,
                                  int count,
                                  int selected)
{
    int regular_count = count > 0 ? count - 1 : 0;
    bool two_columns = COLS >= 72 && LINES >= 17;

    if (two_columns)
    {
        int rows = (regular_count + 1) / 2;
        int width = (COLS - 7) / 2;
        int row = mouse_y - 4;

        if (row >= 0 && row < rows)
        {
            if (mouse_x >= 2 && mouse_x < 2 + width)
            {
                int index = row * 2;
                return index < regular_count ? index : -1;
            }
            if (mouse_x >= 5 + width && mouse_x < 5 + width * 2)
            {
                int index = row * 2 + 1;
                return index < regular_count ? index : -1;
            }
        }
        if (mouse_y == 4 + rows + 1)
        {
            int column = (COLS - width) / 2;

            return mouse_x >= column && mouse_x < column + width
                       ? count - 1
                       : -1;
        }
    }
    else
    {
        int available = LINES > 7 ? LINES - 7 : 1;
        int first = selected - available / 2;
        int width = COLS > 6 ? COLS - 4 : COLS;
        int row = mouse_y - 3;

        if (first < 0)
        {
            first = 0;
        }
        if (first + available > count)
        {
            first = count > available ? count - available : 0;
        }
        if (row >= 0 && row < available &&
            mouse_x >= 2 && mouse_x < 2 + width)
        {
            int index = first + row;

            return index < count ? index : -1;
        }
    }
    return -1;
}

int terminal_ui_main_menu(const char *const labels[], int count)
{
    if (!ui_active || !labels || count <= 0)
    {
        return count > 0 ? count - 1 : 0;
    }
    if (ui_main_selection >= count)
    {
        ui_main_selection = 0;
    }

    for (;;)
    {
        int key;
        int regular_count = count - 1;
        bool two_columns = COLS >= 72 && LINES >= 17;

        render_main_menu(labels, count, ui_main_selection);
        key = getch();
        if (key == KEY_RESIZE)
        {
            continue;
        }
        if (key == 'q' || key == 'Q' || key == 27)
        {
            ui_main_selection = count - 1;
            return ui_main_selection;
        }
        if (key == '\n' || key == '\r' || key == KEY_ENTER)
        {
            return ui_main_selection;
        }
        if (key == KEY_MOUSE)
        {
            MEVENT event;

            if (getmouse(&event) != OK)
            {
                continue;
            }
            if (event.bstate & BUTTON4_PRESSED)
            {
                if (ui_main_selection > 0)
                {
                    --ui_main_selection;
                }
                continue;
            }
            if (event.bstate & BUTTON5_PRESSED)
            {
                if (ui_main_selection + 1 < count)
                {
                    ++ui_main_selection;
                }
                continue;
            }
            if (mouse_left_action(event.bstate))
            {
                int choice = main_menu_mouse_choice(event.y, event.x,
                                                    count,
                                                    ui_main_selection);
                if (choice >= 0)
                {
                    ui_main_selection = choice;
                    return choice;
                }
            }
            continue;
        }
        if (!two_columns)
        {
            if (key == KEY_UP)
            {
                ui_main_selection =
                    (ui_main_selection + count - 1) % count;
            }
            else if (key == KEY_DOWN)
            {
                ui_main_selection = (ui_main_selection + 1) % count;
            }
            continue;
        }
        if (key == KEY_LEFT && ui_main_selection < regular_count &&
            ui_main_selection % 2 == 1)
        {
            --ui_main_selection;
        }
        else if (key == KEY_RIGHT && ui_main_selection < regular_count &&
                 ui_main_selection % 2 == 0 &&
                 ui_main_selection + 1 < regular_count)
        {
            ++ui_main_selection;
        }
        else if (key == KEY_UP)
        {
            if (ui_main_selection == count - 1)
            {
                ui_main_selection = regular_count > 1
                                        ? regular_count - 2
                                        : 0;
            }
            else if (ui_main_selection >= 2)
            {
                ui_main_selection -= 2;
            }
        }
        else if (key == KEY_DOWN)
        {
            if (ui_main_selection < regular_count &&
                ui_main_selection + 2 < regular_count)
            {
                ui_main_selection += 2;
            }
            else
            {
                ui_main_selection = count - 1;
            }
        }
    }
}

static bool parse_numbered_line(const char *line,
                                int *value,
                                const char **label)
{
    char *end = NULL;
    long parsed;

    if (!line || !value || !label)
    {
        return false;
    }
    while (*line == ' ' || *line == '\t')
    {
        ++line;
    }
    if (*line < '0' || *line > '9')
    {
        return false;
    }
    errno = 0;
    parsed = strtol(line, &end, 10);
    if (errno != 0 || end == line || (*end != '.' && *end != ')') ||
        parsed < INT32_MIN || parsed > INT32_MAX)
    {
        return false;
    }
    ++end;
    while (*end == ' ' || *end == '\t')
    {
        ++end;
    }
    *value = (int)parsed;
    *label = end;
    return true;
}

static void render_dialog_background(void)
{
    /* “上一步”期间积累的说明与新对话框合并为一次物理刷新。 */
    ui_defer_output_until_dialog = false;
    /* 输入循环内背景不变，跳过整屏重绘，每键只更新对话框区域。 */
    if (ui_dialog_background_valid)
    {
        return;
    }
    erase();
    draw_header(ui_title);
    draw_footer("");
    /* 与对话框合并成一次物理刷新，避免每次按键先清屏再画框的闪烁。 */
    wnoutrefresh(stdscr);
    ui_dialog_background_valid = true;
}

static int collect_dialog_context(int before_line,
                                  UiLine *context,
                                  int capacity)
{
    int count = 0;

    if (!context || capacity <= 0)
    {
        return 0;
    }
    if (before_line < 0 || before_line > ui_line_count)
    {
        before_line = ui_line_count;
    }
    for (int index = before_line - 1;
         index >= 0 && count < capacity;
         --index)
    {
        int ignored_value;
        const char *ignored_label;
        const char *line = ui_lines[index].text;
        bool duplicate = false;

        if (!line[0] || separator_line(line) ||
            parse_numbered_line(line, &ignored_value, &ignored_label))
        {
            continue;
        }
        for (int saved = 0; saved < count; ++saved)
        {
            if (strcmp(context[saved].text, line) == 0)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }
        snprintf(context[count++].text, UI_LINE_SIZE, "%s", line);
    }
    for (int left = 0; left < count / 2; ++left)
    {
        UiLine temporary = context[left];

        context[left] = context[count - left - 1];
        context[count - left - 1] = temporary;
    }
    return count;
}

static void draw_dialog_context(WINDOW *dialog,
                                int width,
                                int first_row,
                                const UiLine *context,
                                int count)
{
    for (int index = 0; index < count; ++index)
    {
        char heading[UI_LINE_SIZE];
        char decorated[UI_LINE_SIZE + 32];
        char clipped[UI_LINE_SIZE];
        const char *source = context[index].text;
        int attributes;

        if (format_section_heading(source, heading, sizeof(heading)))
        {
            snprintf(decorated, sizeof(decorated), "── %s ──", heading);
            source = decorated;
            attributes = ui_colors
                             ? COLOR_PAIR(UI_COLOR_HEADER) | A_BOLD
                             : A_BOLD;
        }
        else
        {
            attributes = line_attributes(source) | A_DIM;
        }
        clip_to_columns(source, clipped, sizeof(clipped), width - 6);
        wattron(dialog, attributes);
        mvwaddstr(dialog, first_row + index, 3, clipped);
        wattroff(dialog, attributes);
    }
}

static int collect_recent_choices(int min_value,
                                  int max_value,
                                  UiChoice *choices,
                                  int capacity)
{
    bool seen[UI_MAX_CHOICES] = {false};
    int span = max_value - min_value + 1;
    int count = 0;

    if (span <= 0 || span > UI_MAX_CHOICES || capacity < span)
    {
        return 0;
    }
    for (int index = ui_line_count - 1; index >= 0 && count < span; --index)
    {
        int value;
        const char *label;

        if (parse_numbered_line(ui_lines[index].text, &value, &label) &&
            value >= min_value && value <= max_value &&
            !seen[value - min_value])
        {
            UiChoice *choice = &choices[count++];
            seen[value - min_value] = true;
            choice->value = value;
            choice->line_index = index;
            snprintf(choice->label, sizeof(choice->label), "%s", label);
        }
    }
    if (count != span)
    {
        return 0;
    }
    for (int left = 0; left < count - 1; ++left)
    {
        for (int right = left + 1; right < count; ++right)
        {
            if (choices[left].line_index > choices[right].line_index)
            {
                UiChoice temporary = choices[left];
                choices[left] = choices[right];
                choices[right] = temporary;
            }
        }
    }
    return count;
}

static void remember_choice(const UiChoice *choice);

static bool choice_returns_to_main(const UiChoice *choice)
{
    return choice && choice->value == 0 &&
           strstr(choice->label, "返回主菜单") != NULL;
}

static void accept_choice(const UiChoice *choice, bool can_go_back)
{
    bool local_exit = choice && choice->value == 0 &&
                      (strstr(choice->label, "返回") ||
                       strstr(choice->label, "取消") ||
                       strstr(choice->label, "暂不"));
    bool previous_step = local_exit && can_go_back &&
                         strstr(choice->label, "上一步") != NULL;

    ui_return_requested = choice_returns_to_main(choice);
    ui_input_was_cancelled = local_exit;
    ui_back_was_requested = previous_step;
    if (previous_step)
    {
        rewind_step_output();
    }
    remember_choice(choice);
}

static void remember_choice(const UiChoice *choice)
{
    char summary[UI_CHOICE_LABEL_SIZE + 32];

    if (!choice || strstr(choice->label, "返回") ||
        strstr(choice->label, "取消") || strstr(choice->label, "暂不"))
    {
        return;
    }
    snprintf(summary, sizeof(summary), "\n[已选择] %s\n", choice->label);
    append_output(summary);
    ui_dialog_background_valid = false;
}

typedef enum
{
    UI_CHOICE_CANCEL_INPUT,
    UI_CHOICE_CANCEL_LOCAL_RETURN,
    UI_CHOICE_CANCEL_MAIN_RETURN
} UiChoiceCancelMode;

static UiChoiceCancelMode choice_cancel_mode(const UiChoice *choices,
                                             int count)
{
    for (int index = 0; index < count; ++index)
    {
        const UiChoice *choice = &choices[index];

        if (choice->value != 0)
        {
            continue;
        }
        if (strstr(choice->label, "返回主菜单"))
        {
            return UI_CHOICE_CANCEL_MAIN_RETURN;
        }
        if (strstr(choice->label, "返回"))
        {
            return UI_CHOICE_CANCEL_LOCAL_RETURN;
        }
        if (strstr(choice->label, "取消") ||
            strstr(choice->label, "暂不"))
        {
            return UI_CHOICE_CANCEL_LOCAL_RETURN;
        }
    }
    return UI_CHOICE_CANCEL_INPUT;
}

static int cancel_choice(UiChoiceCancelMode mode, bool can_go_back)
{
    ui_input_was_cancelled = true;
    ui_back_was_requested = can_go_back;
    if (can_go_back)
    {
        ui_return_requested = false;
        rewind_step_output();
        ui_defer_output_until_dialog = true;
        return TERMINAL_UI_INPUT_CANCELLED;
    }
    ui_return_requested = mode == UI_CHOICE_CANCEL_MAIN_RETURN;
    return mode == UI_CHOICE_CANCEL_INPUT
               ? TERMINAL_UI_INPUT_CANCELLED
               : 0;
}

static void draw_dialog_prompt(WINDOW *dialog,
                               int width,
                               const char *prompt)
{
    WINDOW *prompt_window;

    if (width <= 6)
    {
        return;
    }
    prompt_window = derwin(dialog, 2, width - 4, 1, 2);
    if (!prompt_window)
    {
        return;
    }
    wattron(prompt_window, A_BOLD);
    waddstr(prompt_window, prompt ? prompt : "");
    wattroff(prompt_window, A_BOLD);
    delwin(prompt_window);
}

static void draw_dialog_step(WINDOW *dialog, int width)
{
    char label[512];
    char clipped[512];
    int attributes;

    if (!dialog || width <= 8 || ui_step_name[0] == '\0')
    {
        return;
    }
    snprintf(label, sizeof(label), "[ 当前步骤：%s%s%s ]",
             ui_step_name,
             ui_step_description[0] ? " · " : "",
             ui_step_description);
    clip_to_columns(label, clipped, sizeof(clipped), width - 6);
    attributes = ui_colors
                     ? COLOR_PAIR(UI_COLOR_HEADER) | A_BOLD
                     : A_BOLD;
    wattron(dialog, attributes);
    mvwaddstr(dialog, 0, 3, clipped);
    wattroff(dialog, attributes);
}

void terminal_ui_show_busy(const char *title, const char *description)
{
    int width;
    int height = 8;
    int start_y;
    int start_x;
    WINDOW *dialog;
    char clipped[512];

    if (!ui_active)
    {
        return;
    }
    width = COLS > 10 ? COLS - 8 : COLS;
    if (width > 84)
    {
        width = 84;
    }
    if (height > LINES)
    {
        height = LINES;
    }
    start_y = (LINES - height) / 2;
    start_x = (COLS - width) / 2;
    render_dialog_background();
    dialog = newwin(height, width, start_y, start_x);
    if (!dialog)
    {
        doupdate();
        return;
    }
    if (ui_colors)
    {
        wbkgd(dialog, COLOR_PAIR(UI_COLOR_BODY));
    }
    box(dialog, 0, 0);
    draw_dialog_step(dialog, width);
    clip_to_columns(title && title[0] ? title : "正在处理...",
                    clipped, sizeof(clipped), width - 8);
    wattron(dialog, A_BOLD);
    mvwaddstr(dialog, 2, 4, clipped);
    wattroff(dialog, A_BOLD);
    clip_to_columns(description ? description : "",
                    clipped, sizeof(clipped), width - 8);
    mvwaddstr(dialog, 4, 4, clipped);
    wattron(dialog, A_DIM);
    mvwaddstr(dialog, height - 2, 4,
              "请稍候，完成后会自动显示结果。");
    wattroff(dialog, A_DIM);
    wnoutrefresh(dialog);
    doupdate();
    delwin(dialog);

    /* 扫描结束前不渲染中间输出，下一个对话框一次性接管屏幕。 */
    ui_defer_output_until_dialog = true;
    ui_dialog_background_valid = false;
}

static int select_from_choices(const char *prompt,
                               UiChoice *choices,
                               int count,
                               bool can_go_back,
                               int minimum_rows)
{
    int selected = 0;
    int padding = minimum_rows > count ? minimum_rows - count : 0;
    bool pad_before_last = padding > 0 && choices[count - 1].value == 0;
    int display_count = count + padding;
    UiLine context[UI_DIALOG_CONTEXT_MAX];
    int context_count = collect_dialog_context(
        choices[0].line_index, context, UI_DIALOG_CONTEXT_MAX);
    UiChoiceCancelMode cancel_mode = choice_cancel_mode(choices, count);
    const char *return_button = can_go_back
        ? "[ 上一步 ]"
        : cancel_mode == UI_CHOICE_CANCEL_MAIN_RETURN
            ? "[ 返回主菜单 ]"
            : cancel_mode == UI_CHOICE_CANCEL_LOCAL_RETURN
                  ? "[ 返回上一级 ]"
                  : "[ 取消 ]";

    for (;;)
    {
        int width = COLS > 10 ? COLS - 8 : COLS;
        int height = LINES > 6 ? LINES - 4 : LINES;
        int visible_context = context_count;
        int visible;
        int first;
        int choice_start;
        int start_y;
        int start_x;
        int return_x;
        int return_width;
        int key;
        WINDOW *dialog;

        if (width > 92)
        {
            width = 92;
        }
        if (height > display_count + context_count + 6)
        {
            height = display_count + context_count + 6;
        }
        if (height < 7)
        {
            height = 7;
        }
        if (height > LINES)
        {
            height = LINES;
        }
        start_y = (LINES - height) / 2;
        start_x = (COLS - width) / 2;
        return_width = display_width(return_button);
        return_x = width - return_width - 2;
        while (visible_context > 0 &&
               height - (4 + visible_context) - 2 < 1)
        {
            --visible_context;
        }
        choice_start = 4 + visible_context;
        visible = height - choice_start - 2;
        {
            int selected_row = pad_before_last && selected == count - 1
                                   ? selected + padding
                                   : selected;

            first = selected_row - visible / 2;
        }
        if (first < 0)
        {
            first = 0;
        }
        if (first + visible > display_count)
        {
            first = display_count > visible ? display_count - visible : 0;
        }

        render_dialog_background();
        dialog = newwin(height, width, start_y, start_x);
        if (!dialog)
        {
            doupdate();
            return choices[selected].value;
        }
        if (ui_colors)
        {
            wbkgd(dialog, COLOR_PAIR(UI_COLOR_BODY));
        }
        keypad(dialog, true);
        box(dialog, 0, 0);
        draw_dialog_step(dialog, width);
        draw_dialog_prompt(dialog, width, prompt);
        draw_dialog_context(dialog, width, 3,
                            context + context_count - visible_context,
                            visible_context);
        if (choice_start - 1 > 2)
        {
            mvwhline(dialog, choice_start - 1, 2,
                     ACS_HLINE, width - 4);
        }
        for (int index = 0; index < count; ++index)
        {
            char label[UI_CHOICE_LABEL_SIZE + 32];
            char clipped[UI_CHOICE_LABEL_SIZE];
            int display_row = pad_before_last && index == count - 1
                                  ? index + padding
                                  : index;
            int attributes = index == selected
                                 ? ui_colors
                                       ? COLOR_PAIR(UI_COLOR_SELECTED) | A_BOLD
                                       : A_REVERSE | A_BOLD
                                 : A_NORMAL;

            if (display_row < first || display_row >= first + visible)
            {
                continue;
            }
            snprintf(label, sizeof(label), "%d  %s",
                     choices[index].value, choices[index].label);
            clip_to_columns(label, clipped, sizeof(clipped), width - 6);
            wattron(dialog, attributes);
            mvwhline(dialog, choice_start + display_row - first,
                     2, ' ', width - 4);
            mvwaddstr(dialog, choice_start + display_row - first, 3, clipped);
            wattroff(dialog, attributes);
        }
        for (int offset = 0; offset < padding; ++offset)
        {
            int display_row = (pad_before_last ? count - 1 : count) + offset;

            if (display_row >= first && display_row < first + visible)
            {
                wattron(dialog, A_DIM);
                mvwaddstr(dialog, choice_start + display_row - first, 3,
                          "·  未发现更多 Wi-Fi");
                wattroff(dialog, A_DIM);
            }
        }
        {
            const char *navigation_hint = can_go_back
                                              ? "↑↓/滚轮 选择  Esc 上一步"
                                              : "↑↓/滚轮 选择  Esc 返回";
            char clipped_hint[64];

            clip_to_columns(navigation_hint, clipped_hint,
                            sizeof(clipped_hint),
                            return_x > 4 ? return_x - 3 : width - 4);
            mvwaddstr(dialog, height - 2, 2, clipped_hint);
            if (return_x >= 2)
            {
                mvwaddstr(dialog, height - 2, return_x, return_button);
            }
        }
        wnoutrefresh(dialog);
        doupdate();
        key = wgetch(dialog);
        delwin(dialog);

        if (key == KEY_RESIZE)
        {
            ui_dialog_background_valid = false;
            continue;
        }
        if (key == KEY_MOUSE)
        {
            MEVENT event;

            if (getmouse(&event) != OK)
            {
                continue;
            }
            if (event.bstate & BUTTON4_PRESSED)
            {
                if (selected > 0)
                {
                    --selected;
                }
                continue;
            }
            if (event.bstate & BUTTON5_PRESSED)
            {
                if (selected + 1 < count)
                {
                    ++selected;
                }
                continue;
            }
            if (!mouse_left_action(event.bstate))
            {
                continue;
            }
            if (return_x >= 2 &&
                mouse_inside(event.y, event.x,
                             start_y + height - 2,
                             start_x + return_x,
                             1, return_width))
            {
                return cancel_choice(cancel_mode, can_go_back);
            }
            for (int index = 0; index < count; ++index)
            {
                int display_row = pad_before_last && index == count - 1
                                      ? index + padding
                                      : index;

                if (display_row < first || display_row >= first + visible)
                {
                    continue;
                }
                if (mouse_inside(event.y, event.x,
                                 start_y + choice_start + display_row - first,
                                 start_x + 2,
                                 1, width - 4))
                {
                    accept_choice(&choices[index], can_go_back);
                    return choices[index].value;
                }
            }
            continue;
        }
        if (key == KEY_UP)
        {
            selected = (selected + count - 1) % count;
        }
        else if (key == KEY_DOWN || key == '\t')
        {
            selected = (selected + 1) % count;
        }
        else if (key == '\n' || key == '\r' || key == KEY_ENTER)
        {
            accept_choice(&choices[selected], can_go_back);
            return choices[selected].value;
        }
        else if (key == 27)
        {
            return cancel_choice(cancel_mode, can_go_back);
        }
        else if (key >= '0' && key <= '9')
        {
            int value = key - '0';
            for (int index = 0; index < count; ++index)
            {
                if (choices[index].value == value)
                {
                    accept_choice(&choices[index], can_go_back);
                    return value;
                }
            }
        }
    }
}

static void remove_last_utf8_character(char *buffer, size_t *length)
{
    if (!buffer || !length || *length == 0)
    {
        return;
    }
    --(*length);
    while (*length > 0 &&
           (((unsigned char)buffer[*length] & 0xc0U) == 0x80U))
    {
        --(*length);
    }
    buffer[*length] = '\0';
}

static void masked_text(const char *source, char *destination, size_t size)
{
    mbstate_t state;
    size_t used = 0;

    if (!destination || size == 0)
    {
        return;
    }
    memset(&state, 0, sizeof(state));
    while (source && *source && used + 1 < size)
    {
        wchar_t value;
        size_t length = mbrtowc(&value, source, MB_CUR_MAX, &state);

        if (length == (size_t)-1 || length == (size_t)-2)
        {
            memset(&state, 0, sizeof(state));
            length = 1;
        }
        else if (length == 0)
        {
            break;
        }
        destination[used++] = '*';
        source += length;
    }
    destination[used] = '\0';
}

bool terminal_ui_read_text(const char *prompt,
                           char *buffer,
                           size_t size,
                           bool secret)
{
    size_t length = 0;
    mbstate_t conversion_state;
    bool can_go_back;
    UiLine context[UI_DIALOG_CONTEXT_MAX];
    int context_count;

    if (!ui_active || !buffer || size == 0)
    {
        return false;
    }
    prepare_dialog_step_label();
    can_go_back = ui_next_step_can_go_back;
    ui_next_step_can_go_back = false;
    ui_back_was_requested = false;
    buffer[0] = '\0';
    if (ui_return_requested)
    {
        return false;
    }
    ui_input_was_cancelled = false;
    memset(&conversion_state, 0, sizeof(conversion_state));
    context_count = collect_dialog_context(
        ui_line_count, context, UI_DIALOG_CONTEXT_MAX);

    for (;;)
    {
        int width = COLS > 10 ? COLS - 8 : COLS;
        int height = LINES > 6 ? LINES - 4 : LINES;
        int visible_context = context_count;
        int input_row;
        int start_y;
        int start_x;
        int confirm_x;
        int cancel_x;
        int clear_x;
        int confirm_width;
        int cancel_width;
        int clear_width;
        int result;
        wint_t input = 0;
        WINDOW *dialog;
        char display[UI_LINE_SIZE];
        char clipped[UI_LINE_SIZE];
        const char *confirm_button = "[ 确认 ]";
        const char *cancel_button = can_go_back
                                        ? "[ 上一步 ]"
                                        : "[ 取消 ]";
        const char *clear_button = "[ 清空 ]";

        if (width > 92)
        {
            width = 92;
        }
        if (height > context_count + 9)
        {
            height = context_count + 9;
        }
        if (height < 8)
        {
            height = 8;
        }
        if (height > LINES)
        {
            height = LINES;
        }
        while (visible_context > 0 &&
               4 + visible_context >= height - 2)
        {
            --visible_context;
        }
        input_row = 4 + visible_context;
        start_y = (LINES - height) / 2;
        start_x = (COLS - width) / 2;
        confirm_width = display_width(confirm_button);
        cancel_width = display_width(cancel_button);
        clear_width = display_width(clear_button);
        confirm_x = (width -
                     (confirm_width + cancel_width + clear_width + 6)) / 2;
        cancel_x = confirm_x + confirm_width + 3;
        clear_x = cancel_x + cancel_width + 3;
        render_dialog_background();
        dialog = newwin(height, width, start_y, start_x);
        if (!dialog)
        {
            doupdate();
            (void)curs_set(0);
            return false;
        }
        if (ui_colors)
        {
            wbkgd(dialog, COLOR_PAIR(UI_COLOR_BODY));
        }
        keypad(dialog, true);
        box(dialog, 0, 0);
        draw_dialog_step(dialog, width);
        draw_dialog_prompt(dialog, width, prompt);
        draw_dialog_context(dialog, width, 3,
                            context + context_count - visible_context,
                            visible_context);
        if (input_row - 1 > 2)
        {
            mvwhline(dialog, input_row - 1, 2,
                     ACS_HLINE, width - 4);
        }
        if (secret)
        {
            masked_text(buffer, display, sizeof(display));
        }
        else
        {
            snprintf(display, sizeof(display), "%s", buffer);
        }
        clip_tail_to_columns(display, clipped, sizeof(clipped), width - 6);
        mvwhline(dialog, input_row, 2, ' ', width - 4);
        mvwaddstr(dialog, input_row, 3, clipped);
        if (confirm_x >= 1)
        {
            mvwaddstr(dialog, height - 2, confirm_x, confirm_button);
            mvwaddstr(dialog, height - 2, cancel_x, cancel_button);
            mvwaddstr(dialog, height - 2, clear_x, clear_button);
        }
        else
        {
            char footer[64];

            clip_to_columns(can_go_back
                                ? "Enter 确认  Esc 上一步  Ctrl+U 清空"
                                : "Enter 确认  Esc 取消  Ctrl+U 清空",
                            footer, sizeof(footer), width - 4);
            mvwaddstr(dialog, height - 2, 2, footer);
        }
        (void)curs_set(1);
        wmove(dialog, input_row,
              3 + (display_width(clipped) < width - 6
                       ? display_width(clipped)
                       : width - 6));
        wnoutrefresh(dialog);
        doupdate();
        result = wget_wch(dialog, &input);
        delwin(dialog);

        if (result == KEY_CODE_YES && input == KEY_MOUSE)
        {
            MEVENT event;

            if (getmouse(&event) != OK ||
                !mouse_left_action(event.bstate) || confirm_x < 1)
            {
                continue;
            }
            if (mouse_inside(event.y, event.x,
                             start_y + height - 2,
                             start_x + confirm_x,
                             1, confirm_width))
            {
                result = OK;
                input = L'\n';
            }
            else if (mouse_inside(event.y, event.x,
                                  start_y + height - 2,
                                  start_x + cancel_x,
                                  1, cancel_width))
            {
                /* 鼠标“取消”只取消当前输入，不等同于全局 Esc。 */
                buffer[0] = '\0';
                ui_return_requested = false;
                ui_input_was_cancelled = true;
                ui_back_was_requested = can_go_back;
                (void)curs_set(0);
                if (can_go_back)
                {
                    rewind_step_output();
                    ui_defer_output_until_dialog = true;
                }
                else
                {
                    render_output();
                }
                return false;
            }
            else if (mouse_inside(event.y, event.x,
                                  start_y + height - 2,
                                  start_x + clear_x,
                                  1, clear_width))
            {
                result = OK;
                input = 21;
            }
            else
            {
                continue;
            }
        }
        if (result == KEY_CODE_YES && input == KEY_RESIZE)
        {
            ui_dialog_background_valid = false;
            continue;
        }
        if (result == KEY_CODE_YES &&
            (input == KEY_BACKSPACE || input == KEY_DC))
        {
            remove_last_utf8_character(buffer, &length);
            continue;
        }
        if (result != OK)
        {
            continue;
        }
        if (input == L'\n' || input == L'\r')
        {
            ui_return_requested = false;
            ui_input_was_cancelled = false;
            ui_back_was_requested = false;
            (void)curs_set(0);
            render_output();
            return true;
        }
        if (input == 27)
        {
            buffer[0] = '\0';
            ui_return_requested = false;
            ui_input_was_cancelled = true;
            ui_back_was_requested = can_go_back;
            (void)curs_set(0);
            if (can_go_back)
            {
                rewind_step_output();
                ui_defer_output_until_dialog = true;
            }
            else
            {
                render_output();
            }
            return false;
        }
        if (input == 8 || input == 127)
        {
            remove_last_utf8_character(buffer, &length);
            continue;
        }
        if (input == 21)
        {
            length = 0;
            buffer[0] = '\0';
            continue;
        }
        if (iswprint((wint_t)input))
        {
            char encoded[MB_LEN_MAX];
            size_t encoded_size = wcrtomb(encoded, (wchar_t)input,
                                          &conversion_state);

            if (encoded_size != (size_t)-1 &&
                length + encoded_size < size)
            {
                memcpy(buffer + length, encoded, encoded_size);
                length += encoded_size;
                buffer[length] = '\0';
            }
            else if (encoded_size == (size_t)-1)
            {
                memset(&conversion_state, 0, sizeof(conversion_state));
                beep();
            }
        }
    }
}

int terminal_ui_read_int(const char *prompt, int min_value, int max_value)
{
    UiChoice choices[UI_MAX_CHOICES];
    int choice_count;
    bool can_go_back = ui_next_step_can_go_back;
    int minimum_rows = ui_next_choice_minimum_rows;

    prepare_dialog_step_label();
    ui_next_step_can_go_back = false;
    ui_next_choice_minimum_rows = 0;
    ui_back_was_requested = false;

    /*
     * 只有明确选择“返回主菜单”时才保持全局返回请求。普通 Esc 和取消
     * 由当前输入控件返回给调用者，让业务调用栈自然回到上一层。
     */
    if (ui_return_requested)
    {
        return min_value <= 0 && max_value >= 0
                   ? 0
                   : TERMINAL_UI_INPUT_CANCELLED;
    }

    choice_count = collect_recent_choices(min_value, max_value,
                                           choices, UI_MAX_CHOICES);

    if (choice_count > 0)
    {
        return select_from_choices(prompt, choices, choice_count,
                                   can_go_back, minimum_rows);
    }

    for (;;)
    {
        char input[64];
        char *end = NULL;
        long value;

        terminal_ui_prepare_step(can_go_back);
        ui_step_pending = ui_step_name[0] != '\0';
        if (!terminal_ui_read_text(prompt, input, sizeof(input), false))
        {
            return TERMINAL_UI_INPUT_CANCELLED;
        }
        errno = 0;
        value = strtol(input, &end, 10);
        if (errno == 0 && end != input && *end == '\0' &&
            value >= min_value && value <= max_value)
        {
            return (int)value;
        }
        beep();
    }
}

bool terminal_ui_confirm(const char *prompt, bool default_yes)
{
    bool selected_yes = default_yes;
    bool can_go_back = ui_next_step_can_go_back;
    UiLine context[UI_DIALOG_CONTEXT_MAX];
    int context_count;

    prepare_dialog_step_label();
    ui_next_step_can_go_back = false;
    ui_back_was_requested = false;
    if (ui_return_requested)
    {
        return false;
    }
    ui_input_was_cancelled = false;
    context_count = collect_dialog_context(
        ui_line_count, context, UI_DIALOG_CONTEXT_MAX);

    for (;;)
    {
        int width = COLS > 10 ? COLS - 8 : COLS;
        int height = LINES > 6 ? LINES - 4 : LINES;
        int visible_context = context_count;
        int button_row;
        int start_y;
        int start_x;
        int confirm_x;
        int cancel_x;
        int confirm_width;
        int cancel_width;
        int back_x = -1;
        int back_width = 0;
        int key;
        WINDOW *dialog;
        int yes_attributes;
        int no_attributes;
        const char *confirm_button = "[  是  ]";
        const char *cancel_button = "[  否  ]";

        if (width > 84)
        {
            width = 84;
        }
        if (height > context_count + 8)
        {
            height = context_count + 8;
        }
        if (height < 8)
        {
            height = 8;
        }
        if (height > LINES)
        {
            height = LINES;
        }
        while (visible_context > 0 &&
               4 + visible_context >= height - 2)
        {
            --visible_context;
        }
        button_row = 4 + visible_context;
        start_y = (LINES - height) / 2;
        start_x = (COLS - width) / 2;
        confirm_x = width / 2 - 14;
        cancel_x = width / 2 + 3;
        confirm_width = display_width(confirm_button);
        cancel_width = display_width(cancel_button);
        if (can_go_back)
        {
            back_width = display_width("[ 上一步 ]");
            back_x = width - back_width - 2;
        }
        render_dialog_background();
        dialog = newwin(height, width, start_y, start_x);
        if (!dialog)
        {
            doupdate();
            return false;
        }
        if (ui_colors)
        {
            wbkgd(dialog, COLOR_PAIR(UI_COLOR_BODY));
        }
        keypad(dialog, true);
        box(dialog, 0, 0);
        draw_dialog_step(dialog, width);
        draw_dialog_prompt(dialog, width, prompt);
        draw_dialog_context(dialog, width, 3,
                            context + context_count - visible_context,
                            visible_context);
        if (button_row - 1 > 2)
        {
            mvwhline(dialog, button_row - 1, 2,
                     ACS_HLINE, width - 4);
        }
        yes_attributes = selected_yes
                             ? ui_colors
                                   ? COLOR_PAIR(UI_COLOR_SELECTED) | A_BOLD
                                   : A_REVERSE | A_BOLD
                             : A_NORMAL;
        no_attributes = !selected_yes
                            ? ui_colors
                                  ? COLOR_PAIR(UI_COLOR_SELECTED) | A_BOLD
                                  : A_REVERSE | A_BOLD
                            : A_NORMAL;
        wattron(dialog, yes_attributes);
        mvwaddstr(dialog, button_row, confirm_x, confirm_button);
        wattroff(dialog, yes_attributes);
        wattron(dialog, no_attributes);
        mvwaddstr(dialog, button_row, cancel_x, cancel_button);
        wattroff(dialog, no_attributes);
        {
            char footer[64];

            clip_to_columns(can_go_back
                                ? "←→ 选择  Enter 确认  Esc 上一步"
                                : "←→ 选择  Enter 确认  Esc 取消",
                            footer, sizeof(footer),
                            can_go_back && back_x > 4
                                ? back_x - 3
                                : width - 4);
            mvwaddstr(dialog, height - 2, 2, footer);
        }
        if (can_go_back && back_x >= 2)
        {
            mvwaddstr(dialog, height - 2, back_x, "[ 上一步 ]");
        }
        wnoutrefresh(dialog);
        doupdate();
        key = wgetch(dialog);
        delwin(dialog);

        if (key == KEY_RESIZE)
        {
            ui_dialog_background_valid = false;
            continue;
        }
        if (key == KEY_MOUSE)
        {
            MEVENT event;

            if (getmouse(&event) != OK ||
                !mouse_left_action(event.bstate))
            {
                continue;
            }
            if (mouse_inside(event.y, event.x,
                             start_y + button_row,
                             start_x + confirm_x,
                             1, confirm_width))
            {
                ui_return_requested = false;
                ui_input_was_cancelled = false;
                ui_back_was_requested = false;
                return true;
            }
            if (mouse_inside(event.y, event.x,
                             start_y + button_row,
                             start_x + cancel_x,
                             1, cancel_width))
            {
                ui_return_requested = false;
                ui_input_was_cancelled = false;
                ui_back_was_requested = false;
                return false;
            }
            if (can_go_back && back_x >= 2 &&
                mouse_inside(event.y, event.x,
                             start_y + height - 2,
                             start_x + back_x,
                             1, back_width))
            {
                ui_return_requested = false;
                ui_input_was_cancelled = true;
                ui_back_was_requested = true;
                rewind_step_output();
                ui_defer_output_until_dialog = true;
                return false;
            }
            continue;
        }
        if (key == KEY_LEFT || key == KEY_RIGHT || key == '\t')
        {
            selected_yes = !selected_yes;
        }
        else if (key == 'y' || key == 'Y')
        {
            ui_return_requested = false;
            ui_input_was_cancelled = false;
            ui_back_was_requested = false;
            return true;
        }
        else if (key == 'n' || key == 'N')
        {
            ui_return_requested = false;
            ui_input_was_cancelled = false;
            ui_back_was_requested = false;
            return false;
        }
        else if (key == 27)
        {
            ui_return_requested = false;
            ui_input_was_cancelled = true;
            ui_back_was_requested = can_go_back;
            if (can_go_back)
            {
                rewind_step_output();
                ui_defer_output_until_dialog = true;
            }
            return false;
        }
        else if (key == '\n' || key == '\r' || key == KEY_ENTER)
        {
            ui_return_requested = false;
            ui_input_was_cancelled = false;
            ui_back_was_requested = false;
            return selected_yes;
        }
    }
}

void terminal_ui_wait_for_return(void)
{
    if (!ui_active)
    {
        return;
    }
    snprintf(ui_footer, sizeof(ui_footer),
             "↑↓ PgUp/PgDn/滚轮 滚动   [ 返回主菜单 ]");
    ui_view_start = ui_line_count > output_height()
                        ? ui_line_count - output_height()
                        : 0;

    for (;;)
    {
        int key;
        int maximum = ui_line_count > output_height()
                          ? ui_line_count - output_height()
                          : 0;

        render_output();
        key = getch();
        if (key == KEY_RESIZE)
        {
            continue;
        }
        if (key == KEY_MOUSE)
        {
            MEVENT event;

            if (getmouse(&event) != OK)
            {
                continue;
            }
            if (mouse_left_action(event.bstate) && event.y == LINES - 1)
            {
                ui_view_start = -1;
                return;
            }
            if ((event.bstate & BUTTON4_PRESSED) && ui_view_start > 0)
            {
                --ui_view_start;
            }
            else if ((event.bstate & BUTTON5_PRESSED) &&
                     ui_view_start < maximum)
            {
                ++ui_view_start;
            }
            continue;
        }
        if (key == '\n' || key == '\r' || key == KEY_ENTER ||
            key == 27 || key == 'q' || key == 'Q')
        {
            ui_view_start = -1;
            return;
        }
        if (key == KEY_UP && ui_view_start > 0)
        {
            --ui_view_start;
        }
        else if (key == KEY_DOWN && ui_view_start < maximum)
        {
            ++ui_view_start;
        }
        else if (key == KEY_PPAGE)
        {
            ui_view_start -= output_height();
            if (ui_view_start < 0)
            {
                ui_view_start = 0;
            }
        }
        else if (key == KEY_NPAGE)
        {
            ui_view_start += output_height();
            if (ui_view_start > maximum)
            {
                ui_view_start = maximum;
            }
        }
        else if (key == KEY_HOME)
        {
            ui_view_start = 0;
        }
        else if (key == KEY_END)
        {
            ui_view_start = maximum;
        }
    }
}

bool terminal_ui_poll_cancel(void)
{
    int key;

    if (!ui_active)
    {
        return false;
    }
    nodelay(stdscr, true);
    key = getch();
    nodelay(stdscr, false);
    if (key == KEY_RESIZE)
    {
        render_output();
        return false;
    }
    if (key == KEY_MOUSE)
    {
        MEVENT event;

        if (getmouse(&event) == OK &&
            mouse_left_action(event.bstate) &&
            event.y == LINES - 1)
        {
            return true;
        }
        return false;
    }
    if (key == 27 || key == 'q' || key == 'Q' || key == KEY_F(10))
    {
        return true;
    }
    return false;
}
