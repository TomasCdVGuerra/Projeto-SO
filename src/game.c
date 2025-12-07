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

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

void screen_refresh(board_t *game_board, int mode)
{
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
    if (game_board->tempo != 0)
        sleep_ms(game_board->tempo);
}

int play_board(board_t *game_board)
{
    pacman_t *pacman = &game_board->pacmans[0];
    command_t *play;
    if (pacman->n_moves == 0)
    { // if is user input
        command_t c;
        c.command = get_input();

        if (c.command == '\0')
            return CONTINUE_PLAY;

        c.turns = 1;
        play = &c;
    }
    else
    { // else if the moves are pre-defined in the file
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the pacman
        play = &pacman->moves[pacman->current_move % pacman->n_moves];
    }

    debug("KEY %c\n", play->command);

    if (play->command == 'Q')
    {
        return QUIT_GAME;
    }

    if (play->command == 'G')
    {
        /* Only allow manual user input to trigger quicksave */
        if (pacman->n_moves == 0)
        {
            debug("Quicksave: user pressed G, creating backup\n");
            return CREATE_BACKUP;
        }
        else
        {
            debug("Quicksave: ignored G from scripted moves\n");
            return CONTINUE_PLAY;
        }
    }

    if (play->command == 'L')
    {
        /* Only allow manual user input to trigger quickload */
        if (pacman->n_moves == 0)
        {
            debug("Quickload: user pressed L, restoring backup\n");
            return LOAD_BACKUP;
        }
        else
        {
            debug("Quickload: ignored L from scripted moves\n");
            return CONTINUE_PLAY;
        }
    }

    int result = move_pacman(game_board, 0, play);
    if (result == REACHED_PORTAL)
    {
        // Next level
        return NEXT_LEVEL;
    }

    if (result == DEAD_PACMAN)
    {
        return DEAD_PACMAN;
    }

    for (int i = 0; i < game_board->n_ghosts; i++)
    {
        ghost_t *ghost = &game_board->ghosts[i];
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the ghost
        move_ghost(game_board, i, &ghost->moves[ghost->current_move % ghost->n_moves]);
    }

    if (!game_board->pacmans[0].alive)
    {
        return DEAD_PACMAN;
    }

    return CONTINUE_PLAY;
}

