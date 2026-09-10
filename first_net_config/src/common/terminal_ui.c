#define _XOPEN_SOURCE 700

#include "common/terminal_ui.h"

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define UI_MAX_LINES 2048
#define UI_LINE_SIZE 2048
#define UI_FORMAT_STACK_SIZE 8192
#define UI_MAX_CHOICES 256
#define UI_CHOICE_LABEL_SIZE 512
#define UI_DIALOG_CONTEXT_MAX 6
#define UI_PRINT_COALESCE_MS 50
#define UI_CELL_BYTES 8

#define ANSI_BODY "\033[0;92;40m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_GREEN "\033[92m"
#define ANSI_YELLOW "\033[93m"
#define ANSI_RED "\033[91m"
#define ANSI_SELECTED "\033[0;30;102m"

enum {
  UI_KEY_NONE = 0,
  UI_KEY_ENTER = 13,
  UI_KEY_ESCAPE = 27,
  UI_KEY_BACKSPACE = 127,
  UI_KEY_UP = 1000,
  UI_KEY_DOWN,
  UI_KEY_LEFT,
  UI_KEY_RIGHT,
  UI_KEY_PAGE_UP,
  UI_KEY_PAGE_DOWN,
  UI_KEY_HOME,
  UI_KEY_END,
  UI_KEY_DELETE,
  UI_KEY_RESIZE,
  UI_KEY_MOUSE,
  UI_KEY_F10
};

typedef struct {
  char text[UI_LINE_SIZE];
} UiLine;

typedef struct {
  int value;
  int line_index;
  char label[UI_CHOICE_LABEL_SIZE];
} UiChoice;

typedef struct {
  int key;
  unsigned char byte;
  int mouse_x;
  int mouse_y;
  int mouse_button;
  bool mouse_release;
} UiEvent;

typedef enum {
  UI_STYLE_BODY,
  UI_STYLE_BOLD,
  UI_STYLE_DIM,
  UI_STYLE_GREEN,
  UI_STYLE_YELLOW,
  UI_STYLE_RED,
  UI_STYLE_SELECTED,
  UI_STYLE_GREEN_BOLD,
  UI_STYLE_GREEN_DIM,
  UI_STYLE_YELLOW_BOLD,
  UI_STYLE_RED_BOLD
} UiStyle;

typedef struct {
  char bytes[UI_CELL_BYTES];
  unsigned char style;
  bool continuation;
} UiFrameCell;

typedef enum {
  UI_CHOICE_CANCEL_INPUT,
  UI_CHOICE_CANCEL_LOCAL_RETURN,
  UI_CHOICE_CANCEL_MAIN_RETURN
} UiChoiceCancelMode;

static bool ui_active;
static bool ui_registered_shutdown;
static bool ui_termios_saved;
static struct termios ui_original_termios;
static UiLine ui_lines[UI_MAX_LINES];
static int ui_line_count = 1;
static int ui_view_start = -1;
static int ui_main_selection;
static int ui_live_start = -1;
static int ui_update_depth;
static bool ui_return_requested;
static bool ui_input_was_cancelled;
static bool ui_back_was_requested;
static bool ui_next_step_can_go_back;
static int ui_next_choice_minimum_rows;
static char ui_step_name[128];
static char ui_step_description[256];
static bool ui_step_pending;
static int ui_step_output_start = -1;
static char ui_title[160] = "RK3588 现场网络配置工具";
static char ui_footer[160] = "↑↓ 选择   Enter 确认   Esc 返回上一级";
static struct timespec ui_last_render_time;
static bool ui_defer_output_until_dialog;
static int ui_rows = 24;
static int ui_columns = 80;
static UiFrameCell *ui_frame;
static UiFrameCell *ui_previous_frame;
static int ui_frame_rows;
static int ui_frame_columns;
static bool ui_frame_building;
static bool ui_previous_frame_valid;
static bool ui_serial_terminal;
static bool ui_cursor_visible;
static bool ui_frame_cursor_visible;
static int ui_frame_cursor_row;
static int ui_frame_cursor_column;
static volatile sig_atomic_t ui_resize_pending;
static struct sigaction ui_old_winch_action;
static bool ui_winch_action_saved;
static const int ui_exit_signals[] = {SIGINT, SIGTERM, SIGHUP};
static struct sigaction ui_old_exit_actions[3];
static bool ui_exit_actions_saved[3];

static void handle_winch(int signal_number) {
  (void)signal_number;
  ui_resize_pending = 1;
}

static void handle_exit_signal(int signal_number) {
  static const char restore_sequence[] =
      "\033[?1006l\033[?1000l\033[?25h\033[0m\033[?1049l";

  if (ui_active) {
    ssize_t restore_result =
        write(STDOUT_FILENO, restore_sequence, sizeof(restore_sequence) - 1);
    (void)restore_result;
    if (ui_termios_saved) {
      (void)tcsetattr(STDIN_FILENO, TCSANOW, &ui_original_termios);
    }
  }
  _exit(128 + signal_number);
}

static void update_terminal_size(void) {
  struct winsize size;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
    if (size.ws_row > 0) {
      ui_rows = size.ws_row;
    }
    if (size.ws_col > 0) {
      ui_columns = size.ws_col;
    }
  }
}

static bool tty_name_has_prefix(const char *name, const char *prefix) {
  return name && prefix && strncmp(name, prefix, strlen(prefix)) == 0;
}

static bool detect_serial_terminal(void) {
  const char *override = getenv("FIRST_NET_CONFIG_SERIAL");
  const char *name;

  if (override && override[0]) {
    return strcmp(override, "0") != 0 && strcasecmp(override, "false") != 0 &&
           strcasecmp(override, "no") != 0;
  }
  name = ttyname(STDIN_FILENO);
  return tty_name_has_prefix(name, "/dev/ttyS") ||
         tty_name_has_prefix(name, "/dev/ttyUSB") ||
         tty_name_has_prefix(name, "/dev/ttyACM") ||
         tty_name_has_prefix(name, "/dev/ttyAMA") ||
         tty_name_has_prefix(name, "/dev/ttyFIQ") ||
         tty_name_has_prefix(name, "/dev/ttymxc") ||
         tty_name_has_prefix(name, "/dev/ttySC") ||
         tty_name_has_prefix(name, "/dev/ttyLP");
}

static int display_width(const char *text) {
  mbstate_t state;
  const char *source = text;
  int width = 0;

  if (!text) {
    return 0;
  }
  memset(&state, 0, sizeof(state));
  while (*source) {
    wchar_t value;
    size_t length = mbrtowc(&value, source, MB_CUR_MAX, &state);
    int cell_width;

    if (length == (size_t)-1 || length == (size_t)-2) {
      memset(&state, 0, sizeof(state));
      ++source;
      ++width;
      continue;
    }
    if (length == 0) {
      break;
    }
    cell_width = wcwidth(value);
    width += cell_width > 0 ? cell_width : 1;
    source += length;
  }
  return width;
}

static void clip_to_columns(const char *source, char *destination,
                            size_t destination_size, int columns) {
  mbstate_t state;
  size_t used = 0;
  int width = 0;

  if (!destination || destination_size == 0) {
    return;
  }
  destination[0] = '\0';
  if (!source || columns <= 0) {
    return;
  }
  memset(&state, 0, sizeof(state));
  while (*source && used + 1 < destination_size) {
    wchar_t value;
    size_t length = mbrtowc(&value, source, MB_CUR_MAX, &state);
    int cell_width;

    if (length == (size_t)-1 || length == (size_t)-2) {
      memset(&state, 0, sizeof(state));
      length = 1;
      cell_width = 1;
    } else if (length == 0) {
      break;
    } else {
      cell_width = wcwidth(value);
      if (cell_width <= 0) {
        cell_width = 1;
      }
    }
    if (width + cell_width > columns || used + length >= destination_size) {
      break;
    }
    memcpy(destination + used, source, length);
    used += length;
    source += length;
    width += cell_width;
  }
  destination[used] = '\0';
}

static void clip_tail_to_columns(const char *source, char *destination,
                                 size_t destination_size, int columns) {
  const char *visible = source ? source : "";

  while (*visible && display_width(visible) > columns) {
    ++visible;
    while (*visible && (((unsigned char)*visible & 0xc0U) == 0x80U)) {
      ++visible;
    }
  }
  clip_to_columns(visible, destination, destination_size, columns);
}

static UiStyle style_from_sequence(const char *style) {
  if (!style || strcmp(style, ANSI_BODY) == 0)
    return UI_STYLE_BODY;
  if (strcmp(style, ANSI_BOLD) == 0)
    return UI_STYLE_BOLD;
  if (strcmp(style, ANSI_DIM) == 0)
    return UI_STYLE_DIM;
  if (strcmp(style, ANSI_GREEN) == 0)
    return UI_STYLE_GREEN;
  if (strcmp(style, ANSI_YELLOW) == 0)
    return UI_STYLE_YELLOW;
  if (strcmp(style, ANSI_RED) == 0)
    return UI_STYLE_RED;
  if (strcmp(style, ANSI_SELECTED) == 0)
    return UI_STYLE_SELECTED;
  if (strcmp(style, ANSI_GREEN ANSI_BOLD) == 0)
    return UI_STYLE_GREEN_BOLD;
  if (strcmp(style, ANSI_GREEN ANSI_DIM) == 0)
    return UI_STYLE_GREEN_DIM;
  if (strcmp(style, ANSI_YELLOW ANSI_BOLD) == 0)
    return UI_STYLE_YELLOW_BOLD;
  if (strcmp(style, ANSI_RED ANSI_BOLD) == 0)
    return UI_STYLE_RED_BOLD;
  return UI_STYLE_BODY;
}

