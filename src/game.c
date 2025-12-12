#include "board.h"
#include "display.h"
#include "loader.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define CREATE_BACKUP 4

typedef struct
{
    board_t *board;
    int ghost_index;
} ghost_thread_args_t;

void *ghost_worker(void *arg)
{
    ghost_thread_args_t *args = (ghost_thread_args_t *)arg;
    board_t *board = args->board;
    int ghost_idx = args->ghost_index;

    while (board->threads_active)
    {
        barrier_wait(&board->turn_barrier);

        if (!board->threads_active)
        {
            break;
        }

        ghost_t *ghost = &board->ghosts[ghost_idx];
        command_t *cmd = &ghost->moves[ghost->current_move % ghost->n_moves];
        move_ghost(board, ghost_idx, cmd);

        barrier_wait(&board->turn_barrier);
    }

    free(args);
    return NULL;
}

void screen_refresh(board_t *game_board, int mode)
{
    draw_board(game_board, mode);
    refresh_screen();
    if (game_board->tempo > 0)
        sleep_ms(game_board->tempo);
}

int play_board(board_t *game_board)
{
    pacman_t *pacman = &game_board->pacmans[0];
    command_t *play;

    if (pacman->n_moves == 0)
    {
        command_t c;
        c.command = get_input();
        c.turns = 1;
        c.turns_left = 1;
        if (c.command == '\0')
            c.command = 'T';
        play = &c;
    }
    else
    {
        play = &pacman->moves[pacman->current_move % pacman->n_moves];
    }

    if (play->command == 'Q')
        return QUIT_GAME;

    if (pacman->n_moves == 0)
    {
        if (play->command == 'G')
            return CREATE_BACKUP;
    }

    barrier_wait(&game_board->turn_barrier);
    int result = move_pacman(game_board, 0, play);
    barrier_wait(&game_board->turn_barrier);

    if (result == REACHED_PORTAL)
        return NEXT_LEVEL;
    if (result == DEAD_PACMAN || !game_board->pacmans[0].alive)
        return DEAD_PACMAN;
    return CONTINUE_PLAY;
}

static int restore_from_backup(board_t *game_board, int *accumulated_points,
                               pid_t *saved_pid, int *saved_pipe_fd)
{
    if (*saved_pid == 0)
        return -1;

    kill(*saved_pid, SIGCONT);

    board_t temp_board;
    if (read(*saved_pipe_fd, &temp_board, sizeof(board_t)) != sizeof(board_t))
    {
        kill(*saved_pid, SIGKILL);
        waitpid(*saved_pid, NULL, 0);
        close(*saved_pipe_fd);
        *saved_pid = 0;
        *saved_pipe_fd = -1;
        return -1;
    }

    unload_level(game_board);
    *game_board = temp_board;

    int board_size = game_board->width * game_board->height;
    game_board->board = malloc(board_size * sizeof(board_pos_t));
    read(*saved_pipe_fd, game_board->board, board_size * sizeof(board_pos_t));

    for (int i = 0; i < board_size; i++)
    {
        pthread_mutex_init(&game_board->board[i].pos_mutex, NULL);
    }

    game_board->pacmans = malloc(game_board->n_pacmans * sizeof(pacman_t));
    read(*saved_pipe_fd, game_board->pacmans, game_board->n_pacmans * sizeof(pacman_t));

    game_board->ghosts = malloc(game_board->n_ghosts * sizeof(ghost_t));
    read(*saved_pipe_fd, game_board->ghosts, game_board->n_ghosts * sizeof(ghost_t));

    read(*saved_pipe_fd, accumulated_points, sizeof(int));

    int lvl;
    read(*saved_pipe_fd, &lvl, sizeof(int));
    set_current_level(lvl);

    waitpid(*saved_pid, NULL, 0);
    close(*saved_pipe_fd);
    *saved_pid = 0;
    *saved_pipe_fd = -1;

    return 0;
}

static void init_threads(board_t *game_board)
{
    barrier_init(&game_board->turn_barrier, game_board->n_ghosts + 1);
    game_board->threads_active = 1;

    game_board->ghost_threads = malloc(game_board->n_ghosts * sizeof(pthread_t));

    for (int i = 0; i < game_board->n_ghosts; i++)
    {
        ghost_thread_args_t *args = malloc(sizeof(ghost_thread_args_t));
        args->board = game_board;
        args->ghost_index = i;
        pthread_create(&game_board->ghost_threads[i], NULL, ghost_worker, args);
    }
}

