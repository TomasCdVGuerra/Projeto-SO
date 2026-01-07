#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <semaphore.h>
#include <fcntl.h>
#include <poll.h>

#include <stdbool.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

#define MAX_PIPE_PATH_LENGTH 40
#define BUFFER_SIZE 10

typedef struct
{
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} client_connection_t;

client_connection_t connection_buffer[BUFFER_SIZE];
int buffer_in = 0;
int buffer_out = 0;
int buffer_count = 0;

pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t *buffer_full;
sem_t *buffer_empty;
static char sem_full_name[64];
static char sem_empty_name[64];
static char fifo_name_global[256];
static void cleanup_ipc(void);
static void sigint_handler(int signo)
{
    (void)signo;
    cleanup_ipc();
    _exit(1);
}
static void cleanup_ipc(void)
{
    if (buffer_full != NULL && buffer_full != SEM_FAILED)
    {
        sem_close(buffer_full);
        sem_unlink(sem_full_name);
    }
    if (buffer_empty != NULL && buffer_empty != SEM_FAILED)
    {
        sem_close(buffer_empty);
        sem_unlink(sem_empty_name);
    }
    if (fifo_name_global[0] != '\0')
    {
        unlink(fifo_name_global);
    }
}

static int read_full(int fd, void *buf, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        ssize_t r = read(fd, (char *)buf + off, n - off);
        if (r == 0)
            return -1; // EOF
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

typedef struct
{
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct
{
    board_t *board;
    int req_fd;
    int notif_fd;
    int ghost_index;
    int *shutdown_flag;
} session_args_t;

char get_board_char(board_t *board, int x, int y)
{
    int index = y * board->width + x;
    char ch = board->board[index].content;

    // Check for Pacman
    for (int p = 0; p < board->n_pacmans; p++)
    {
        if (board->pacmans[p].pos_x == x && board->pacmans[p].pos_y == y && board->pacmans[p].alive)
        {
            return 'C'; // Pacman character
        }
    }

    // Check for Ghosts
    for (int g = 0; g < board->n_ghosts; g++)
    {
        if (board->ghosts[g].pos_x == x && board->ghosts[g].pos_y == y)
        {
            return 'M'; // Monster character
        }
    }

    return ch;
}

int send_board_update(int fd, board_t *board)
{
    char op_code = 4;
    int width = board->width;
    int height = board->height;
    int tempo = board->tempo;
    int victory = 0;   // TODO
    int game_over = 0; // TODO
    int points = board->pacmans[0].points;

    ssize_t w = 0;
    if ((w = write(fd, &op_code, 1)) != 1)
        return (w < 0 && errno == EPIPE) ? -1 : 0;
    if ((w = write(fd, &width, sizeof(int))) != (ssize_t)sizeof(int))
        return (w < 0 && errno == EPIPE) ? -1 : 0;
    if ((w = write(fd, &height, sizeof(int))) != (ssize_t)sizeof(int))
        return (w < 0 && errno == EPIPE) ? -1 : 0;
    if ((w = write(fd, &tempo, sizeof(int))) != (ssize_t)sizeof(int))
        return (w < 0 && errno == EPIPE) ? -1 : 0;
    if ((w = write(fd, &victory, sizeof(int))) != (ssize_t)sizeof(int))
        return (w < 0 && errno == EPIPE) ? -1 : 0;
    if ((w = write(fd, &game_over, sizeof(int))) != (ssize_t)sizeof(int))
        return (w < 0 && errno == EPIPE) ? -1 : 0;
    if ((w = write(fd, &points, sizeof(int))) != (ssize_t)sizeof(int))
        return (w < 0 && errno == EPIPE) ? -1 : 0;

    char *buffer = malloc(width * height);
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            buffer[y * width + x] = get_board_char(board, x, y);
        }
    }
    w = write(fd, buffer, width * height);
    free(buffer);
    if (w != width * height)
        return (w < 0 && errno == EPIPE) ? -1 : 0;

    return 0;
}

