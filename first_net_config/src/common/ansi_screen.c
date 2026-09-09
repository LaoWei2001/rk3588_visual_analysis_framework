#define _XOPEN_SOURCE 700

#include "common/ansi_screen.h"

#include "common/terminal_ui.h"

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define ANSI_CELL_BYTES 8

typedef struct {
  char bytes[ANSI_CELL_BYTES];
  unsigned char attributes;
  bool continuation;
} AnsiCell;

int ansi_screen_columns_value = 80;
int ansi_screen_rows_value = 24;

static AnsiCell *screen_cells;
static int allocated_columns;
static int allocated_rows;
static int current_attributes;
static bool input_nonblocking;
static bool size_changed;

static bool resize_cells(int rows, int columns) {
  AnsiCell *replacement;

  if (rows == allocated_rows && columns == allocated_columns && screen_cells) {
    return true;
  }
  replacement = calloc((size_t)rows * (size_t)columns, sizeof(*replacement));
  if (!replacement) {
    return false;
  }
  free(screen_cells);
  screen_cells = replacement;
  allocated_rows = rows;
  allocated_columns = columns;
  return true;
}

void ansi_screen_sync(void) {
  struct winsize size;
  int previous_rows = ansi_screen_rows_value;
  int previous_columns = ansi_screen_columns_value;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
    if (size.ws_row > 0) {
      ansi_screen_rows_value = size.ws_row;
    }
    if (size.ws_col > 0) {
      ansi_screen_columns_value = size.ws_col;
    }
  }
  if (previous_rows != ansi_screen_rows_value ||
      previous_columns != ansi_screen_columns_value) {
    size_changed = true;
  }
}

bool ansi_screen_active(void) { return terminal_ui_enabled(); }

int ansi_screen_erase(void) {
  ansi_screen_sync();
  if (!resize_cells(LINES, COLS)) {
    return ERR;
  }
  memset(screen_cells, 0, (size_t)LINES * (size_t)COLS * sizeof(*screen_cells));
  current_attributes = A_NORMAL;
  return OK;
}

int ansi_screen_attr_on(int attributes) {
  current_attributes |= attributes;
  return OK;
}

int ansi_screen_attr_off(int attributes) {
  current_attributes &= ~attributes;
  return OK;
}

int ansi_screen_add_char(int row, int column, int character) {
  AnsiCell *cell;

  if (!screen_cells || row < 0 || row >= LINES || column < 0 ||
      column >= COLS) {
    return ERR;
  }
  cell = &screen_cells[row * COLS + column];
  memset(cell, 0, sizeof(*cell));
  cell->bytes[0] = (char)character;
  cell->attributes = (unsigned char)current_attributes;
  return OK;
}

int ansi_screen_add_text(int row, int column, const char *text) {
  mbstate_t state;

  if (!screen_cells || !text || row < 0 || row >= LINES) {
    return ERR;
  }
  memset(&state, 0, sizeof(state));
  while (*text && column < COLS) {
    wchar_t value;
    size_t length = mbrtowc(&value, text, ANSI_CELL_BYTES - 1, &state);
    int width;
    AnsiCell *cell;

    if (length == (size_t)-1 || length == (size_t)-2) {
      memset(&state, 0, sizeof(state));
      length = 1;
      width = 1;
    } else if (length == 0) {
      break;
    } else {
      width = wcwidth(value);
      if (width <= 0) {
        width = 1;
      }
    }
    if (column >= 0 && column + width <= COLS) {
      cell = &screen_cells[row * COLS + column];
      memset(cell, 0, sizeof(*cell));
      memcpy(cell->bytes, text, length);
      cell->attributes = (unsigned char)current_attributes;
      for (int offset = 1; offset < width; ++offset) {
        AnsiCell *continuation = &screen_cells[row * COLS + column + offset];
        memset(continuation, 0, sizeof(*continuation));
        continuation->continuation = true;
        continuation->attributes = (unsigned char)current_attributes;
      }
    }
    column += width;
    text += length;
  }
  return OK;
}

static void apply_attributes(unsigned char attributes) {
  /* 每次切换属性都明确恢复白字黑底，避免继承终端主题背景。 */
  fputs("\033[0;37;40m", stdout);
  if (attributes & A_BOLD) {
    fputs("\033[1m", stdout);
  }
  if (attributes & A_DIM) {
    fputs("\033[2m", stdout);
  }
  fputs("\033[37m", stdout);
}

int ansi_screen_refresh(void) {
  unsigned char active_attributes = 0xffU;

  if (!screen_cells) {
    return ERR;
  }
  /* 先设置黑底再清屏，保证擦除区域和动画空白区域同样为黑色。 */
  fputs("\033[?25l\033[0;37;40m\033[H", stdout);
  for (int row = 0; row < LINES; ++row) {
    fprintf(stdout, "\033[%d;1H\033[2K", row + 1);
    active_attributes = 0xffU;
    for (int column = 0; column < COLS; ++column) {
      const AnsiCell *cell = &screen_cells[row * COLS + column];

      if (cell->continuation) {
        continue;
      }
      if (cell->attributes != active_attributes) {
        apply_attributes(cell->attributes);
        active_attributes = cell->attributes;
      }
      if (cell->bytes[0]) {
        fputs(cell->bytes, stdout);
      } else {
        fputc(' ', stdout);
      }
    }
  }
  fputs("\033[0;37;40m", stdout);
  fflush(stdout);
  return OK;
}

int ansi_screen_get_key(void) {
  struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
  unsigned char value;
  int result;

  ansi_screen_sync();
  if (size_changed) {
    size_changed = false;
    return KEY_RESIZE;
  }
  do {
    result = poll(&descriptor, 1, input_nonblocking ? 0 : -1);
  } while (result < 0 && errno == EINTR);
  if (result <= 0 || !(descriptor.revents & POLLIN) ||
      read(STDIN_FILENO, &value, 1) != 1) {
    return ERR;
  }
  return value;
}

int ansi_screen_set_nodelay(void *window, bool enabled) {
  (void)window;
  input_nonblocking = enabled;
  return OK;
}

int ansi_screen_flush_input(void) {
  return tcflush(STDIN_FILENO, TCIFLUSH) == 0 ? OK : ERR;
}

int ansi_screen_sleep(int milliseconds) {
  struct timespec requested = {milliseconds / 1000,
                               (long)(milliseconds % 1000) * 1000000L};

  while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {
  }
  return OK;
}

bool ansi_screen_has_colors(void) { return isatty(STDOUT_FILENO) != 0; }

int ansi_screen_init_pair(short pair, short foreground, short background) {
  (void)pair;
  (void)foreground;
  (void)background;
  return OK;
}
