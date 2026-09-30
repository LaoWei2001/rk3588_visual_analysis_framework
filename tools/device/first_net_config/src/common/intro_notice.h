#ifndef FIRST_NET_CONFIG_INTRO_NOTICE_H
#define FIRST_NET_CONFIG_INTRO_NOTICE_H

#include <stdio.h>

/* Rows reserved for one blank separator plus the startup license notice. */
int intro_notice_reserved_rows(void);

/* Draw the project copyright and GPL notice starting at first_row. */
void intro_notice_draw(int first_row);

/* Print the same notice when the program is running without ncurses. */
void intro_notice_print(FILE *stream);

#endif