static void cleanup_threads(board_t *game_board)
{
    game_board->threads_active = 0;

    barrier_wait(&game_board->turn_barrier);

    for (int i = 0; i < game_board->n_ghosts; i++)
    {
        pthread_join(game_board->ghost_threads[i], NULL);
    }

    free(game_board->ghost_threads);
    barrier_destroy(&game_board->turn_barrier);
}

int main(int argc, char **argv)
{
    int use_loader = 0;
    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;
    pid_t saved_pid = 0;
    int saved_pipe_fd = -1;

    open_debug_file("debug.log");

    if (argc == 2)
    {
        if (init_level_loader(argv[1]) != 0)
        {
            const char *msg = "Error: cannot access directory '";
            write(STDERR_FILENO, msg, strlen(msg));
            write(STDERR_FILENO, argv[1], strlen(argv[1]));
            write(STDERR_FILENO, "'\n", 2);
            return 2;
        }
        use_loader = 1;
    }
    else if (argc > 2)
    {
        const char *usage = "Usage: ";
        write(STDERR_FILENO, usage, strlen(usage));
        write(STDERR_FILENO, argv[0], strlen(argv[0]));
        write(STDERR_FILENO, " [<level_directory>]\n", 22);
        return 1;
    }

    srand((unsigned int)time(NULL));
    terminal_init();

    while (!end_game)
    {
        if (use_loader)
        {
            int result = load_next_level(&game_board, accumulated_points);
            if (result == 1)
                break;
            if (result < 0)
            {
                write(STDERR_FILENO, "Error: failed to load level\n", 29);
                break;
            }
        }
        else
        {
            load_level(&game_board, accumulated_points);
        }

        init_threads(&game_board);

        draw_board(&game_board, DRAW_MENU);
        refresh_screen();

        while (true)
        {
            int result = play_board(&game_board);

            if (result == NEXT_LEVEL)
            {
                screen_refresh(&game_board, DRAW_WIN);
                sleep_ms(game_board.tempo);
                break;
            }

            if (result == CREATE_BACKUP)
            {
                if (saved_pid != 0)
                    continue;

                int pfd[2];
                if (pipe(pfd) == -1)
                    continue;

                pid_t pid = fork();
                if (pid < 0)
                {
                    close(pfd[0]);
                    close(pfd[1]);
                }
                else if (pid == 0)
                {
                    close(pfd[0]);
                    raise(SIGSTOP);

                    write(pfd[1], &game_board, sizeof(board_t));

                    int board_size = game_board.width * game_board.height;
                    write(pfd[1], game_board.board, board_size * sizeof(board_pos_t));
                    write(pfd[1], game_board.pacmans, game_board.n_pacmans * sizeof(pacman_t));
                    write(pfd[1], game_board.ghosts, game_board.n_ghosts * sizeof(ghost_t));
                    write(pfd[1], &accumulated_points, sizeof(int));

                    int lvl = get_current_level();
                    write(pfd[1], &lvl, sizeof(int));

                    close(pfd[1]);
                    _exit(0);
                }
                else
                {
                    close(pfd[1]);
                    saved_pid = pid;
                    saved_pipe_fd = pfd[0];
                }
                continue;
            }
            if (result == QUIT_GAME)
            {
                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(game_board.tempo);
                end_game = true;
                break;
            }

            if (result == DEAD_PACMAN)
            {
                if (saved_pid != 0 &&
                    restore_from_backup(&game_board, &accumulated_points,
                                        &saved_pid, &saved_pipe_fd) == 0)
                {
                    screen_refresh(&game_board, DRAW_MENU);
                }
                else
                {
                    screen_refresh(&game_board, DRAW_GAME_OVER);
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }
                continue;
            }

            screen_refresh(&game_board, DRAW_MENU);
            accumulated_points = game_board.pacmans[0].points;
        }

        cleanup_threads(&game_board);
        print_board(&game_board);
        unload_level(&game_board);
    }

    if (saved_pid != 0)
    {
        kill(saved_pid, SIGKILL);
        waitpid(saved_pid, NULL, 0);
        if (saved_pipe_fd != -1)
            close(saved_pipe_fd);
    }

    terminal_cleanup();
    cleanup_level_loader();
    close_debug_file();

    return 0;
}
