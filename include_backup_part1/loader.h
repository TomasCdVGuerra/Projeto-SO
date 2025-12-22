/* loader.h - Level and behavior loader (POSIX-based parsing to be implemented)
 * This header provides initialization/cleanup hooks and will later expose
 * APIs to fetch parsed level/behavior information.
 */
#ifndef LOADER_H
#define LOADER_H

#include "board.h"

/* Initialize the loader with the directory containing .lvl/.m/.p files.
 * Returns 0 on success, non-zero on error.
 */
int init_level_loader(const char *dirpath);

/* Cleanup any resources allocated by the loader. */
void cleanup_level_loader(void);

/* Returns non-zero if the loader was initialized successfully. */
int loader_is_initialized(void);

/* Get/Set current level index (for save/restore) */
int get_current_level(void);
void set_current_level(int level);

/* Load the next level from the directory.
 * Populates `board` with the level data.
 * `accumulated_points` is carried over from previous levels.
 * Returns:
 *   0 on success
 *   1 if no more levels are available
 *  -1 on error
 */
int load_next_level(board_t *board, int accumulated_points);

/* Reset the internal level iterator to allow loading from the first level again. */
void reset_level_iterator(void);

#endif /* LOADER_H */
