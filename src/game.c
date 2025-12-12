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

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0)
    {
        ssize_t w = write(fd, p, left);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            return -1;
        p += (size_t)w;
        left -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    size_t left = n;
    while (left > 0)
    {
        ssize_t r = read(fd, p, left);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;
        p += (size_t)r;
        left -= (size_t)r;
    }
    return 0;
}

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

    /* Signal turn start: all threads wake and move in parallel */
    barrier_wait(&game_board->turn_barrier);
    int result = move_pacman(game_board, 0, play);
    /* Wait for all movements to complete before rendering */
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

    debug("restore_from_backup: resume pid=%d\n", *saved_pid);

    kill(*saved_pid, SIGCONT);

    board_t temp_board;
    if (read_all(*saved_pipe_fd, &temp_board, sizeof(board_t)) != 0)
    {
        kill(*saved_pid, SIGKILL);
        waitpid(*saved_pid, NULL, 0);
        close(*saved_pipe_fd);
        *saved_pid = 0;
        *saved_pipe_fd = -1;
        debug("restore_from_backup: read board header failed\n");
        return -1;
    }

    unload_level(game_board);
    *game_board = temp_board;

    int board_size = game_board->width * game_board->height;
    game_board->board = malloc(board_size * sizeof(board_pos_t));
    if (!game_board->board)
        return -1;
    if (read_all(*saved_pipe_fd, game_board->board, board_size * sizeof(board_pos_t)) != 0)
    {
        debug("restore_from_backup: read board cells failed\n");
        return -1;
    }

    for (int i = 0; i < board_size; i++)
    {
        /* Never reuse mutex state coming from the saved snapshot.
         * The bytes read from the pipe may represent a locked/busy mutex.
         */
        memset(&game_board->board[i].pos_mutex, 0, sizeof(pthread_mutex_t));
        pthread_mutex_init(&game_board->board[i].pos_mutex, NULL);
    }

    game_board->pacmans = malloc(game_board->n_pacmans * sizeof(pacman_t));
    if (!game_board->pacmans)
        return -1;
    if (read_all(*saved_pipe_fd, game_board->pacmans, game_board->n_pacmans * sizeof(pacman_t)) != 0)
    {
        debug("restore_from_backup: read pacmans failed\n");
        return -1;
    }

    game_board->ghosts = malloc(game_board->n_ghosts * sizeof(ghost_t));
    if (!game_board->ghosts)
        return -1;
    if (read_all(*saved_pipe_fd, game_board->ghosts, game_board->n_ghosts * sizeof(ghost_t)) != 0)
    {
        debug("restore_from_backup: read ghosts failed\n");
        return -1;
    }

    if (read_all(*saved_pipe_fd, accumulated_points, sizeof(int)) != 0)
    {
        debug("restore_from_backup: read points failed\n");
        return -1;
    }

    int lvl;
    if (read_all(*saved_pipe_fd, &lvl, sizeof(int)) != 0)
    {
        debug("restore_from_backup: read level idx failed\n");
        return -1;
    }
    set_current_level(lvl);

    waitpid(*saved_pid, NULL, 0);
    close(*saved_pipe_fd);
    *saved_pid = 0;
    *saved_pipe_fd = -1;

    /* Ensure threading primitives get re-initialized after restore. */
    memset(&game_board->turn_barrier, 0, sizeof(simple_barrier_t));
    game_board->ghost_threads = NULL;
    game_board->threads_active = 0;

    debug("restore_from_backup: success, lvl=%d\n", lvl);

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

    bool restored_from_backup = false;

    while (!end_game)
    {
        bool threads_inited = false;

        if (!restored_from_backup)
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
        }
        restored_from_backup = false;

        init_threads(&game_board);
        threads_inited = true;

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
                {
                    debug("CREATE_BACKUP: saved_pid already %d, skipping\n", saved_pid);
                    continue;
                }

                /* Take snapshot only with threads stopped to avoid copying live mutex state */
                if (threads_inited)
                {
                    cleanup_threads(&game_board);
                    threads_inited = false;
                }

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

                    if (write_all(pfd[1], &game_board, sizeof(board_t)) != 0)
                    {
                        close(pfd[1]);
                        _exit(1);
                    }

                    int board_size = game_board.width * game_board.height;
                    if (write_all(pfd[1], game_board.board, board_size * sizeof(board_pos_t)) != 0)
                    {
                        close(pfd[1]);
                        _exit(1);
                    }
                    if (write_all(pfd[1], game_board.pacmans, game_board.n_pacmans * sizeof(pacman_t)) != 0)
                    {
                        close(pfd[1]);
                        _exit(1);
                    }
                    if (write_all(pfd[1], game_board.ghosts, game_board.n_ghosts * sizeof(ghost_t)) != 0)
                    {
                        close(pfd[1]);
                        _exit(1);
                    }
                    if (write_all(pfd[1], &accumulated_points, sizeof(int)) != 0)
                    {
                        close(pfd[1]);
                        _exit(1);
                    }

                    int lvl = get_current_level();
                    if (write_all(pfd[1], &lvl, sizeof(int)) != 0)
                    {
                        close(pfd[1]);
                        _exit(1);
                    }

                    close(pfd[1]);
                    _exit(0);
                }
                else
                {
                    close(pfd[1]);
                    saved_pid = pid;
                    saved_pipe_fd = pfd[0];
                    debug("CREATE_BACKUP: saved new pid=%d\n", saved_pid);
                }

                /* Restart threads after snapshot */
                init_threads(&game_board);
                threads_inited = true;
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
                debug("DEAD_PACMAN: saved_pid=%d\n", saved_pid);
                if (saved_pid != 0)
                {
                    if (threads_inited)
                    {
                        cleanup_threads(&game_board);
                        threads_inited = false;
                    }

                    debug("DEAD_PACMAN: attempting restore\n");
                    if (restore_from_backup(&game_board, &accumulated_points,
                                            &saved_pid, &saved_pipe_fd) == 0)
                    {
                        debug("DEAD_PACMAN: restore ok\n");
                        /* Cross-level restore: break out and reload the restored level */
                        restored_from_backup = true;
                        break;
                    }
                    debug("DEAD_PACMAN: restore failed\n");
                }

                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(game_board.tempo);
                end_game = true;
                break;
            }

            screen_refresh(&game_board, DRAW_MENU);
            accumulated_points = game_board.pacmans[0].points;
        }

        if (threads_inited)
        {
            cleanup_threads(&game_board);
            threads_inited = false;
        }

        if (!restored_from_backup)
        {
            /* Normal level completion or game over - clean up and move on */
            print_board(&game_board);
            unload_level(&game_board);
        }
        else
        {
            /* Restored from backup - board is already cleaned and reloaded, just display it */
            draw_board(&game_board, DRAW_MENU);
            refresh_screen();
        }
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

    debug("Main: returning 0\n");
    return 0;
}
