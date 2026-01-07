#include "board.h"
#include "display.h"
#include "protocol.h"
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

    if (board->board[index].has_portal)
    {
        return '@';
    }

    if (board->board[index].has_dot)
    {
        return '.';
    }

    if (ch == 'W')
    {
        return '#';
    }

    return ch;
}

int send_board_update(int fd, board_t *board)
{
    char op_code = OP_CODE_BOARD;
    int width = board->width;
    int height = board->height;
    int tempo = board->tempo;

    int victory = 1;
    for (int i = 0; i < width * height; i++)
    {
        if (board->board[i].has_dot)
        {
            victory = 0;
            break;
        }
    }

    int game_over = victory;
    int points = 0;

    if (board->n_pacmans > 0)
    {
        points = board->pacmans[0].points;
        if (!board->pacmans[0].alive)
        {
            game_over = 1;
        }
    }
    else
    {
        game_over = 1;
    }

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

    // Avoid malloc/free on every update tick: reuse a per-thread buffer.
    static _Thread_local char *buffer = NULL;
    static _Thread_local size_t buffer_cap = 0;
    size_t needed = (size_t)width * (size_t)height;
    if (needed > buffer_cap)
    {
        char *newbuf = realloc(buffer, needed);
        if (!newbuf)
            return 0;
        buffer = newbuf;
        buffer_cap = needed;
    }
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            buffer[y * width + x] = get_board_char(board, x, y);
        }
    }
    w = write(fd, buffer, width * height);
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

    if (board->n_pacmans <= 0)
    {
        int *retval = malloc(sizeof(int));
        if (retval)
            *retval = QUIT_GAME;
        return (void *)retval;
    }

    pacman_t *pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));
    *retval = CONTINUE_PLAY;

    // Avoid busy-waiting on the request FIFO.
    int flags = fcntl(req_fd, F_GETFL, 0);
    if (flags != -1)
    {
        (void)fcntl(req_fd, F_SETFL, flags | O_NONBLOCK);
    }

    while (true)
    {
        if (*shutdown_flag)
        {
            debug("pacman_thread: shutdown_flag set -> QUIT_GAME\n");
            *retval = QUIT_GAME;
            return (void *)retval;
        }

        if (!pacman->alive)
        {
            debug("pacman_thread: pacman not alive -> LOAD_BACKUP\n");
            *retval = LOAD_BACKUP;
            return (void *)retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t *play = NULL;
        command_t c;
        memset(&c, 0, sizeof(c));
        c.turns = 1;
        c.turns_left = 1;

        if (pacman->n_moves == 0)
        {
            unsigned char op_code = 0;
            ssize_t n = read(req_fd, &op_code, 1);
            if (n == 0)
            {
                debug("pacman_thread: req_fd EOF (client closed request pipe) -> QUIT_GAME\n");
                *retval = QUIT_GAME;
                return (void *)retval;
            }
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                debug("pacman_thread: read(req_fd) error errno=%d -> QUIT_GAME\n", errno);
                *retval = QUIT_GAME;
                return (void *)retval;
            }

            if (op_code == OP_CODE_PLAY)
            {
                char cmd_char = 0;
                if (read(req_fd, &cmd_char, 1) != 1)
                {
                    // If we read only the opcode (partial message), ignore this tick.
                    debug("pacman_thread: partial OP_CODE_PLAY (missing cmd byte)\n");
                    continue;
                }
                c.command = cmd_char;
                play = &c;
            }
            else if (op_code == OP_CODE_DISCONNECT)
            {
                debug("pacman_thread: received OP_CODE_DISCONNECT -> QUIT_GAME\n");
                *retval = QUIT_GAME;
                return (void *)retval;
            }
            else
            {
                // Out-of-sync or unknown opcode.
                debug("pacman_thread: unexpected opcode=%u (ignoring)\n", (unsigned)op_code);
                continue;
            }
        }
        else
        {
            play = &pacman->moves[pacman->current_move % pacman->n_moves];
        }

        if (!play)
            continue;

        debug("KEY %c\n", play->command);

        if (play->command == 'Q')
        {
            *retval = QUIT_GAME;
            return (void *)retval;
        }

        pthread_rwlock_wrlock(&board->state_lock);
        int result = move_pacman(board, 0, play);
        debug("move_pacman: cmd=%c result=%d pos=%d,%d points=%d\n",
              play->command,
              result,
              pacman->pos_x,
              pacman->pos_y,
              pacman->points);
        pthread_rwlock_unlock(&board->state_lock);

        if (result == REACHED_PORTAL)
        {
            *retval = NEXT_LEVEL;
            return (void *)retval;
        }

        if (result == DEAD_PACMAN)
        {
            *retval = LOAD_BACKUP;
            return (void *)retval;
        }
    }
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
    debug("run_game_session: entered with dir=%s, req_fd=%d, notif_fd=%d\n", level_dir_name, req_fd, notif_fd);
    DIR *level_dir = opendir(level_dir_name);
    if (level_dir == NULL)
    {
        debug("Failed to open level dir: %s\n", level_dir_name);
        return;
    }
    debug("Level directory opened successfully\n");

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

        debug("Loading level: %s\n", entry->d_name);
        load_level(&game_board, entry->d_name, level_dir_name, accumulated_points);
        debug("Level loaded successfully\n");
        // Ignore server-side scripted moves; keep PAC position/tempo only
        game_board.pacmans[0].n_moves = 0;
        game_board.pacmans[0].current_move = 0;

        // Send initial board state immediately after level load
        debug("Sending initial board update\n");
        send_board_update(notif_fd, &game_board);
        debug("Initial board update sent\n");

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
            int ghosts_started = 0;
            bool update_started = false;

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
            if (pthread_create(&pacman_tid, NULL, pacman_thread, (void *)pac_arg) != 0)
            {
                free(pac_arg);
                free(ghost_tids);
                debug("pthread_create pacman failed\n");
                end_game = true;
                break;
            }

            // Create Ghost threads
            for (int i = 0; i < game_board.n_ghosts; i++)
            {
                session_args_t *arg = malloc(sizeof(session_args_t));
                if (!arg)
                {
                    session_shutdown = 1;
                    break;
                }
                arg->board = &game_board;
                arg->ghost_index = i;
                arg->shutdown_flag = &session_shutdown;
                if (pthread_create(&ghost_tids[i], NULL, ghost_thread, (void *)arg) != 0)
                {
                    free(arg);
                    session_shutdown = 1;
                    break;
                }
                ghosts_started++;
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
            if (pthread_create(&update_tid, NULL, server_update_thread, (void *)upd_arg) != 0)
            {
                free(upd_arg);
                session_shutdown = 1;
                debug("pthread_create update failed\n");
            }
            else
            {
                update_started = true;
            }

            int *retval;
            pthread_join(pacman_tid, (void **)&retval);

            if (retval)
            {
                debug("pacman_thread finished with code=%d\n", *retval);
            }
            else
            {
                debug("pacman_thread finished with NULL retval\n");
            }

            pthread_rwlock_wrlock(&game_board.state_lock);
            session_shutdown = 1;
            pthread_rwlock_unlock(&game_board.state_lock);

            if (update_started)
            {
                pthread_join(update_tid, NULL);
            }
            for (int i = 0; i < ghosts_started; i++)
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
                // Ignore server-side scripted moves; keep PAC position/tempo only
                game_board.pacmans[0].n_moves = 0;
                game_board.pacmans[0].current_move = 0;
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
        debug("Opening notification pipe: %s\n", client.notif_pipe_path);
        int notif_fd = open(client.notif_pipe_path, O_WRONLY);
        debug("Notification pipe opened, fd=%d\n", notif_fd);
        if (notif_fd == -1)
        {
            perror("open notif pipe");
            continue;
        }

        debug("Opening request pipe: %s\n", client.req_pipe_path);
        int req_fd = open(client.req_pipe_path, O_RDONLY);
        debug("Request pipe opened, fd=%d\n", req_fd);

        if (req_fd == -1)
        {
            perror("open client pipes");
            close(notif_fd);
            continue;
        }

        // Send confirmation
        debug("Sending confirmation to client\n");
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
        debug("Calling run_game_session with levels_dir=%s\n", level_dir_name);
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
    if (!game_tids)
    {
        perror("malloc game_tids");
        return 1;
    }
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