static void emit_style(UiStyle style) {
  fputs(ANSI_BODY, stdout);
  switch (style) {
  case UI_STYLE_BOLD:
    fputs(ANSI_BOLD, stdout);
    break;
  case UI_STYLE_DIM:
    fputs(ANSI_DIM, stdout);
    break;
  case UI_STYLE_GREEN:
    fputs(ANSI_GREEN, stdout);
    break;
  case UI_STYLE_YELLOW:
    fputs(ANSI_YELLOW, stdout);
    break;
  case UI_STYLE_RED:
    fputs(ANSI_RED, stdout);
    break;
  case UI_STYLE_SELECTED:
    fputs(ANSI_SELECTED, stdout);
    break;
  case UI_STYLE_GREEN_BOLD:
    fputs(ANSI_GREEN ANSI_BOLD, stdout);
    break;
  case UI_STYLE_GREEN_DIM:
    fputs(ANSI_GREEN ANSI_DIM, stdout);
    break;
  case UI_STYLE_YELLOW_BOLD:
    fputs(ANSI_YELLOW ANSI_BOLD, stdout);
    break;
  case UI_STYLE_RED_BOLD:
    fputs(ANSI_RED ANSI_BOLD, stdout);
    break;
  case UI_STYLE_BODY:
    break;
  }
}

static bool prepare_frame_buffers(void) {
  UiFrameCell *frame;
  UiFrameCell *previous;
  size_t count;

  if (ui_frame && ui_previous_frame && ui_frame_rows == ui_rows &&
      ui_frame_columns == ui_columns) {
    return true;
  }
  count = (size_t)ui_rows * (size_t)ui_columns;
  frame = calloc(count, sizeof(*frame));
  previous = calloc(count, sizeof(*previous));
  if (!frame || !previous) {
    free(frame);
    free(previous);
    return false;
  }
  free(ui_frame);
  free(ui_previous_frame);
  ui_frame = frame;
  ui_previous_frame = previous;
  ui_frame_rows = ui_rows;
  ui_frame_columns = ui_columns;
  ui_previous_frame_valid = false;
  return true;
}

static UiFrameCell *frame_cell(int row, int column) {
  if (!ui_frame || row < 0 || row >= ui_rows || column < 0 ||
      column >= ui_columns) {
    return NULL;
  }
  return &ui_frame[row * ui_columns + column];
}

static void frame_put_text(int row, int column, const char *text,
                           UiStyle style, int maximum_columns) {
  mbstate_t state;
  int used_columns = 0;

  if (!text || row < 0 || row >= ui_rows || column < 0 ||
      column >= ui_columns || maximum_columns <= 0) {
    return;
  }
  memset(&state, 0, sizeof(state));
  while (*text && column < ui_columns && used_columns < maximum_columns) {
    wchar_t value;
    size_t length = mbrtowc(&value, text, UI_CELL_BYTES - 1, &state);
    int width;
    UiFrameCell *cell;

    if (length == (size_t)-1 || length == (size_t)-2) {
      memset(&state, 0, sizeof(state));
      length = 1;
      width = 1;
    } else if (length == 0) {
      break;
    } else {
      width = wcwidth(value);
      if (width <= 0)
        width = 1;
    }
    if (column + width > ui_columns || used_columns + width > maximum_columns) {
      break;
    }
    cell = frame_cell(row, column);
    memset(cell, 0, sizeof(*cell));
    memcpy(cell->bytes, text, length);
    cell->style = (unsigned char)style;
    for (int offset = 1; offset < width; ++offset) {
      UiFrameCell *continuation = frame_cell(row, column + offset);
      memset(continuation, 0, sizeof(*continuation));
      continuation->style = (unsigned char)style;
      continuation->continuation = true;
    }
    column += width;
    used_columns += width;
    text += length;
  }
}

static void move_cursor(int row, int column) {
  if (ui_frame_building) {
    ui_frame_cursor_row = row;
    ui_frame_cursor_column = column;
    return;
  }
  fprintf(stdout, "\033[%d;%dH", row + 1, column + 1);
}

static void fill_row(int row, const char *style) {
  if (ui_frame_building) {
    UiStyle frame_style = style_from_sequence(style);
    for (int column = 0; column < ui_columns; ++column) {
      UiFrameCell *cell = frame_cell(row, column);
      if (cell)
        cell->style = (unsigned char)frame_style;
    }
    return;
  }
  move_cursor(row, 0);
  fputs(style ? style : ANSI_BODY, stdout);
  for (int index = 0; index < ui_columns; ++index) {
    fputc(' ', stdout);
  }
  fputs(ANSI_BODY, stdout);
}

static void draw_text(int row, int column, const char *text,
                      const char *style) {
  char clipped[UI_LINE_SIZE];

  if (row < 0 || row >= ui_rows || column < 0 || column >= ui_columns) {
    return;
  }
  clip_to_columns(text ? text : "", clipped, sizeof(clipped),
                  ui_columns - column);
  if (ui_frame_building) {
    frame_put_text(row, column, clipped, style_from_sequence(style),
                   ui_columns - column);
    return;
  }
  move_cursor(row, column);
  fputs(style ? style : ANSI_BODY, stdout);
  fputs(clipped, stdout);
  fputs(ANSI_BODY, stdout);
}

static void fill_span(int row, int column, int width, const char *style) {
  if (row < 0 || row >= ui_rows || column < 0 || column >= ui_columns ||
      width <= 0) {
    return;
  }
  if (column + width > ui_columns) {
    width = ui_columns - column;
  }
  if (ui_frame_building) {
    UiStyle frame_style = style_from_sequence(style);
    for (int index = 0; index < width; ++index) {
      UiFrameCell *cell = frame_cell(row, column + index);
      if (cell) {
        memset(cell, 0, sizeof(*cell));
        cell->style = (unsigned char)frame_style;
      }
    }
    return;
  }
  move_cursor(row, column);
  fputs(style ? style : ANSI_BODY, stdout);
  for (int index = 0; index < width; ++index) {
    fputc(' ', stdout);
  }
  fputs(ANSI_BODY, stdout);
}

static void draw_repeat(int row, int column, int count, const char *glyph,
                        const char *style) {
  if (row < 0 || row >= ui_rows || column < 0 || column >= ui_columns ||
      count <= 0 || !glyph) {
    return;
  }
  if (column + count > ui_columns) {
    count = ui_columns - column;
  }
  if (ui_frame_building) {
    UiStyle frame_style = style_from_sequence(style);
    int glyph_width = display_width(glyph);
    int used = 0;

    if (glyph_width <= 0)
      return;
    while (used + glyph_width <= count) {
      frame_put_text(row, column + used, glyph, frame_style, glyph_width);
      used += glyph_width;
    }
    return;
  }
  move_cursor(row, column);
  fputs(style ? style : ANSI_BODY, stdout);
  for (int index = 0; index < count; ++index) {
    fputs(glyph, stdout);
  }
  fputs(ANSI_BODY, stdout);
}

static void draw_button(int row, int column, int width, const char *label,
                        bool selected) {
  char clipped[UI_LINE_SIZE];
  int text_width;
  int start;
  const char *style = selected ? ANSI_SELECTED : NULL;

  if (row < 0 || row >= ui_rows || column < 0 || width <= 0) {
    return;
  }
  if (column + width > ui_columns) {
    width = ui_columns - column;
  }
  clip_to_columns(label ? label : "", clipped, sizeof(clipped), width - 2);
  text_width = display_width(clipped);
  start = column + (width - text_width) / 2;
  fill_span(row, column, width, style);
  draw_text(row, start > column ? start : column, clipped, style);
}

static void draw_box(int top, int left, int height, int width) {
  const char *border_style = ANSI_GREEN ANSI_DIM;

  if (height < 2 || width < 2) {
    return;
  }
  for (int row = 0; row < height; ++row) {
    fill_span(top + row, left, width, NULL);
  }
  draw_text(top, left, "┌", border_style);
  draw_repeat(top, left + 1, width - 2, "─", border_style);
  draw_text(top, left + width - 1, "┐", border_style);
  for (int row = 1; row < height - 1; ++row) {
    draw_text(top + row, left, "│", border_style);
    draw_text(top + row, left + width - 1, "│", border_style);
  }
  draw_text(top + height - 1, left, "└", border_style);
  draw_repeat(top + height - 1, left + 1, width - 2, "─", border_style);
  draw_text(top + height - 1, left + width - 1, "┘", border_style);
}