int main(int argc, char **argv)
{
    /* Open debug file early so initial logs are captured */
    open_debug_file("debug.log");
    debug("main: argc=%d argv1=%s\n", argc, (argc > 1 ? argv[1] : "(null)"));
    /* Backward-compatible CLI: if no directory argument is provided, run the
     * legacy behaviour (use `load_level()`). If a directory is provided, try
     * to initialize the POSIX loader and use `load_next_level()` instead.
     */
    int use_loader = 0;
    if (argc == 2)
    {
        if (init_level_loader(argv[1]) != 0)
        {
            const char *m1 = "Error: cannot access directory '";
            write(STDERR_FILENO, m1, strlen(m1));
            write(STDERR_FILENO, argv[1], strlen(argv[1]));
            write(STDERR_FILENO, "'\n", 2);
            return 2;
        }
        use_loader = 1;
    }
    else if (argc > 2)
    {
        const char *u1 = "Usage: ";
        const char *u2 = " [<level_directory>]\n";
        write(STDERR_FILENO, u1, strlen(u1));
        write(STDERR_FILENO, argv[0], strlen(argv[0]));
        write(STDERR_FILENO, u2, strlen(u2));
        return 1;
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    terminal_init();

    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    /* Fork-based quicksave: PID of the suspended child process holding saved state.
     * Per Exercise 2: only one saved state at a time. 0 means no saved state.
     */
    pid_t saved_pid = 0;
    int saved_pipe_fd = -1; /* Pipe to read saved state from child */

    while (!end_game)
    {
        if (use_loader)
        {
            /* Load next level using the POSIX loader. Handle return codes:
             *  0 = success, 1 = no more levels, negative = error
             */
            int lr = load_next_level(&game_board, accumulated_points);
            if (lr == 1)
            {
                /* No more levels: exit the game loop */
                break;
            }
            else if (lr < 0)
            {
                const char *e = "Error: failed to load next level\n";
                write(STDERR_FILENO, e, strlen(e));
                break;
            }
        }
        else
        {
            /* Legacy single-level loader (pre-file-system changes) */
            load_level(&game_board, accumulated_points);
        }

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
                /* Fork-based quicksave per Exercise 2.
                 * If a previous saved child exists, do nothing as required.
                 */
                if (saved_pid != 0)
                {
                    debug("Quicksave: save already exists, ignoring G\n");
                    continue;
                }

                int pfd[2];
                if (pipe(pfd) == -1)
                {
                    debug("Quicksave: pipe failed: %d\n", errno);
                    continue;
                }

                pid_t pid = fork();
                if (pid < 0)
                {
                    debug("Quicksave: fork failed: %d\n", errno);
                    close(pfd[0]);
                    close(pfd[1]);
                }
                else if (pid == 0)
                {
                    /* CHILD: Suspend ourselves to hold the saved game state.
                     * We'll be resumed with SIGCONT when parent does quickload.
                     */
                    close(pfd[0]); /* Close read end */

                    debug("Quicksave: child (pid=%d) suspending to hold saved state\n", getpid());
                    raise(SIGSTOP); /* Suspend ourselves */

                    /* When we reach here, we've been resumed by SIGCONT from parent.
                     * We write our state to the pipe and exit.
                     */
                    debug("Quicksave: child (pid=%d) resumed, sending state to parent\n", getpid());

                    /* Write board struct */
                    write(pfd[1], &game_board, sizeof(board_t));

                    /* Write dynamic arrays */
                    int board_size = game_board.width * game_board.height;
                    write(pfd[1], game_board.board, board_size * sizeof(board_pos_t));
                    write(pfd[1], game_board.pacmans, game_board.n_pacmans * sizeof(pacman_t));
                    write(pfd[1], game_board.ghosts, game_board.n_ghosts * sizeof(ghost_t));

                    /* Write accumulated points */
                    write(pfd[1], &accumulated_points, sizeof(int));

                    /* Write current level index */
                    int lvl = get_current_level();
                    write(pfd[1], &lvl, sizeof(int));

                    close(pfd[1]);

                    /* Clean up and exit */
                    /* Note: we don't unload_level here because we want to preserve the pointers
                       in the parent's memory (which we are a copy of). But since we are exiting,
                       OS reclaims memory. We should be careful not to double-free if we used shared mem,
                       but here it's COW. */
                    _exit(0);
                }
                else
                {
                    /* PARENT: Record child PID and pipe. */
                    close(pfd[1]); /* Close write end */
                    saved_pid = pid;
                    saved_pipe_fd = pfd[0];
                    debug("Quicksave: parent continues, child pid=%d suspended with saved state\n", (int)saved_pid);
                }

                continue;
            }

            if (result == LOAD_BACKUP)
            {
            load_backup_label:
                if (saved_pid == 0)
                {
                    debug("Quickload: no saved state to restore\n");
                    continue;
                }

                debug("Quickload: restoring state from child (pid=%d)\n", (int)saved_pid);

                /* Resume the child process so it writes data to pipe */
                kill(saved_pid, SIGCONT);

                /* Read board struct */
                board_t temp_board;
                if (read(saved_pipe_fd, &temp_board, sizeof(board_t)) != sizeof(board_t))
                {
                    debug("Quickload: failed to read board struct\n");
                    /* If read fails, maybe child died? Cleanup. */
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }

                /* Unload current level to free memory before overwriting */
                unload_level(&game_board);

                /* Copy struct fields (except pointers which we'll allocate) */
                game_board = temp_board;

                /* Allocate and read dynamic arrays */
                int board_size = game_board.width * game_board.height;
                game_board.board = malloc(board_size * sizeof(board_pos_t));
                if (!game_board.board)
                {
                    debug("Quickload: malloc failed for board\n");
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }
                if (read(saved_pipe_fd, game_board.board, board_size * sizeof(board_pos_t)) != (ssize_t)(board_size * sizeof(board_pos_t)))
                {
                    debug("Quickload: failed to read board data\n");
                    free(game_board.board);
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }

                game_board.pacmans = malloc(game_board.n_pacmans * sizeof(pacman_t));
                if (!game_board.pacmans)
                {
                    debug("Quickload: malloc failed for pacmans\n");
                    free(game_board.board);
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }
                if (read(saved_pipe_fd, game_board.pacmans, game_board.n_pacmans * sizeof(pacman_t)) != (ssize_t)(game_board.n_pacmans * sizeof(pacman_t)))
                {
                    debug("Quickload: failed to read pacmans data\n");
                    free(game_board.board);
                    free(game_board.pacmans);
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }

                game_board.ghosts = malloc(game_board.n_ghosts * sizeof(ghost_t));
                if (!game_board.ghosts)
                {
                    debug("Quickload: malloc failed for ghosts\n");
                    free(game_board.board);
                    free(game_board.pacmans);
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }
                if (read(saved_pipe_fd, game_board.ghosts, game_board.n_ghosts * sizeof(ghost_t)) != (ssize_t)(game_board.n_ghosts * sizeof(ghost_t)))
                {
                    debug("Quickload: failed to read ghosts data\n");
                    free(game_board.board);
                    free(game_board.pacmans);
                    free(game_board.ghosts);
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }

                /* Read accumulated points */
                if (read(saved_pipe_fd, &accumulated_points, sizeof(int)) != sizeof(int))
                {
                    debug("Quickload: failed to read accumulated_points\n");
                    free(game_board.board);
                    free(game_board.pacmans);
                    free(game_board.ghosts);
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }

                /* Read current level index */
                int lvl;
                if (read(saved_pipe_fd, &lvl, sizeof(int)) != sizeof(int))
                {
                    debug("Quickload: failed to read level index\n");
                    free(game_board.board);
                    free(game_board.pacmans);
                    free(game_board.ghosts);
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    close(saved_pipe_fd);
                    saved_pid = 0;
                    saved_pipe_fd = -1;
                    continue;
                }
                set_current_level(lvl);

                /* Wait for child to exit */
                waitpid(saved_pid, NULL, 0);
                close(saved_pipe_fd);
                saved_pid = 0;
                saved_pipe_fd = -1;

                debug("Quickload: state restored successfully\n");

                /* Force a screen refresh so the user sees the restored state immediately */
                screen_refresh(&game_board, DRAW_MENU);

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
                if (saved_pid != 0)
                {
                    debug("Dead Pacman: restoring from save (pid=%d)\n", (int)saved_pid);
                    goto load_backup_label;
                }
                else
                {
                    screen_refresh(&game_board, DRAW_GAME_OVER);
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }
            }

            screen_refresh(&game_board, DRAW_MENU);

            accumulated_points = game_board.pacmans[0].points;
        }
        print_board(&game_board);
        unload_level(&game_board);
    }

    /* Cleanup saved process if it exists */
    if (saved_pid != 0)
    {
        kill(saved_pid, SIGKILL);
        waitpid(saved_pid, NULL, 0);
        if (saved_pipe_fd != -1)
            close(saved_pipe_fd);
    }

    terminal_cleanup();

    /* Cleanup loader resources */
    cleanup_level_loader();

    close_debug_file();

    return 0;
}
