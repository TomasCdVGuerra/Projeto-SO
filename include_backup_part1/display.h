#ifndef DISPLAY_H
#define DISPLAY_H

#include "board.h"
#include <ncurses.h>

#define DRAW_GAME_OVER 0
#define DRAW_WIN 1
#define DRAW_MENU 2
int terminal_init();

void draw_board(board_t *board, int mode);
void draw(char c, int colour_i, int pos_x, int pos_y);
void refresh_screen();
char get_input();

void terminal_cleanup();

#endif