static void draw_separator(int row, int left, int width) {
  draw_repeat(row, left + 2, width - 4, "─", ANSI_GREEN ANSI_DIM);
}

static void begin_frame(void) {
  update_terminal_size();
  ui_frame_cursor_visible = false;
  ui_frame_cursor_row = 0;
  ui_frame_cursor_column = 0;
  if (prepare_frame_buffers()) {
    memset(ui_frame, 0,
           (size_t)ui_rows * (size_t)ui_columns * sizeof(*ui_frame));
    ui_frame_building = true;
    return;
  }

  /* 极端低内存时保留可用的整屏绘制回退。 */
  ui_frame_building = false;
  fputs("\033[?25l" ANSI_BODY "\033[H", stdout);
  for (int row = 0; row < ui_rows; ++row) {
    fprintf(stdout, "\033[%d;1H\033[2K", row + 1);
  }
}

static bool frame_cells_equal(const UiFrameCell *left,
                              const UiFrameCell *right) {
  return left->style == right->style &&
         left->continuation == right->continuation &&
         memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static void set_frame_cursor_visible(bool visible) {
  if (ui_frame_building) {
    ui_frame_cursor_visible = visible;
    return;
  }
  fputs(visible ? "\033[?25h" : "\033[?25l", stdout);
  ui_cursor_visible = visible;
}

static void end_frame(void) {
  if (ui_frame_building) {
    size_t frame_size =
        (size_t)ui_rows * (size_t)ui_columns * sizeof(*ui_frame);

    ui_frame_building = false;
    if (!ui_previous_frame_valid) {
      fputs("\033[?25l" ANSI_BODY "\033[2J\033[H", stdout);
      ui_cursor_visible = false;
    }
    for (int row = 0; row < ui_rows; ++row) {
      int first = -1;
      int last = -1;

      for (int column = 0; column < ui_columns; ++column) {
        const UiFrameCell *cell = &ui_frame[row * ui_columns + column];
        const UiFrameCell *previous =
            &ui_previous_frame[row * ui_columns + column];

        if (!frame_cells_equal(cell, previous)) {
          if (first < 0)
            first = column;
          last = column;
        }
      }
      if (first < 0)
        continue;
      while (first > 0 &&
             (ui_frame[row * ui_columns + first].continuation ||
              ui_previous_frame[row * ui_columns + first].continuation)) {
        --first;
      }
      while (last + 1 < ui_columns &&
             (ui_frame[row * ui_columns + last + 1].continuation ||
              ui_previous_frame[row * ui_columns + last + 1].continuation)) {
        ++last;
      }

      fprintf(stdout, "\033[%d;%dH", row + 1, first + 1);
      UiStyle active_style = (UiStyle)-1;
      for (int column = first; column <= last; ++column) {
        const UiFrameCell *cell = &ui_frame[row * ui_columns + column];

        if (cell->continuation)
          continue;
        if ((UiStyle)cell->style != active_style) {
          active_style = (UiStyle)cell->style;
          emit_style(active_style);
        }
        if (cell->bytes[0])
          fputs(cell->bytes, stdout);
        else
          fputc(' ', stdout);
      }
    }
    memcpy(ui_previous_frame, ui_frame, frame_size);
    ui_previous_frame_valid = true;
    if (ui_frame_cursor_visible) {
      fprintf(stdout, "\033[%d;%dH", ui_frame_cursor_row + 1,
              ui_frame_cursor_column + 1);
      if (!ui_cursor_visible)
        fputs("\033[?25h", stdout);
      ui_cursor_visible = true;
    } else {
      if (ui_cursor_visible)
        fputs("\033[?25l", stdout);
      ui_cursor_visible = false;
    }
  }
  fputs(ANSI_BODY, stdout);
  fflush(stdout);
}

static const char *line_style(const char *text) {
  if (!text) {
    return NULL;
  }
  if (strstr(text, "[失败]") || strstr(text, "[拒绝]") ||
      strstr(text, "[高风险]") || strstr(text, "[严重")) {
    return ANSI_RED ANSI_BOLD;
  }
  if (strstr(text, "[警告]") || strstr(text, "[提醒]") ||
      strstr(text, "[重要提醒]") || strstr(text, "[安全保护]")) {
    return ANSI_YELLOW ANSI_BOLD;
  }
  if (strstr(text, "[完成]") || strstr(text, "[正常]") ||
      strstr(text, "[恢复完成]")) {
    return ANSI_GREEN ANSI_BOLD;
  }
  return NULL;
}

static bool separator_line(const char *text) {
  bool has_separator = false;

  if (!text) {
    return false;
  }
  while (*text) {
    if (*text != '=' && *text != '-' && *text != ' ' && *text != '\t') {
      return false;
    }
    if (*text == '=' || *text == '-') {
      has_separator = true;
    }
    ++text;
  }
  return has_separator;
}

static bool format_section_heading(const char *source, char *destination,
                                   size_t size) {
  const char *start;
  const char *end;
  size_t length;

  if (!source || strncmp(source, "===", 3) != 0 || size == 0) {
    return false;
  }
  start = source;
  while (*start == '=' || *start == ' ') {
    ++start;
  }
  end = source + strlen(source);
  while (end > start && (end[-1] == '=' || end[-1] == ' ')) {
    --end;
  }
  if (end == start) {
    return false;
  }
  length = (size_t)(end - start);
  if (length >= size) {
    length = size - 1;
  }
  memcpy(destination, start, length);
  destination[length] = '\0';
  return true;
}

static void draw_header_and_footer_with_title(const char *title,
                                              const char *footer) {
  char clipped[512];
  int column;

  fill_row(0, ANSI_GREEN ANSI_BOLD);
  clip_to_columns(title ? title : "RK3588 网络配置", clipped, sizeof(clipped),
                  ui_columns - 2);
  column = (ui_columns - display_width(clipped)) / 2;
  draw_text(0, column > 0 ? column : 0, clipped, ANSI_GREEN ANSI_BOLD);
  if (ui_rows > 2) {
    /* 根据实时窗口宽度绘制，避免宽终端右侧留下缺口。 */
    draw_repeat(1, 0, ui_columns, "─", ANSI_GREEN ANSI_DIM);
  }
  if (ui_rows > 3) {
    draw_repeat(ui_rows - 2, 0, ui_columns, "─", ANSI_GREEN ANSI_DIM);
  }
  fill_row(ui_rows - 1, ANSI_DIM);
  clip_to_columns(footer ? footer : "", clipped, sizeof(clipped),
                  ui_columns - 2);
  column = (ui_columns - display_width(clipped)) / 2;
  draw_text(ui_rows - 1, column > 0 ? column : 0, clipped, ANSI_DIM);
}

static void draw_header_and_footer(const char *footer) {
  draw_header_and_footer_with_title(ui_title, footer);
}

static int output_height(void) { return ui_rows > 5 ? ui_rows - 4 : 1; }

static void reset_output(void) {
  ui_line_count = 1;
  ui_lines[0].text[0] = '\0';
  ui_view_start = -1;
  ui_live_start = -1;
}

static void add_output_line(void) {
  if (ui_line_count >= UI_MAX_LINES) {
    memmove(&ui_lines[0], &ui_lines[1],
            sizeof(ui_lines[0]) * (UI_MAX_LINES - 1));
    ui_line_count = UI_MAX_LINES - 1;
  }
  ui_lines[ui_line_count].text[0] = '\0';
  ++ui_line_count;
}

static void clear_current_line(void) {
  ui_lines[ui_line_count - 1].text[0] = '\0';
}

static void append_byte(char value) {
  UiLine *line = &ui_lines[ui_line_count - 1];
  size_t length = strlen(line->text);

  if (length + 1 >= sizeof(line->text)) {
    add_output_line();
    line = &ui_lines[ui_line_count - 1];
    length = 0;
  }
  line->text[length] = value;
  line->text[length + 1] = '\0';
}

static void append_output(const char *text) {
  size_t index = 0;

  while (text && text[index]) {
    unsigned char value = (unsigned char)text[index];

    if (value == '\033' && text[index + 1] == '[') {
      size_t end = index + 2;

      while (text[end] && !((unsigned char)text[end] >= 0x40 &&
                            (unsigned char)text[end] <= 0x7e)) {
        ++end;
      }
      if (text[end]) {
        if (text[end] == 'K') {
          clear_current_line();
        } else if (text[end] == 'J') {
          reset_output();
        }
        index = end + 1;
        continue;
      }
    }
    if (value == '\r') {
      clear_current_line();
    } else if (value == '\n') {
      add_output_line();
    } else if (value == '\b') {
      UiLine *line = &ui_lines[ui_line_count - 1];
      size_t length = strlen(line->text);
      if (length > 0) {
        line->text[length - 1] = '\0';
      }
    } else if (value == '\t') {
      for (int space = 0; space < 4; ++space) {
        append_byte(' ');
      }
    } else {
      append_byte((char)value);
    }
    ++index;
  }
  ui_view_start = -1;
}

static void render_output(void) {
  int height;
  int start;
  int screen_row = 2;

  if (!ui_active || ui_update_depth > 0) {
    return;
  }
  ui_defer_output_until_dialog = false;
  begin_frame();
  draw_header_and_footer(ui_footer);
  height = output_height();
  start = ui_view_start >= 0       ? ui_view_start
          : ui_line_count > height ? ui_line_count - height
                                   : 0;
  if (start < 0) {
    start = 0;
  }
  if (start >= ui_line_count) {
    start = ui_line_count - 1;
  }
  for (int index = start; index < ui_line_count && screen_row < ui_rows - 2;
       ++index) {
    char heading[UI_LINE_SIZE];
    char decorated[UI_LINE_SIZE];
    const char *text = ui_lines[index].text;
    const char *style = line_style(text);

    if (separator_line(text)) {
      continue;
    }
    if (format_section_heading(text, heading, sizeof(heading))) {
      snprintf(decorated, sizeof(decorated), "── %.2000s ──", heading);
      text = decorated;
      style = ANSI_GREEN ANSI_BOLD;
    }
    draw_text(screen_row++, 1, text, style);
  }
  end_frame();
}

static bool read_one_byte(unsigned char *value, int timeout_ms) {
  struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
  int result;

  do {
    result = poll(&descriptor, 1, timeout_ms);
  } while (result < 0 && errno == EINTR && !ui_resize_pending);
  if (result <= 0 || !(descriptor.revents & POLLIN)) {
    return false;
  }
  return read(STDIN_FILENO, value, 1) == 1;
}

static UiEvent read_event(int timeout_ms) {
  UiEvent event = {0};
  unsigned char first;

  if (ui_resize_pending) {
    ui_resize_pending = 0;
    update_terminal_size();
    event.key = UI_KEY_RESIZE;
    return event;
  }
  if (!read_one_byte(&first, timeout_ms)) {
    if (ui_resize_pending) {
      ui_resize_pending = 0;
      update_terminal_size();
      event.key = UI_KEY_RESIZE;
    }
    return event;
  }
  if (first != 0x1b) {
    event.byte = first;
    if (first == '\r' || first == '\n') {
      event.key = UI_KEY_ENTER;
    } else if (first == 8 || first == 127) {
      event.key = UI_KEY_BACKSPACE;
    } else {
      event.key = first;
    }
    return event;
  }
  {
    unsigned char sequence[64];
    size_t length = 0;

    while (length + 1 < sizeof(sequence) &&
           read_one_byte(&sequence[length], length == 0 ? 80 : 15)) {
      unsigned char value = sequence[length++];
      if ((value >= '@' && value <= '~') &&
          !(length == 1 && (value == '[' || value == 'O'))) {
        break;
      }
    }
    sequence[length] = '\0';
    event.key = UI_KEY_ESCAPE;
    if (length >= 2 && (sequence[0] == '[' || sequence[0] == 'O')) {
      unsigned char final = sequence[length - 1];

      if (final == 'A')
        event.key = UI_KEY_UP;
      else if (final == 'B')
        event.key = UI_KEY_DOWN;
      else if (final == 'C')
        event.key = UI_KEY_RIGHT;
      else if (final == 'D')
        event.key = UI_KEY_LEFT;
      else if (final == 'H')
        event.key = UI_KEY_HOME;
      else if (final == 'F')
        event.key = UI_KEY_END;
      else if (final == '~') {
        int number = atoi((const char *)sequence + 1);
        if (number == 3)
          event.key = UI_KEY_DELETE;
        else if (number == 5)
          event.key = UI_KEY_PAGE_UP;
        else if (number == 6)
          event.key = UI_KEY_PAGE_DOWN;
        else if (number == 21)
          event.key = UI_KEY_F10;
      } else if (sequence[0] == '[' && sequence[1] == '<') {
        int button;
        int x;
        int y;
        char action;

        if (sscanf((const char *)sequence, "[<%d;%d;%d%c", &button, &x, &y,
                   &action) == 4) {
          event.key = UI_KEY_MOUSE;
          event.mouse_button = button;
          event.mouse_x = x - 1;
          event.mouse_y = y - 1;
          event.mouse_release = action == 'm';
        }
      }
    }
  }
  return event;
}

static bool mouse_left_click(const UiEvent *event) {
  return event && event->key == UI_KEY_MOUSE && !event->mouse_release &&
         (event->mouse_button & 3) == 0;
}

bool terminal_ui_start(void) {
  const char *term = getenv("TERM");
  struct termios raw;
  struct sigaction action;

  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || !term ||
      term[0] == '\0' || strcmp(term, "dumb") == 0) {
    return false;
  }
  (void)setlocale(LC_ALL, "");
  update_terminal_size();
  ui_serial_terminal = detect_serial_terminal();
  if (ui_columns < 48 || ui_rows < 10 ||
      tcgetattr(STDIN_FILENO, &ui_original_termios) != 0) {
    return false;
  }
  raw = ui_original_termios;
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    return false;
  }
  ui_termios_saved = true;
  memset(&action, 0, sizeof(action));
  action.sa_handler = handle_winch;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGWINCH, &action, &ui_old_winch_action) == 0) {
    ui_winch_action_saved = true;
  }
  action.sa_handler = handle_exit_signal;
  for (size_t index = 0;
       index < sizeof(ui_exit_signals) / sizeof(ui_exit_signals[0]); ++index) {
    if (sigaction(ui_exit_signals[index], &action,
                  &ui_old_exit_actions[index]) == 0) {
      ui_exit_actions_saved[index] = true;
    }
  }
  ui_active = true;
  ui_return_requested = false;
  ui_input_was_cancelled = false;
  ui_back_was_requested = false;
  ui_next_step_can_go_back = false;
  ui_next_choice_minimum_rows = 0;
  ui_step_pending = false;
  ui_defer_output_until_dialog = false;
  ui_previous_frame_valid = false;
  ui_frame_building = false;
  ui_cursor_visible = false;
  memset(&ui_last_render_time, 0, sizeof(ui_last_render_time));
  reset_output();
  if (!ui_registered_shutdown) {
    (void)atexit(terminal_ui_shutdown);
    ui_registered_shutdown = true;
  }
  /* 首个差分帧会完成清屏；这里不重复清屏，避免串口启动时闪白两次。 */
  fputs("\033[?1049h\033[?25l\033[?1000h\033[?1006h", stdout);
  fflush(stdout);
  return true;
}