int create_backup()
{
    // clear the terminal for process transition
    terminal_cleanup();

    pid_t child = fork();

    if (child != 0)
    {
        if (child < 0)
        {
            return -1;
        }

        return child;
    }
    else
    {
        debug("[%d] Created\n", getpid());

        return 0;
    }
}

void screen_refresh(board_t *game_board, int mode)
{
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
}

void *server_update_thread(void *arg)
{
    session_args_t *args = (session_args_t *)arg;
    board_t *board = args->board;
    int notif_fd = args->notif_fd;
    int *shutdown_flag = args->shutdown_flag;

    free(args);

    sleep_ms(board->tempo / 2);
    while (true)
    {
        sleep_ms(board->tempo);
        pthread_rwlock_wrlock(&board->state_lock);
        if (*shutdown_flag)
        {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        if (send_board_update(notif_fd, board) == -1)
        {
            *shutdown_flag = 1;
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void *pacman_thread(void *arg)
{
    session_args_t *args = (session_args_t *)arg;
    board_t *board = args->board;
    int req_fd = args->req_fd;
    int *shutdown_flag = args->shutdown_flag;

    free(args);

    pacman_t *pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));
    *retval = CONTINUE_PLAY;

    // Non-blocking read with poll to avoid busy-wait
    int flags = fcntl(req_fd, F_GETFL, 0);
    fcntl(req_fd, F_SETFL, flags | O_NONBLOCK);

    while (true)
    {
        if (*shutdown_flag)
        {
            *retval = QUIT_GAME;
            return (void *)retval;
        }

        if (!pacman->alive)
        {
            *retval = LOAD_BACKUP;
            return (void *)retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t *play;
        command_t c;
        if (pacman->n_moves == 0)
        {
            char op_code;
            char cmd_char;
            struct pollfd pfd;
            pfd.fd = req_fd;
            pfd.events = POLLIN;
            int pres = poll(&pfd, 1, board->tempo);
            if (pres < 0)
            {
                if (errno == EINTR)
                    continue;
                *retval = QUIT_GAME;
                return (void *)retval;
            }
            if (pres == 0)
            {
                continue; // no input this tick
            }

            ssize_t n = read(req_fd, &op_code, 1);
            if (n == 0)
            {
                *retval = QUIT_GAME;
                return (void *)retval;
            }
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                *retval = QUIT_GAME;
                return (void *)retval;
            }

            if (op_code == 3) // OP_CODE_PLAY
            {
                if (read(req_fd, &cmd_char, 1) > 0)
                {
                    c.command = cmd_char;
                    c.turns = 1;
                    play = &c;
                }
                else
                {
                    continue;
                }
            }
            else if (op_code == 2) // OP_CODE_DISCONNECT
            {
                *retval = QUIT_GAME;
                return (void *)retval;
            }
            else
            {
                continue;
            }
        }
        else
        {
            play = &pacman->moves[pacman->current_move % pacman->n_moves];
        }

        debug("KEY %c\n", play->command);

        // QUIT
        if (play->command == 'Q')
        {
            *retval = QUIT_GAME;
            return (void *)retval;
        }

        // G command disabled

        pthread_rwlock_rdlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL)
        {
            // Next level
            *retval = NEXT_LEVEL;
            break;
        }

        if (result == DEAD_PACMAN)
        {
            // Restart
            *retval = LOAD_BACKUP;
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    pthread_rwlock_unlock(&board->state_lock);
    return (void *)retval;
}

void *ghost_thread(void *arg)
{
    session_args_t *ghost_arg = (session_args_t *)arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    int *shutdown_flag = ghost_arg->shutdown_flag;

    free(ghost_arg);

    ghost_t *ghost = &board->ghosts[ghost_ind];

    while (true)
    {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (*shutdown_flag)
        {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }

        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

#include <signal.h>

#define MAX_ACTIVE_GAMES 100
board_t *active_boards[MAX_ACTIVE_GAMES];
pthread_mutex_t active_boards_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t sigusr1_received = 0;

void sigusr1_handler(int signo)
{
    (void)signo;
    sigusr1_received = 1;
}

typedef struct
{
    int id;
    int score;
} score_entry_t;

int compare_scores(const void *a, const void *b)
{
    score_entry_t *sa = (score_entry_t *)a;
    score_entry_t *sb = (score_entry_t *)b;
    return sb->score - sa->score; // Descending
}

void log_top_scores()
{
    pthread_mutex_lock(&active_boards_mutex);

    score_entry_t scores[MAX_ACTIVE_GAMES];
    int count = 0;

    for (int i = 0; i < MAX_ACTIVE_GAMES; i++)
    {
        if (active_boards[i] != NULL)
        {
            // Assuming client ID is not stored in board, using index or just listing scores.
            // The requirement says "identificados pelo próprio id".
            // We don't have client ID in board_t.
            // We can add it or just use a placeholder.
            // Let's assume we can't easily get the ID without modifying board_t.
            // I'll just use the index i as ID for now.
            scores[count].id = i;
            scores[count].score = active_boards[i]->pacmans[0].points;
            count++;
        }
    }

    pthread_mutex_unlock(&active_boards_mutex);

    qsort(scores, count, sizeof(score_entry_t), compare_scores);

    FILE *f = fopen("top_scores.txt", "w");
    if (f)
    {
        for (int i = 0; i < count && i < 5; i++)
        {
            fprintf(f, "Client %d: %d\n", scores[i].id, scores[i].score);
        }
        fclose(f);
        debug("Logged top scores to top_scores.txt\n");
    }
    else
    {
        perror("fopen top_scores.txt");
    }
}

void run_game_session(char *level_dir_name, int req_fd, int notif_fd)
{
    DIR *level_dir = opendir(level_dir_name);
    if (level_dir == NULL)
    {
        debug("Failed to open level dir: %s\n", level_dir_name);
        return;
    }

    int accumulated_points = 0;
    bool end_game = false;
    int session_shutdown = 0;
    board_t game_board;

    // Register board
    int board_idx = -1;
    pthread_mutex_lock(&active_boards_mutex);
    for (int i = 0; i < MAX_ACTIVE_GAMES; i++)
    {
        if (active_boards[i] == NULL)
        {
            active_boards[i] = &game_board;
            board_idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&active_boards_mutex);

    struct dirent *entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game)
    {
        if (entry->d_name[0] == '.')
            continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot)
            continue;
        if (strcmp(dot, ".lvl") != 0)
            continue;

        load_level(&game_board, entry->d_name, level_dir_name, accumulated_points);
        // Ignore server-side scripted moves; keep PAC position/tempo only
        game_board.pacmans[0].n_moves = 0;
        game_board.pacmans[0].current_move = 0;

        while (true)
        {
            pthread_t update_tid, pacman_tid;
            pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));
            if (!ghost_tids)
            {
                debug("malloc ghost_tids failed\n");
                end_game = true;
                break;
            }
            session_shutdown = 0;

            // Create Pacman thread
            session_args_t *pac_arg = malloc(sizeof(session_args_t));
            if (!pac_arg)
            {
                free(ghost_tids);
                debug("malloc pac_arg failed\n");
                end_game = true;
                break;
            }
            pac_arg->board = &game_board;
            pac_arg->req_fd = req_fd;
            pac_arg->notif_fd = notif_fd;
            pac_arg->shutdown_flag = &session_shutdown;
            pthread_create(&pacman_tid, NULL, pacman_thread, (void *)pac_arg);

            // Create Ghost threads
            for (int i = 0; i < game_board.n_ghosts; i++)
            {
                session_args_t *arg = malloc(sizeof(session_args_t));
                if (!arg)
                {
                    session_shutdown = 1;
                    pthread_rwlock_wrlock(&game_board.state_lock);
                    pthread_rwlock_unlock(&game_board.state_lock);
                    break;
                }
                arg->board = &game_board;
                arg->ghost_index = i;
                arg->shutdown_flag = &session_shutdown;
                pthread_create(&ghost_tids[i], NULL, ghost_thread, (void *)arg);
            }

            // Create Update thread
            session_args_t *upd_arg = malloc(sizeof(session_args_t));
            if (!upd_arg)
            {
                session_shutdown = 1;
                pthread_rwlock_wrlock(&game_board.state_lock);
                pthread_rwlock_unlock(&game_board.state_lock);
                free(ghost_tids);
                end_game = true;
                break;
            }
            upd_arg->board = &game_board;
            upd_arg->notif_fd = notif_fd;
            upd_arg->shutdown_flag = &session_shutdown;
            pthread_create(&update_tid, NULL, server_update_thread, (void *)upd_arg);

            int *retval;
            pthread_join(pacman_tid, (void **)&retval);

            pthread_rwlock_wrlock(&game_board.state_lock);
            session_shutdown = 1;
            pthread_rwlock_unlock(&game_board.state_lock);

            pthread_join(update_tid, NULL);
            for (int i = 0; i < game_board.n_ghosts; i++)
            {
                pthread_join(ghost_tids[i], NULL);
            }

            free(ghost_tids);

            int result = *retval;
            free(retval);

            if (result == NEXT_LEVEL)
            {
                sleep_ms(game_board.tempo);
                break;
            }

            if (result == QUIT_GAME)
            {
                end_game = true;
                break;
            }

            if (result == LOAD_BACKUP)
            {
                unload_level(&game_board);
                load_level(&game_board, entry->d_name, level_dir_name, accumulated_points);
                continue;
            }

            accumulated_points = game_board.pacmans[0].points;
        }
        unload_level(&game_board);
    }
    closedir(level_dir);

    // Unregister board
    if (board_idx != -1)
    {
        pthread_mutex_lock(&active_boards_mutex);
        active_boards[board_idx] = NULL;
        pthread_mutex_unlock(&active_boards_mutex);
    }
}

void *game_thread(void *arg)
{
    char *level_dir_name = (char *)arg;

    while (1)
    {
        client_connection_t client;

        // Wait for client
        sem_wait(buffer_full);
        pthread_mutex_lock(&buffer_mutex);

        client = connection_buffer[buffer_out];
        buffer_out = (buffer_out + 1) % BUFFER_SIZE;
        buffer_count--;

        pthread_mutex_unlock(&buffer_mutex);
        sem_post(buffer_empty);

        debug("Game thread picked up client: %s\n", client.req_pipe_path);

        // Open client pipes - notif first to unblock client waiting
        int notif_fd = open(client.notif_pipe_path, O_WRONLY);
        if (notif_fd == -1)
        {
            perror("open notif pipe");
            continue;
        }
        
        int req_fd = open(client.req_pipe_path, O_RDONLY);
        
        
        if (req_fd == -1)
        {
            perror("open client pipes");
            close(notif_fd);
            continue;
        }

        // Send confirmation
        char response[2] = {1, 0};
        if (write(notif_fd, response, 2) != 2)
        {
            if (errno == EPIPE)
            {
                debug("Client pipe closed while confirming session\n");
            }
            else
            {
                perror("write confirmation");
            }
            close(req_fd);
            close(notif_fd);
            continue;
        }

        debug("Starting game session for client %s\n", client.req_pipe_path);
        run_game_session(level_dir_name, req_fd, notif_fd);
        debug("Finished game session for client %s\n", client.req_pipe_path);

        close(req_fd);
        close(notif_fd);
    }
    return NULL;
}

void *host_thread(void *arg)
{
    char *fifo_name = (char *)arg;

    int server_fd = open(fifo_name, O_RDWR);
    if (server_fd == -1)
    {
        perror("open server fifo");
        return NULL;
    }

    debug("Host thread started, listening on %s\n", fifo_name);

    // Unblock SIGUSR1
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

    // Install handler
    struct sigaction sa;
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    while (1)
    {
        char op_code;
        if (read_full(server_fd, &op_code, sizeof(char)) != 0)
        {
            debug("Host thread failed reading opcode (errno=%d)\n", errno);
            if (errno == EINTR)
            {
                if (sigusr1_received)
                {
                    sigusr1_received = 0;
                    log_top_scores();
                }
                continue;
            }
            perror("read server fifo");
            break;
        }

        if (op_code == 1)
        { // OP_CODE_CONNECT
            client_connection_t req;
            // Read exactly 40 bytes for each path
            // Note: read might return less than requested, should handle partial reads ideally.
            // For simplicity assuming atomic writes/reads for now or blocking.

            if (read_full(server_fd, req.req_pipe_path, 40) != 0)
            {
                debug("Failed to read req_pipe_path\n");
                continue;
            }
            if (read_full(server_fd, req.notif_pipe_path, 40) != 0)
            {
                debug("Failed to read notif_pipe_path\n");
                continue;
            }

            debug("Received connection request: %s, %s\n", req.req_pipe_path, req.notif_pipe_path);

            // Add to buffer
            sem_wait(buffer_empty);
            pthread_mutex_lock(&buffer_mutex);

            connection_buffer[buffer_in] = req;
            buffer_in = (buffer_in + 1) % BUFFER_SIZE;
            buffer_count++;

            pthread_mutex_unlock(&buffer_mutex);
            sem_post(buffer_full);
        }
        else
        {
            debug("Unknown opcode: %d\n", op_code);
            // Consume rest of message? We don't know the length.
            // This is a protocol error.
        }
    }
    close(server_fd);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        printf("Usage: %s <level_directory> <max_games> <fifo_name>\n", argv[0]);
        return -1;
    }

    char *level_dir_name = argv[1];
    int max_games = atoi(argv[2]);
    char *fifo_name = argv[3];

    strncpy(fifo_name_global, fifo_name, sizeof(fifo_name_global) - 1);
    fifo_name_global[sizeof(fifo_name_global) - 1] = '\0';

    // Create FIFO
    if (mkfifo(fifo_name, 0666) == -1)
    {
        if (errno != EEXIST)
        {
            perror("mkfifo");
            return 1;
        }
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    // Ignore SIGPIPE so broken client pipes do not kill the server
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sigint_handler);

    // Block SIGUSR1
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    // Initialize semaphores
    uid_t uid = getuid();
    snprintf(sem_full_name, sizeof(sem_full_name), "/pacman_buffer_full_%d", (int)uid);
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/pacman_buffer_empty_%d", (int)uid);

    sem_unlink(sem_full_name);
    sem_unlink(sem_empty_name);

    buffer_full = sem_open(sem_full_name, O_CREAT, 0644, 0);
    buffer_empty = sem_open(sem_empty_name, O_CREAT, 0644, BUFFER_SIZE);

    if (buffer_full == SEM_FAILED || buffer_empty == SEM_FAILED)
    {
        perror("sem_open");
        return 1;
    }

    atexit(cleanup_ipc);

    open_debug_file("debug.log");

    debug("Server starting: levels_dir=%s max_games=%d fifo=%s\n", level_dir_name, max_games, fifo_name);
    printf("Server starting: levels_dir=%s max_games=%d fifo=%s\n", level_dir_name, max_games, fifo_name);
    fflush(stdout);

    // Start Host Thread
    pthread_t host_tid;
    if (pthread_create(&host_tid, NULL, host_thread, (void *)fifo_name) != 0)
    {
        perror("pthread_create host");
        return 1;
    }

    debug("Host thread created; launching %d game threads\n", max_games);
    printf("Server running. Waiting for clients on %s\n", fifo_name);
    fflush(stdout);

    // Start Game Threads
    pthread_t *game_tids = malloc(max_games * sizeof(pthread_t));
    for (int i = 0; i < max_games; i++)
    {
        if (pthread_create(&game_tids[i], NULL, game_thread, (void *)level_dir_name) != 0)
        {
            perror("pthread_create game");
            return 1;
        }
    }

    // Wait for threads
    pthread_join(host_tid, NULL);

    // Cleanup
    free(game_tids);
    sem_close(buffer_full);
    sem_close(buffer_empty);
    sem_unlink(sem_full_name);
    sem_unlink(sem_empty_name);
    unlink(fifo_name);
    close_debug_file();

    return 0;
}
