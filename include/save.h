#ifndef SAVE_H
#define SAVE_H

#include "board.h"

/* Save the provided board to path using POSIX I/O. Returns 0 on success, negative on error. */
int save_game(const char *path, board_t *board);

/* Load board state from path into provided board. Returns 0 on success, negative on error. */
int load_game(const char *path, board_t *board);

#endif