void terminal_ui_shutdown(void) {
  if (!ui_active) {
    return;
  }
  fputs("\033[?1006l\033[?1000l\033[?25h\033[0m\033[?1049l", stdout);
  fflush(stdout);
  if (ui_termios_saved) {
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &ui_original_termios);
    ui_termios_saved = false;
  }
  if (ui_winch_action_saved) {
    (void)sigaction(SIGWINCH, &ui_old_winch_action, NULL);
    ui_winch_action_saved = false;
  }
  for (size_t index = 0;
       index < sizeof(ui_exit_signals) / sizeof(ui_exit_signals[0]); ++index) {
    if (ui_exit_actions_saved[index]) {
      (void)sigaction(ui_exit_signals[index], &ui_old_exit_actions[index],
                      NULL);
      ui_exit_actions_saved[index] = false;
    }
  }
  ui_active = false;
  ui_update_depth = 0;
}

void terminal_ui_detach(void) {
  ui_active = false;
  ui_termios_saved = false;
  ui_winch_action_saved = false;
  for (size_t index = 0;
       index < sizeof(ui_exit_signals) / sizeof(ui_exit_signals[0]); ++index) {
    ui_exit_actions_saved[index] = false;
  }
}

bool terminal_ui_enabled(void) { return ui_active; }

bool terminal_ui_is_serial(void) { return ui_active && ui_serial_terminal; }

int terminal_ui_content_width(void) {
  return ui_active && ui_columns > 3 ? ui_columns - 3 : 80;
}

bool terminal_ui_return_requested(void) {
  return ui_active && ui_return_requested;
}

bool terminal_ui_consume_return_request(void) {
  bool requested = ui_return_requested;
  ui_return_requested = false;
  return requested;
}

bool terminal_ui_input_cancelled(void) {
  return ui_active && ui_input_was_cancelled;
}

bool terminal_ui_back_requested(void) {
  return ui_active && ui_back_was_requested;
}

void terminal_ui_prepare_step(bool can_go_back) {
  if (ui_active) {
    ui_next_step_can_go_back = can_go_back;
  }
}

