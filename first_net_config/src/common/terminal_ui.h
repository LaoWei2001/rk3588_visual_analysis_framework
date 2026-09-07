#ifndef FIRST_NET_CONFIG_TERMINAL_UI_H
#define FIRST_NET_CONFIG_TERMINAL_UI_H

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#define TERMINAL_UI_INPUT_CANCELLED INT_MIN

bool terminal_ui_start(void);
void terminal_ui_shutdown(void);
void terminal_ui_detach(void);
bool terminal_ui_enabled(void);
int terminal_ui_content_width(void);
bool terminal_ui_return_requested(void);
bool terminal_ui_consume_return_request(void);
bool terminal_ui_input_cancelled(void);
bool terminal_ui_back_requested(void);
void terminal_ui_prepare_step(bool can_go_back);
void terminal_ui_set_step(const char *name, const char *description);
void terminal_ui_prepare_choice_rows(int minimum_rows);
void terminal_ui_show_busy(const char *title, const char *description);

void terminal_ui_begin_screen(const char *title);
void terminal_ui_set_footer(const char *text);
void terminal_ui_begin_update(void);
void terminal_ui_end_update(void);
void terminal_ui_mark_live_region(void);
void terminal_ui_reset_live_region(void);
void terminal_ui_wait_for_return(void);
int terminal_ui_main_menu(const char *const labels[], int count);
int terminal_ui_read_int(const char *prompt, int min_value, int max_value);
bool terminal_ui_read_text(const char *prompt,
                           char *buffer,
                           size_t size,
                           bool secret);
bool terminal_ui_confirm(const char *prompt, bool default_yes);
bool terminal_ui_poll_cancel(void);

int ui_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

#endif
