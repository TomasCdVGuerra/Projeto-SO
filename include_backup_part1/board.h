#ifndef BOARD_H
#define BOARD_H

#include <pthread.h>

#define MAX_MOVES 20
#define MAX_LEVELS 20
#define MAX_FILENAME 256
#define MAX_GHOSTS 25

typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int threshold;
    int generation;
} simple_barrier_t;

typedef enum
{
    REACHED_PORTAL = 1,
    VALID_MOVE = 0,
    INVALID_MOVE = -1,
    DEAD_PACMAN = -2,
} move_t;

typedef struct
{
    char command;
    int turns;
    int turns_left;
} command_t;

typedef struct
{
    int pos_x, pos_y;
    int alive;
    int points;
    int passo;
    command_t moves[MAX_MOVES];
    int current_move;
    int n_moves;
    int waiting;
} pacman_t;

typedef struct
{
    int pos_x, pos_y;
    int passo;
    command_t moves[MAX_MOVES];
    int n_moves;
    int current_move;
    int waiting;
    int charged;
} ghost_t;

typedef struct
{
    char content;
    int has_dot;
    int has_portal;
    pthread_mutex_t pos_mutex;
} board_pos_t;

typedef struct
{
    int width, height;
    board_pos_t *board;
    int n_pacmans;
    pacman_t *pacmans;
    int n_ghosts;
    ghost_t *ghosts;
    char level_name[256];
    char pacman_file[256];
    char ghosts_files[MAX_GHOSTS][256];
    int tempo;
    simple_barrier_t turn_barrier;
    pthread_t *ghost_threads;
    int threads_active;
} board_t;
void sleep_ms(int milliseconds);
void barrier_init(simple_barrier_t *barrier, int count);
void barrier_wait(simple_barrier_t *barrier);
void barrier_destroy(simple_barrier_t *barrier);
int move_pacman(board_t *board, int pacman_index, command_t *command);
int move_ghost(board_t *board, int ghost_index, command_t *command);
void kill_pacman(board_t *board, int pacman_index);
int load_pacman(board_t *board, int points);
int load_ghost(board_t *board);
int load_level(board_t *board, int accumulated_points);
void unload_level(board_t *board);
void open_debug_file(char *filename);
void close_debug_file();
void debug(const char *format, ...);
void print_board(board_t *board);

#endif