void terminal_ui_set_step(const char *name, const char *description) {
  if (!ui_active) {
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
}

static void rewind_step_output(void) {
  if (ui_step_output_start >= 0 && ui_step_output_start < ui_line_count) {
    ui_line_count = ui_step_output_start + 1;
    ui_lines[ui_step_output_start].text[0] = '\0';
    ui_view_start = -1;
  }
}

static void prepare_dialog_step_label(void) {
  if (!ui_step_pending) {
    ui_step_name[0] = '\0';
    ui_step_description[0] = '\0';
  }
  ui_step_pending = false;
}

void terminal_ui_prepare_choice_rows(int minimum_rows) {
  if (ui_active) {
    ui_next_choice_minimum_rows = minimum_rows > 0 ? minimum_rows : 0;
  }
}

void terminal_ui_begin_screen(const char *title) {
  if (!ui_active) {
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

void terminal_ui_set_footer(const char *text) {
  if (ui_active) {
    snprintf(ui_footer, sizeof(ui_footer), "%s", text ? text : "");
    render_output();
  }
}

void terminal_ui_begin_update(void) {
  if (ui_active) {
    ++ui_update_depth;
  }
}

void terminal_ui_end_update(void) {
  if (ui_active && ui_update_depth > 0 && --ui_update_depth == 0) {
    render_output();
  }
}

void terminal_ui_mark_live_region(void) {
  if (!ui_active) {
    return;
  }
  if (ui_lines[ui_line_count - 1].text[0] != '\0') {
    add_output_line();
  }
  ui_live_start = ui_line_count - 1;
}

void terminal_ui_reset_live_region(void) {
  if (!ui_active || ui_live_start < 0 || ui_live_start >= ui_line_count) {
    return;
  }
  ui_line_count = ui_live_start + 1;
  ui_lines[ui_live_start].text[0] = '\0';
  ui_view_start = -1;
  render_output();
}

int ui_printf(const char *format, ...) {
  va_list args;
  va_list copy;
  char stack_buffer[UI_FORMAT_STACK_SIZE];
  char *buffer = stack_buffer;
  int written;

  va_start(args, format);
  if (!ui_active) {
    written = vprintf(format, args);
    va_end(args);
    return written;
  }
  va_copy(copy, args);
  written = vsnprintf(stack_buffer, sizeof(stack_buffer), format, copy);
  va_end(copy);
  if (written < 0) {
    va_end(args);
    return written;
  }
  if ((size_t)written >= sizeof(stack_buffer)) {
    buffer = malloc((size_t)written + 1);
    if (!buffer) {
      va_end(args);
      return -1;
    }
    (void)vsnprintf(buffer, (size_t)written + 1, format, args);
  }
  va_end(args);
  append_output(buffer);
  if (!ui_defer_output_until_dialog) {
    struct timespec now;
    long elapsed_ms;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed_ms = (now.tv_sec - ui_last_render_time.tv_sec) * 1000L +
                 (now.tv_nsec - ui_last_render_time.tv_nsec) / 1000000L;
    if ((ui_last_render_time.tv_sec == 0 && ui_last_render_time.tv_nsec == 0) ||
        elapsed_ms >= UI_PRINT_COALESCE_MS) {
      ui_last_render_time = now;
      render_output();
    }
  }
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return written;
}

static void render_main_menu(const char *const labels[], int count,
                             int selected) {
  int regular_count = count > 0 ? count - 1 : 0;
  bool two_columns = ui_columns >= 72 && ui_rows >= 17;

  begin_frame();
  draw_header_and_footer_with_title(
      "RK3588 现场网络配置工具",
      "↑↓←→ 选择   Enter/单击 确认   Esc/Q 退出程序");
  draw_text(2, 2, "选择要执行的操作", ANSI_DIM);
  if (two_columns) {
    int rows = (regular_count + 1) / 2;
    int button_width = (ui_columns - 7) / 2;
    int start_row = 4;

    for (int index = 0; index < regular_count; ++index) {
      char label[UI_CHOICE_LABEL_SIZE];
      int row = start_row + index / 2;
      int column = 2 + (index % 2) * (button_width + 3);

      snprintf(label, sizeof(label), "%d  %s", index + 1, labels[index]);
      draw_button(row, column, button_width, label, index == selected);
    }
    if (count > 0) {
      char label[UI_CHOICE_LABEL_SIZE];
      int column = (ui_columns - button_width) / 2;

      snprintf(label, sizeof(label), "0  %s", labels[count - 1]);
      draw_button(start_row + rows + 1, column, button_width, label,
                  selected == count - 1);
    }
  } else {
    int available = ui_rows > 7 ? ui_rows - 7 : 1;
    int first = selected - available / 2;
    int width = ui_columns > 6 ? ui_columns - 4 : ui_columns;

    if (first < 0)
      first = 0;
    if (first + available > count)
      first = count > available ? count - available : 0;
    for (int index = first; index < count && index < first + available;
         ++index) {
      char label[UI_CHOICE_LABEL_SIZE];
      int value = index == count - 1 ? 0 : index + 1;

      snprintf(label, sizeof(label), "%d  %s", value, labels[index]);
      draw_button(3 + index - first, 2, width, label, index == selected);
    }
  }
  end_frame();
}

static int main_menu_mouse_choice(int mouse_y, int mouse_x, int count,
                                  int selected) {
  int regular_count = count > 0 ? count - 1 : 0;
  bool two_columns = ui_columns >= 72 && ui_rows >= 17;

  if (two_columns) {
    int rows = (regular_count + 1) / 2;
    int width = (ui_columns - 7) / 2;
    int row = mouse_y - 4;

    if (row >= 0 && row < rows) {
      if (mouse_x >= 2 && mouse_x < 2 + width) {
        int index = row * 2;
        return index < regular_count ? index : -1;
      }
      if (mouse_x >= 5 + width && mouse_x < 5 + width * 2) {
        int index = row * 2 + 1;
        return index < regular_count ? index : -1;
      }
    }
    if (mouse_y == 4 + rows + 1) {
      int column = (ui_columns - width) / 2;
      return mouse_x >= column && mouse_x < column + width ? count - 1 : -1;
    }
  } else {
    int available = ui_rows > 7 ? ui_rows - 7 : 1;
    int first = selected - available / 2;
    int width = ui_columns > 6 ? ui_columns - 4 : ui_columns;
    int row = mouse_y - 3;

    if (first < 0)
      first = 0;
    if (first + available > count)
      first = count > available ? count - available : 0;
    if (row >= 0 && row < available && mouse_x >= 2 && mouse_x < 2 + width) {
      int index = first + row;
      return index < count ? index : -1;
    }
  }
  return -1;
}

int terminal_ui_main_menu(const char *const labels[], int count) {
  if (!ui_active || !labels || count <= 0) {
    return count > 0 ? count - 1 : 0;
  }
  if (ui_main_selection >= count) {
    ui_main_selection = 0;
  }
  for (;;) {
    UiEvent event;
    int regular_count = count - 1;
    bool two_columns = ui_columns >= 72 && ui_rows >= 17;

    render_main_menu(labels, count, ui_main_selection);
    event = read_event(-1);
    if (event.key == UI_KEY_RESIZE)
      continue;
    if (event.key == UI_KEY_ESCAPE || event.key == 'q' || event.key == 'Q') {
      return ui_main_selection = count - 1;
    }
    if (event.key == UI_KEY_ENTER) {
      return ui_main_selection;
    }
    if (event.key == UI_KEY_MOUSE) {
      if (event.mouse_button == 64 && ui_main_selection > 0) {
        --ui_main_selection;
      } else if (event.mouse_button == 65 && ui_main_selection + 1 < count) {
        ++ui_main_selection;
      } else if (mouse_left_click(&event)) {
        int choice = main_menu_mouse_choice(event.mouse_y, event.mouse_x, count,
                                            ui_main_selection);
        if (choice >= 0) {
          return ui_main_selection = choice;
        }
      }
      continue;
    }
    if (!two_columns) {
      if (event.key == UI_KEY_UP)
        ui_main_selection = (ui_main_selection + count - 1) % count;
      else if (event.key == UI_KEY_DOWN || event.key == '\t')
        ui_main_selection = (ui_main_selection + 1) % count;
      continue;
    }
    if (event.key == UI_KEY_LEFT && ui_main_selection < regular_count &&
        ui_main_selection % 2 == 1) {
      --ui_main_selection;
    } else if (event.key == UI_KEY_RIGHT && ui_main_selection < regular_count &&
               ui_main_selection % 2 == 0 &&
               ui_main_selection + 1 < regular_count) {
      ++ui_main_selection;
    } else if (event.key == UI_KEY_UP) {
      if (ui_main_selection == count - 1)
        ui_main_selection = regular_count > 1 ? regular_count - 2 : 0;
      else if (ui_main_selection >= 2)
        ui_main_selection -= 2;
    } else if (event.key == UI_KEY_DOWN || event.key == '\t') {
      if (ui_main_selection < regular_count &&
          ui_main_selection + 2 < regular_count)
        ui_main_selection += 2;
      else
        ui_main_selection = count - 1;
    }
  }
}

static bool parse_numbered_line(const char *line, int *value,
                                const char **label) {
  char *end = NULL;
  long parsed;

  if (!line || !value || !label) {
    return false;
  }
  while (*line == ' ' || *line == '\t')
    ++line;
  if (*line < '0' || *line > '9')
    return false;
  errno = 0;
  parsed = strtol(line, &end, 10);
  if (errno != 0 || end == line || (*end != '.' && *end != ')') ||
      parsed < INT32_MIN || parsed > INT32_MAX) {
    return false;
  }
  ++end;
  while (*end == ' ' || *end == '\t')
    ++end;
  *value = (int)parsed;
  *label = end;
  return true;
}

static int collect_recent_choices(int min_value, int max_value,
                                  UiChoice *choices, int capacity) {
  bool seen[UI_MAX_CHOICES] = {false};
  int span = max_value - min_value + 1;
  int count = 0;

  if (span <= 0 || span > UI_MAX_CHOICES || capacity < span) {
    return 0;
  }
  for (int index = ui_line_count - 1; index >= 0 && count < span; --index) {
    int value;
    const char *label;

    if (parse_numbered_line(ui_lines[index].text, &value, &label) &&
        value >= min_value && value <= max_value && !seen[value - min_value]) {
      UiChoice *choice = &choices[count++];
      seen[value - min_value] = true;
      choice->value = value;
      choice->line_index = index;
      snprintf(choice->label, sizeof(choice->label), "%s", label);
    }
  }
  if (count != span) {
    return 0;
  }
  for (int left = 0; left < count - 1; ++left) {
    for (int right = left + 1; right < count; ++right) {
      if (choices[left].line_index > choices[right].line_index) {
        UiChoice temporary = choices[left];
        choices[left] = choices[right];
        choices[right] = temporary;
      }
    }
  }
  return count;
}

static int collect_dialog_context(int before_line, UiLine *context,
                                  int capacity) {
  int count = 0;

  if (!context || capacity <= 0)
    return 0;
  if (before_line < 0 || before_line > ui_line_count)
    before_line = ui_line_count;
  for (int index = before_line - 1; index >= 0 && count < capacity; --index) {
    int ignored_value;
    const char *ignored_label;
    const char *line = ui_lines[index].text;
    bool duplicate = false;

    if (!line[0] || separator_line(line) ||
        parse_numbered_line(line, &ignored_value, &ignored_label)) {
      continue;
    }
    for (int saved = 0; saved < count; ++saved) {
      if (strcmp(context[saved].text, line) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    snprintf(context[count++].text, UI_LINE_SIZE, "%s", line);
  }
  for (int left = 0; left < count / 2; ++left) {
    UiLine temporary = context[left];
    context[left] = context[count - left - 1];
    context[count - left - 1] = temporary;
  }
  return count;
}

static void begin_dialog_frame(void) {
  ui_defer_output_until_dialog = false;
  begin_frame();
  draw_header_and_footer("");
}

static void draw_dialog_step(int top, int left, int width) {
  char label[512];
  char clipped[512];

  if (!ui_step_name[0] || width <= 8) {
    return;
  }
  snprintf(label, sizeof(label), "[ 当前步骤：%s%s%s ]", ui_step_name,
           ui_step_description[0] ? " · " : "", ui_step_description);
  clip_to_columns(label, clipped, sizeof(clipped), width - 6);
  draw_text(top, left + 3, clipped, ANSI_GREEN ANSI_BOLD);
}

static void draw_dialog_prompt(int top, int left, int width,
                               const char *prompt) {
  char first_line[UI_LINE_SIZE];
  char second_line[UI_LINE_SIZE];
  const char *source = prompt ? prompt : "";
  size_t consumed;

  clip_to_columns(source, first_line, sizeof(first_line), width - 4);
  draw_text(top + 1, left + 2, first_line, ANSI_BOLD);
  consumed = strlen(first_line);
  source += consumed;
  while (*source == ' ' || *source == '\t') {
    ++source;
  }
  if (*source) {
    clip_to_columns(source, second_line, sizeof(second_line), width - 4);
    draw_text(top + 2, left + 2, second_line, ANSI_BOLD);
  }
}

static void draw_dialog_context(int top, int left, int width, int first_row,
                                const UiLine *context, int count) {
  for (int index = 0; index < count; ++index) {
    char heading[UI_LINE_SIZE];
    char decorated[UI_LINE_SIZE];
    char clipped[UI_LINE_SIZE];
    const char *source = context[index].text;
    const char *style = ANSI_DIM;

    if (format_section_heading(source, heading, sizeof(heading))) {
      snprintf(decorated, sizeof(decorated), "── %.2000s ──", heading);
      source = decorated;
      style = ANSI_GREEN ANSI_BOLD;
    } else if (line_style(source)) {
      style = line_style(source);
    }
    clip_to_columns(source, clipped, sizeof(clipped), width - 6);
    draw_text(top + first_row + index, left + 3, clipped, style);
  }
}

static void calculate_dialog(int maximum_width, int desired_height,
                             int minimum_height, int *top, int *left,
                             int *height, int *width) {
  *width = ui_columns > 10 ? ui_columns - 8 : ui_columns;
  if (*width > maximum_width) {
    *width = maximum_width;
  }
  *height = ui_rows > 6 ? ui_rows - 4 : ui_rows;
  if (*height > desired_height) {
    *height = desired_height;
  }
  if (*height < minimum_height) {
    *height = minimum_height;
  }
  if (*height > ui_rows) {
    *height = ui_rows;
  }
  *top = (ui_rows - *height) / 2;
  *left = (ui_columns - *width) / 2;
}

static bool choice_returns_to_main(const UiChoice *choice) {
  return choice && choice->value == 0 &&
         strstr(choice->label, "返回主菜单") != NULL;
}

static void remember_choice(const UiChoice *choice) {
  char summary[UI_CHOICE_LABEL_SIZE + 32];

  if (!choice || strstr(choice->label, "返回") ||
      strstr(choice->label, "取消") || strstr(choice->label, "暂不")) {
    return;
  }
  snprintf(summary, sizeof(summary), "\n[已选择] %s\n", choice->label);
  append_output(summary);
}

static void accept_choice(const UiChoice *choice, bool can_go_back) {
  bool local_exit =
      choice && choice->value == 0 &&
      (strstr(choice->label, "返回") || strstr(choice->label, "取消") ||
       strstr(choice->label, "暂不"));
  bool previous_step =
      local_exit && can_go_back && strstr(choice->label, "上一步") != NULL;

  ui_return_requested = choice_returns_to_main(choice);
  ui_input_was_cancelled = local_exit;
  ui_back_was_requested = previous_step;
  if (previous_step)
    rewind_step_output();
  remember_choice(choice);
}

static UiChoiceCancelMode choice_cancel_mode(const UiChoice *choices,
                                             int count) {
  for (int index = 0; index < count; ++index) {
    if (choices[index].value != 0)
      continue;
    if (strstr(choices[index].label, "返回主菜单"))
      return UI_CHOICE_CANCEL_MAIN_RETURN;
    if (strstr(choices[index].label, "返回") ||
        strstr(choices[index].label, "取消") ||
        strstr(choices[index].label, "暂不"))
      return UI_CHOICE_CANCEL_LOCAL_RETURN;
  }
  return UI_CHOICE_CANCEL_INPUT;
}

static int cancel_choice(UiChoiceCancelMode mode, bool can_go_back) {
  ui_input_was_cancelled = true;
  ui_back_was_requested = can_go_back;
  if (can_go_back) {
    ui_return_requested = false;
    rewind_step_output();
    ui_defer_output_until_dialog = true;
    return TERMINAL_UI_INPUT_CANCELLED;
  }
  ui_return_requested = mode == UI_CHOICE_CANCEL_MAIN_RETURN;
  return mode == UI_CHOICE_CANCEL_INPUT ? TERMINAL_UI_INPUT_CANCELLED : 0;
}

void terminal_ui_show_busy(const char *title, const char *description) {
  int top;
  int left;
  int height;
  int width;
  char clipped[512];

  if (!ui_active)
    return;
  calculate_dialog(84, 8, 8, &top, &left, &height, &width);
  begin_dialog_frame();
  draw_box(top, left, height, width);
  draw_dialog_step(top, left, width);
  clip_to_columns(title && title[0] ? title : "正在处理...", clipped,
                  sizeof(clipped), width - 8);
  draw_text(top + 2, left + 4, clipped, ANSI_BOLD);
  clip_to_columns(description ? description : "", clipped, sizeof(clipped),
                  width - 8);
  draw_text(top + 4, left + 4, clipped, ANSI_GREEN);
  draw_text(top + height - 2, left + 4, "请稍候，完成后会自动显示结果。",
            ANSI_DIM);
  end_frame();
  ui_defer_output_until_dialog = true;
}

static int select_from_choices(const char *prompt, UiChoice *choices, int count,
                               bool can_go_back, int minimum_rows) {
  int selected = 0;
  int padding = minimum_rows > count ? minimum_rows - count : 0;
  bool pad_before_last = padding > 0 && choices[count - 1].value == 0;
  int display_count = count + padding;
  UiLine context[UI_DIALOG_CONTEXT_MAX];
  int context_count = collect_dialog_context(choices[0].line_index, context,
                                             UI_DIALOG_CONTEXT_MAX);
  UiChoiceCancelMode cancel_mode = choice_cancel_mode(choices, count);
  const char *return_button =
      can_go_back                                    ? "[ 上一步 ]"
      : cancel_mode == UI_CHOICE_CANCEL_MAIN_RETURN  ? "[ 返回主菜单 ]"
      : cancel_mode == UI_CHOICE_CANCEL_LOCAL_RETURN ? "[ 返回上一级 ]"
                                                     : "[ 取消 ]";

  for (;;) {
    int top;
    int left;
    int height;
    int width;
    int visible_context = context_count;
    int visible;
    int first;
    int choice_start;
    int return_x;
    int return_width;
    UiEvent event;

    calculate_dialog(92, display_count + context_count + 6, 7, &top, &left,
                     &height, &width);
    return_width = display_width(return_button);
    return_x = width - return_width - 2;
    while (visible_context > 0 && height - (4 + visible_context) - 2 < 1) {
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
      first = 0;
    if (first + visible > display_count)
      first = display_count > visible ? display_count - visible : 0;

    begin_dialog_frame();
    draw_box(top, left, height, width);
    draw_dialog_step(top, left, width);
    draw_dialog_prompt(top, left, width, prompt);
    draw_dialog_context(top, left, width, 3,
                        context + context_count - visible_context,
                        visible_context);
    if (choice_start - 1 > 2) {
      draw_separator(top + choice_start - 1, left, width);
    }
    for (int index = 0; index < count; ++index) {
      char label[UI_CHOICE_LABEL_SIZE + 32];
      int display_row =
          pad_before_last && index == count - 1 ? index + padding : index;

      if (display_row < first || display_row >= first + visible) {
        continue;
      }
      snprintf(label, sizeof(label), "%d  %s", choices[index].value,
               choices[index].label);
      fill_span(top + choice_start + display_row - first, left + 2, width - 4,
                index == selected ? ANSI_SELECTED : NULL);
      draw_text(top + choice_start + display_row - first, left + 3, label,
                index == selected ? ANSI_SELECTED : NULL);
    }
    for (int offset = 0; offset < padding; ++offset) {
      int display_row = (pad_before_last ? count - 1 : count) + offset;
      if (display_row >= first && display_row < first + visible) {
        draw_text(top + choice_start + display_row - first, left + 3,
                  "·  未发现更多 Wi-Fi", ANSI_DIM);
      }
    }
    {
      const char *navigation_hint =
          can_go_back ? "↑↓/滚轮 选择  Esc 上一步" : "↑↓/滚轮 选择  Esc 返回";
      char clipped_hint[64];

      clip_to_columns(navigation_hint, clipped_hint, sizeof(clipped_hint),
                      return_x > 4 ? return_x - 3 : width - 4);
      draw_text(top + height - 2, left + 2, clipped_hint, ANSI_DIM);
      if (return_x >= 2) {
        draw_text(top + height - 2, left + return_x, return_button, NULL);
      }
    }
    end_frame();
    event = read_event(-1);
    if (event.key == UI_KEY_RESIZE)
      continue;
    if (event.key == UI_KEY_UP ||
        (event.key == UI_KEY_MOUSE && event.mouse_button == 64))
      selected = (selected + count - 1) % count;
    else if (event.key == UI_KEY_DOWN || event.key == '\t' ||
             (event.key == UI_KEY_MOUSE && event.mouse_button == 65))
      selected = (selected + 1) % count;
    else if (event.key == UI_KEY_ENTER) {
      accept_choice(&choices[selected], can_go_back);
      return choices[selected].value;
    } else if (event.key == UI_KEY_ESCAPE)
      return cancel_choice(cancel_mode, can_go_back);
    else if (event.key >= '0' && event.key <= '9') {
      int value = event.key - '0';
      for (int index = 0; index < count; ++index) {
        if (choices[index].value == value) {
          accept_choice(&choices[index], can_go_back);
          return value;
        }
      }
    } else if (mouse_left_click(&event)) {
      if (return_x >= 2 && event.mouse_y == top + height - 2 &&
          event.mouse_x >= left + return_x &&
          event.mouse_x < left + return_x + return_width) {
        return cancel_choice(cancel_mode, can_go_back);
      }
      for (int index = 0; index < count; ++index) {
        int display_row =
            pad_before_last && index == count - 1 ? index + padding : index;
        if (display_row >= first && display_row < first + visible &&
            event.mouse_y == top + choice_start + display_row - first &&
            event.mouse_x >= left + 2 && event.mouse_x < left + width - 2) {
          accept_choice(&choices[index], can_go_back);
          return choices[index].value;
        }
      }
    }
  }
}

static void remove_last_utf8_character(char *buffer, size_t *length) {
  if (!buffer || !length || *length == 0)
    return;
  --(*length);
  while (*length > 0 && (((unsigned char)buffer[*length] & 0xc0U) == 0x80U))
    --(*length);
  buffer[*length] = '\0';
}

static void masked_text(const char *source, char *destination, size_t size) {
  size_t used = 0;

  if (!destination || size == 0)
    return;
  while (source && *source && used + 1 < size) {
    destination[used++] = '*';
    ++source;
    while (*source && (((unsigned char)*source & 0xc0U) == 0x80U))
      ++source;
  }
  destination[used] = '\0';
}

bool terminal_ui_read_text(const char *prompt, char *buffer, size_t size,
                           bool secret) {
  size_t length = 0;
  bool can_go_back;
  UiLine context[UI_DIALOG_CONTEXT_MAX];
  int context_count;

  if (!ui_active || !buffer || size == 0)
    return false;
  prepare_dialog_step_label();
  can_go_back = ui_next_step_can_go_back;
  ui_next_step_can_go_back = false;
  ui_back_was_requested = false;
  buffer[0] = '\0';
  if (ui_return_requested)
    return false;
  ui_input_was_cancelled = false;
  context_count =
      collect_dialog_context(ui_line_count, context, UI_DIALOG_CONTEXT_MAX);
  for (;;) {
    int top;
    int left;
    int height;
    int width;
    int visible_context = context_count;
    int input_row;
    int confirm_x;
    int cancel_x;
    int clear_x;
    int confirm_width;
    int cancel_width;
    int clear_width;
    char display[UI_LINE_SIZE];
    char clipped[UI_LINE_SIZE];
    UiEvent event;
    const char *confirm_button = "[ 确认 ]";
    const char *cancel_button = can_go_back ? "[ 上一步 ]" : "[ 取消 ]";
    const char *clear_button = "[ 清空 ]";

    calculate_dialog(92, context_count + 9, 8, &top, &left, &height, &width);
    while (visible_context > 0 && 4 + visible_context >= height - 2) {
      --visible_context;
    }
    input_row = 4 + visible_context;
    confirm_width = display_width(confirm_button);
    cancel_width = display_width(cancel_button);
    clear_width = display_width(clear_button);
    confirm_x = (width - (confirm_width + cancel_width + clear_width + 6)) / 2;
    cancel_x = confirm_x + confirm_width + 3;
    clear_x = cancel_x + cancel_width + 3;

    begin_dialog_frame();
    draw_box(top, left, height, width);
    draw_dialog_step(top, left, width);
    draw_dialog_prompt(top, left, width, prompt);
    draw_dialog_context(top, left, width, 3,
                        context + context_count - visible_context,
                        visible_context);
    if (input_row - 1 > 2) {
      draw_separator(top + input_row - 1, left, width);
    }
    if (secret)
      masked_text(buffer, display, sizeof(display));
    else
      snprintf(display, sizeof(display), "%s", buffer);
    clip_tail_to_columns(display, clipped, sizeof(clipped), width - 6);
    fill_span(top + input_row, left + 2, width - 4, NULL);
    draw_text(top + input_row, left + 3, clipped, ANSI_BOLD);
    if (confirm_x >= 1) {
      draw_text(top + height - 2, left + confirm_x, confirm_button,
                ANSI_GREEN ANSI_BOLD);
      draw_text(top + height - 2, left + cancel_x, cancel_button, NULL);
      draw_text(top + height - 2, left + clear_x, clear_button, NULL);
    } else {
      draw_text(top + height - 2, left + 2,
                can_go_back ? "Enter 确认  Esc 上一步  Ctrl+U 清空"
                            : "Enter 确认  Esc 取消  Ctrl+U 清空",
                ANSI_DIM);
    }
    move_cursor(top + input_row, left + 3 + display_width(clipped));
    set_frame_cursor_visible(true);
    end_frame();
    event = read_event(-1);
    if (event.key == UI_KEY_RESIZE)
      continue;
    if (mouse_left_click(&event) && event.mouse_y == top + height - 2 &&
        confirm_x >= 1) {
      if (event.mouse_x >= left + confirm_x &&
          event.mouse_x < left + confirm_x + confirm_width) {
        event.key = UI_KEY_ENTER;
      } else if (event.mouse_x >= left + cancel_x &&
                 event.mouse_x < left + cancel_x + cancel_width) {
        event.key = UI_KEY_ESCAPE;
      } else if (event.mouse_x >= left + clear_x &&
                 event.mouse_x < left + clear_x + clear_width) {
        event.key = 21;
      } else {
        continue;
      }
    }
    if (event.key == UI_KEY_ENTER) {
      ui_return_requested = false;
      ui_input_was_cancelled = false;
      ui_back_was_requested = false;
      render_output();
      return true;
    }
    if (event.key == UI_KEY_ESCAPE) {
      buffer[0] = '\0';
      ui_return_requested = false;
      ui_input_was_cancelled = true;
      ui_back_was_requested = can_go_back;
      if (can_go_back) {
        set_frame_cursor_visible(false);
        rewind_step_output();
        ui_defer_output_until_dialog = true;
      } else
        render_output();
      return false;
    }
    if (event.key == UI_KEY_BACKSPACE || event.key == UI_KEY_DELETE) {
      remove_last_utf8_character(buffer, &length);
      continue;
    }
    if (event.key == 21) {
      length = 0;
      buffer[0] = '\0';
      continue;
    }
    if (event.key >= 32 && event.key != 127 && length + 1 < size) {
      buffer[length++] = (char)event.byte;
      buffer[length] = '\0';
    }
  }
}

int terminal_ui_read_int(const char *prompt, int min_value, int max_value) {
  UiChoice choices[UI_MAX_CHOICES];
  int choice_count;
  bool can_go_back = ui_next_step_can_go_back;
  int minimum_rows = ui_next_choice_minimum_rows;

  prepare_dialog_step_label();
  ui_next_step_can_go_back = false;
  ui_next_choice_minimum_rows = 0;
  ui_back_was_requested = false;
  if (ui_return_requested)
    return min_value <= 0 && max_value >= 0 ? 0 : TERMINAL_UI_INPUT_CANCELLED;
  choice_count =
      collect_recent_choices(min_value, max_value, choices, UI_MAX_CHOICES);
  if (choice_count > 0)
    return select_from_choices(prompt, choices, choice_count, can_go_back,
                               minimum_rows);
  for (;;) {
    char input[64];
    char *end = NULL;
    long value;

    terminal_ui_prepare_step(can_go_back);
    ui_step_pending = ui_step_name[0] != '\0';
    if (!terminal_ui_read_text(prompt, input, sizeof(input), false))
      return TERMINAL_UI_INPUT_CANCELLED;
    errno = 0;
    value = strtol(input, &end, 10);
    if (errno == 0 && end != input && *end == '\0' && value >= min_value &&
        value <= max_value)
      return (int)value;
    fputc('\a', stdout);
    fflush(stdout);
  }
}

bool terminal_ui_confirm(const char *prompt, bool default_yes) {
  bool selected_yes = default_yes;
  bool can_go_back = ui_next_step_can_go_back;
  UiLine context[UI_DIALOG_CONTEXT_MAX];
  int context_count;

  prepare_dialog_step_label();
  ui_next_step_can_go_back = false;
  ui_back_was_requested = false;
  if (ui_return_requested)
    return false;
  ui_input_was_cancelled = false;
  context_count =
      collect_dialog_context(ui_line_count, context, UI_DIALOG_CONTEXT_MAX);
  for (;;) {
    int top;
    int left;
    int height;
    int width;
    int visible_context = context_count;
    int button_row;
    int yes_x;
    int no_x;
    int back_x = -1;
    int back_width = 0;
    UiEvent event;
    const char *yes_button = "[  是  ]";
    const char *no_button = "[  否  ]";

    calculate_dialog(84, context_count + 8, 8, &top, &left, &height, &width);
    while (visible_context > 0 && 4 + visible_context >= height - 2) {
      --visible_context;
    }
    button_row = 4 + visible_context;
    yes_x = width / 2 - 14;
    no_x = width / 2 + 3;
    if (can_go_back) {
      back_width = display_width("[ 上一步 ]");
      back_x = width - back_width - 2;
    }

    begin_dialog_frame();
    draw_box(top, left, height, width);
    draw_dialog_step(top, left, width);
    draw_dialog_prompt(top, left, width, prompt);
    draw_dialog_context(top, left, width, 3,
                        context + context_count - visible_context,
                        visible_context);
    if (button_row - 1 > 2) {
      draw_separator(top + button_row - 1, left, width);
    }
    draw_text(top + button_row, left + yes_x, yes_button,
              selected_yes ? ANSI_SELECTED : NULL);
    draw_text(top + button_row, left + no_x, no_button,
              !selected_yes ? ANSI_SELECTED : NULL);
    {
      char footer[64];
      clip_to_columns(can_go_back ? "←→ 选择  Enter 确认  Esc 上一步"
                                  : "←→ 选择  Enter 确认  Esc 取消",
                      footer, sizeof(footer),
                      can_go_back && back_x > 4 ? back_x - 3 : width - 4);
      draw_text(top + height - 2, left + 2, footer, ANSI_DIM);
    }
    if (can_go_back && back_x >= 2) {
      draw_text(top + height - 2, left + back_x, "[ 上一步 ]", NULL);
    }
    end_frame();
    event = read_event(-1);
    if (event.key == UI_KEY_RESIZE)
      continue;
    if (event.key == UI_KEY_LEFT || event.key == UI_KEY_RIGHT ||
        event.key == '\t')
      selected_yes = !selected_yes;
    else if (event.key == 'y' || event.key == 'Y') {
      ui_return_requested = false;
      ui_input_was_cancelled = false;
      ui_back_was_requested = false;
      return true;
    } else if (event.key == 'n' || event.key == 'N') {
      ui_return_requested = false;
      ui_input_was_cancelled = false;
      ui_back_was_requested = false;
      return false;
    } else if (event.key == UI_KEY_ESCAPE) {
      ui_return_requested = false;
      ui_input_was_cancelled = true;
      ui_back_was_requested = can_go_back;
      if (can_go_back) {
        rewind_step_output();
        ui_defer_output_until_dialog = true;
      }
      return false;
    } else if (event.key == UI_KEY_ENTER) {
      ui_return_requested = false;
      ui_input_was_cancelled = false;
      ui_back_was_requested = false;
      return selected_yes;
    } else if (mouse_left_click(&event) && event.mouse_y == top + button_row) {
      if (event.mouse_x >= left + yes_x &&
          event.mouse_x < left + yes_x + display_width(yes_button)) {
        ui_return_requested = false;
        ui_input_was_cancelled = false;
        ui_back_was_requested = false;
        return true;
      }
      if (event.mouse_x >= left + no_x &&
          event.mouse_x < left + no_x + display_width(no_button)) {
        ui_return_requested = false;
        ui_input_was_cancelled = false;
        ui_back_was_requested = false;
        return false;
      }
    } else if (mouse_left_click(&event) && can_go_back && back_x >= 2 &&
               event.mouse_y == top + height - 2 &&
               event.mouse_x >= left + back_x &&
               event.mouse_x < left + back_x + back_width) {
      ui_return_requested = false;
      ui_input_was_cancelled = true;
      ui_back_was_requested = true;
      rewind_step_output();
      ui_defer_output_until_dialog = true;
      return false;
    }
  }
}

void terminal_ui_wait_for_return(void) {
  if (!ui_active)
    return;
  snprintf(ui_footer, sizeof(ui_footer),
           "↑↓ PgUp/PgDn/滚轮 滚动   Enter/Esc/Q/单击底栏 返回主菜单");
  ui_view_start =
      ui_line_count > output_height() ? ui_line_count - output_height() : 0;
  for (;;) {
    int maximum =
        ui_line_count > output_height() ? ui_line_count - output_height() : 0;
    UiEvent event;

    render_output();
    event = read_event(-1);
    if (event.key == UI_KEY_RESIZE)
      continue;
    if (event.key == UI_KEY_ENTER || event.key == UI_KEY_ESCAPE ||
        event.key == 'q' || event.key == 'Q' ||
        (mouse_left_click(&event) && event.mouse_y == ui_rows - 1)) {
      ui_view_start = -1;
      return;
    }
    if ((event.key == UI_KEY_UP ||
         (event.key == UI_KEY_MOUSE && event.mouse_button == 64)) &&
        ui_view_start > 0)
      --ui_view_start;
    else if ((event.key == UI_KEY_DOWN ||
              (event.key == UI_KEY_MOUSE && event.mouse_button == 65)) &&
             ui_view_start < maximum)
      ++ui_view_start;
    else if (event.key == UI_KEY_PAGE_UP) {
      ui_view_start -= output_height();
      if (ui_view_start < 0)
        ui_view_start = 0;
    } else if (event.key == UI_KEY_PAGE_DOWN) {
      ui_view_start += output_height();
      if (ui_view_start > maximum)
        ui_view_start = maximum;
    } else if (event.key == UI_KEY_HOME)
      ui_view_start = 0;
    else if (event.key == UI_KEY_END)
      ui_view_start = maximum;
  }
}

bool terminal_ui_poll_cancel(void) {
  UiEvent event;

  if (!ui_active)
    return false;
  event = read_event(0);
  if (event.key == UI_KEY_RESIZE) {
    render_output();
    return false;
  }
  if (mouse_left_click(&event) && event.mouse_y == ui_rows - 1)
    return true;
  return event.key == UI_KEY_ESCAPE || event.key == 'q' || event.key == 'Q' ||
         event.key == UI_KEY_F10;
}
