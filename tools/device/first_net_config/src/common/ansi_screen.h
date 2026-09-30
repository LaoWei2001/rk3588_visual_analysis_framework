#ifndef FIRST_NET_CONFIG_ANSI_SCREEN_H
#define FIRST_NET_CONFIG_ANSI_SCREEN_H

#include <stdbool.h>

/*
 * 启动动画使用的极小绘图接口。名字刻意接近旧 ncurses 调用，动画算法本身
 * 因而无需和终端控制细节耦合；实现只使用 ANSI、termios、poll 和 ioctl。
 */
extern int ansi_screen_columns_value;
extern int ansi_screen_rows_value;

void ansi_screen_sync(void);
bool ansi_screen_active(void);
int ansi_screen_erase(void);
int ansi_screen_refresh(void);
int ansi_screen_attr_on(int attributes);
int ansi_screen_attr_off(int attributes);
int ansi_screen_add_char(int row, int column, int character);
int ansi_screen_add_text(int row, int column, const char *text);
int ansi_screen_get_key(void);
int ansi_screen_set_nodelay(void *window, bool enabled);
int ansi_screen_flush_input(void);
int ansi_screen_sleep(int milliseconds);
bool ansi_screen_has_colors(void);
int ansi_screen_init_pair(short pair, short foreground, short background);

#define COLS ansi_screen_columns_value
#define LINES ansi_screen_rows_value
#define stdscr ((void *)1)

#define OK 0
#define ERR (-1)
#define KEY_RESIZE 0x102

#define A_NORMAL 0
#define A_BOLD 0x01
#define A_DIM 0x02
#define COLOR_WHITE 7
#define COLOR_BLACK 0
#define COLOR_PAIRS 256
#define COLOR_PAIR(pair) ((void)(pair), 0)

#define isendwin() (!ansi_screen_active())
#define erase() ansi_screen_erase()
#define refresh() ansi_screen_refresh()
#define attron(attributes) ansi_screen_attr_on(attributes)
#define attroff(attributes) ansi_screen_attr_off(attributes)
#define mvaddch(row, column, character)                                        \
  ansi_screen_add_char((row), (column), (character))
#define mvaddstr(row, column, text)                                            \
  ansi_screen_add_text((row), (column), (text))
#define getch() ansi_screen_get_key()
#define nodelay(window, enabled) ansi_screen_set_nodelay((window), (enabled))
#define flushinp() ansi_screen_flush_input()
#define napms(milliseconds) ansi_screen_sleep(milliseconds)
#define has_colors() ansi_screen_has_colors()
#define init_pair(pair, foreground, background)                                \
  ansi_screen_init_pair((pair), (foreground), (background))

#endif
