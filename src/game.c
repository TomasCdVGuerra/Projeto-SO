#include "board.h"
#include "display.h"
#include "loader.h"
#include "save.h"
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
        /* Fork-based quicksave: child writes save file and exits; parent continues. */
        pid_t pid = fork();
        if (pid < 0)
        {
            debug("Fork failed for quicksave\n");
            return CONTINUE_PLAY;
        }
        else if (pid == 0)
        {
            /* Child: perform save and exit immediately */
            int r = save_game("save.dat", game_board);
            if (r == 0)
                debug("Quicksave: saved to save.dat\n");
            else
                debug("Quicksave: save failed\n");
            _exit(0);
        }
        else
        {
            /* Parent: continue playing */
            return CONTINUE_PLAY;
        }
    }

    if (play->command == 'L')
    {
        /* Quickload from save.dat */
        int r = load_game("save.dat", game_board);
        if (r == 0)
        {
            debug("Quickload: loaded save.dat\n");
        }
        else
        {
            debug("Quickload: failed to load save.dat\n");
        }
        return CONTINUE_PLAY;
    }

    int result = move_pacman(game_board, 0, play);
    if (result == REACHED_PORTAL)
    {
        // Next level
        return NEXT_LEVEL;
    }

    if (result == DEAD_PACMAN)
    {
        return QUIT_GAME;
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
        return QUIT_GAME;
    }

    return CONTINUE_PLAY;
}

int main(int argc, char **argv)
{
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

    open_debug_file("debug.log");

    terminal_init();

    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    /* PID of the saved (stopped) child process holding a quicksave state.
     * Per-assignment: only one saved state at a time. 0 means no saved state.
     */
    pid_t saved_pid = 0;

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
                /* Create a fork-based quicksave. If a previous saved child
                 * exists, kill it to maintain only one saved state.
                 */
                if (saved_pid != 0)
                {
                    kill(saved_pid, SIGKILL);
                    waitpid(saved_pid, NULL, 0);
                    saved_pid = 0;
                }

                pid_t pid = fork();
                if (pid < 0)
                {
                    debug("Quicksave: fork failed: %d\n", errno);
                }
                else if (pid == 0)
                {
                    /* Child: suspend itself to act as the saved state. It will
                     * be resumed later with SIGCONT to restore.
                     */
                    debug("Quicksave: child created (pid=%d) - suspending\n", getpid());
                    kill(getpid(), SIGSTOP);
                    /* When resumed, child continues execution here and effectively
                     * becomes the restored process.
                     */
                    debug("Quicksave: child resumed (pid=%d)\n", getpid());
                    /* Clear saved_pid in child context to avoid double-management */
                    saved_pid = 0;
                }
                else
                {
                    /* Parent: record child's pid and continue playing */
                    saved_pid = pid;
                    debug("Quicksave: saved child pid=%d\n", (int)saved_pid);
                }

                /* continue playing in the parent */
                continue;
            }

            if (result == LOAD_BACKUP)
            {
                if (saved_pid == 0)
                {
                    debug("Quicksave: no saved state to restore\n");
                    continue;
                }

                /* Signal the saved child to continue and exit the parent so
                 * the child takes over the terminal/process. Cleanup first.
                 */
                debug("Quicksave: restoring saved pid=%d\n", (int)saved_pid);
                if (kill(saved_pid, SIGCONT) != 0)
                {
                    debug("Quicksave: failed to SIGCONT pid=%d\n", (int)saved_pid);
                    continue;
                }

                /* Cleanup parent and exit so the child continues as the process */
                terminal_cleanup();
                cleanup_level_loader();
                close_debug_file();
                _exit(0);
            }

            if (result == QUIT_GAME)
            {
                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(game_board.tempo);
                end_game = true;
                break;
            }

            screen_refresh(&game_board, DRAW_MENU);

            accumulated_points = game_board.pacmans[0].points;
        }
        print_board(&game_board);
        unload_level(&game_board);
    }

    terminal_cleanup();

    /* Cleanup loader resources */
    cleanup_level_loader();

    close_debug_file();

    return 0;
}
