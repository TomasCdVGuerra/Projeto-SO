#include "board.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

static int debug_fd = -1;

static void lock_two_positions(board_t *board, int idx1, int idx2)
{
    int first = idx1;
    int second = idx2;
    if (first > second)
    {
        int tmp = first;
        first = second;
        second = tmp;
    }

    /* Keep lock order stable to avoid deadlock. */
    pthread_mutex_lock(&board->board[first].pos_mutex);
    if (second != first)
    {
        pthread_mutex_lock(&board->board[second].pos_mutex);
    }
}

static void unlock_two_positions(board_t *board, int idx1, int idx2)
{
    int first = idx1;
    int second = idx2;
    if (first > second)
    {
        int tmp = first;
        first = second;
        second = tmp;
    }

    if (second != first)
    {
        pthread_mutex_unlock(&board->board[second].pos_mutex);
    }
    pthread_mutex_unlock(&board->board[first].pos_mutex);
}

void barrier_init(simple_barrier_t *barrier, int count)
{
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    barrier->count = 0;
    barrier->threshold = count;
    barrier->generation = 0;
}

void barrier_wait(simple_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);

    int my_generation = barrier->generation;

    if (++barrier->count >= barrier->threshold)
    {
        barrier->count = 0;
        barrier->generation++;
        pthread_cond_broadcast(&barrier->cond);
    }
    else
    {
        while (my_generation == barrier->generation)
        {
            pthread_cond_wait(&barrier->cond, &barrier->mutex);
        }
    }

    pthread_mutex_unlock(&barrier->mutex);
}

void barrier_destroy(simple_barrier_t *barrier)
{
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
}

static int find_and_kill_pacman(board_t *board, int new_x, int new_y)
{
    for (int p = 0; p < board->n_pacmans; p++)
    {
        pacman_t *pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive)
        {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

static inline int get_board_index(board_t *board, int x, int y)
{
    return y * board->width + x;
}

static inline int is_valid_position(board_t *board, int x, int y)
{
    return (x >= 0 && x < board->width && y >= 0 && y < board->height);
}

void sleep_ms(int milliseconds)
{
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int move_pacman(board_t *board, int pacman_index, command_t *command)
{
    if (pacman_index < 0 || !board->pacmans[pacman_index].alive)
        return DEAD_PACMAN;

    pacman_t *pac = &board->pacmans[pacman_index];
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    if (pac->waiting > 0)
    {
        pac->waiting -= 1;
        return VALID_MOVE;
    }
    pac->waiting = pac->passo;

    char direction = command->command;
    if (direction == 'R')
    {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    switch (direction)
    {
    case 'W':
        new_y--;
        break;
    case 'S':
        new_y++;
        break;
    case 'A':
        new_x--;
        break;
    case 'D':
        new_x++;
        break;
    case 'T':
        if (command->turns_left == 1)
        {
            pac->current_move += 1;
            command->turns_left = command->turns;
        }
        else
        {
            command->turns_left -= 1;
        }
        return VALID_MOVE;
    default:
        return INVALID_MOVE;
    }

    pac->current_move += 1;

    if (!is_valid_position(board, new_x, new_y))
        return INVALID_MOVE;

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);

    lock_two_positions(board, old_index, new_index);

    char target_content = board->board[new_index].content;

    if (board->board[new_index].has_portal)
    {
        board->board[old_index].content = ' ';
        board->board[new_index].content = 'P';
        unlock_two_positions(board, old_index, new_index);
        return REACHED_PORTAL;
    }

    if (target_content == 'W')
    {
        unlock_two_positions(board, old_index, new_index);
        return INVALID_MOVE;
    }

    if (target_content == 'M')
    {
        unlock_two_positions(board, old_index, new_index);
        kill_pacman(board, pacman_index);
        return DEAD_PACMAN;
    }

    if (board->board[new_index].has_dot)
    {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    board->board[old_index].content = ' ';
    pac->pos_x = new_x;
    pac->pos_y = new_y;
    board->board[new_index].content = 'P';

    unlock_two_positions(board, old_index, new_index);

    return VALID_MOVE;
}

static int move_ghost_charged_direction(board_t *board, ghost_t *ghost, char direction, int *new_x, int *new_y)
{
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    *new_x = x;
    *new_y = y;

    switch (direction)
    {
    case 'W':
        if (y == 0)
            return INVALID_MOVE;
        *new_y = 0;
        for (int i = y - 1; i >= 0; i--)
        {
            char target_content = board->board[get_board_index(board, x, i)].content;
            if (target_content == 'W' || target_content == 'M')
            {
                *new_y = i + 1;
                return VALID_MOVE;
            }
            else if (target_content == 'P')
            {
                *new_y = i;
                return find_and_kill_pacman(board, *new_x, *new_y);
            }
        }
        break;

    case 'S':
        if (y == board->height - 1)
            return INVALID_MOVE;
        *new_y = board->height - 1;
        for (int i = y + 1; i < board->height; i++)
        {
            char target_content = board->board[get_board_index(board, x, i)].content;
            if (target_content == 'W' || target_content == 'M')
            {
                *new_y = i - 1;
                return VALID_MOVE;
            }
            if (target_content == 'P')
            {
                *new_y = i;
                return find_and_kill_pacman(board, *new_x, *new_y);
            }
        }
        break;

    case 'A':
        if (x == 0)
            return INVALID_MOVE;
        *new_x = 0;
        for (int j = x - 1; j >= 0; j--)
        {
            char target_content = board->board[get_board_index(board, j, y)].content;
            if (target_content == 'W' || target_content == 'M')
            {
                *new_x = j + 1;
                return VALID_MOVE;
            }
            if (target_content == 'P')
            {
                *new_x = j;
                return find_and_kill_pacman(board, *new_x, *new_y);
            }
        }
        break;

    case 'D':
        if (x == board->width - 1)
            return INVALID_MOVE;
        *new_x = board->width - 1;
        for (int j = x + 1; j < board->width; j++)
        {
            char target_content = board->board[get_board_index(board, j, y)].content;
            if (target_content == 'W' || target_content == 'M')
            {
                *new_x = j - 1;
                return VALID_MOVE;
            }
            if (target_content == 'P')
            {
                *new_x = j;
                return find_and_kill_pacman(board, *new_x, *new_y);
            }
        }
        break;
    default:
        debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
        return INVALID_MOVE;
    }
    return VALID_MOVE;
}

int move_ghost_charged(board_t *board, int ghost_index, char direction)
{
    ghost_t *ghost = &board->ghosts[ghost_index];
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    int new_x = x;
    int new_y = y;

    ghost->charged = 0;
    int result = move_ghost_charged_direction(board, ghost, direction, &new_x, &new_y);
    if (result == INVALID_MOVE)
    {
        debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
        return INVALID_MOVE;
    }

    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    int new_index = get_board_index(board, new_x, new_y);

    lock_two_positions(board, old_index, new_index);

    board->board[old_index].content = ' ';
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    board->board[new_index].content = 'M';

    unlock_two_positions(board, old_index, new_index);

    return result;
}

int move_ghost(board_t *board, int ghost_index, command_t *command)
{
    ghost_t *ghost = &board->ghosts[ghost_index];
    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    if (ghost->waiting > 0)
    {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;

    if (direction == 'R')
    {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    switch (direction)
    {
    case 'W':
        new_y--;
        break;
    case 'S':
        new_y++;
        break;
    case 'A':
        new_x--;
        break;
    case 'D':
        new_x++;
        break;
    case 'C':
        ghost->current_move += 1;
        ghost->charged = 1;
        return VALID_MOVE;
    case 'T':
        if (command->turns_left == 1)
        {
            ghost->current_move += 1;
            command->turns_left = command->turns;
        }
        else
            command->turns_left -= 1;
        return VALID_MOVE;
    default:
        return INVALID_MOVE;
    }
    ghost->current_move++;
    if (ghost->charged)
        return move_ghost_charged(board, ghost_index, direction);

    if (!is_valid_position(board, new_x, new_y))
    {
        return INVALID_MOVE;
    }

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);

    lock_two_positions(board, old_index, new_index);

    char target_content = board->board[new_index].content;

    if (target_content == 'W' || target_content == 'M')
    {
        unlock_two_positions(board, old_index, new_index);
        return INVALID_MOVE;
    }

    int result = VALID_MOVE;
    if (target_content == 'P')
    {
        result = find_and_kill_pacman(board, new_x, new_y);
    }

    board->board[old_index].content = ' ';

    ghost->pos_x = new_x;
    ghost->pos_y = new_y;

    board->board[new_index].content = 'M';

    unlock_two_positions(board, old_index, new_index);

    return result;
}

void kill_pacman(board_t *board, int pacman_index)
{
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t *pac = &board->pacmans[pacman_index];
    int index = pac->pos_y * board->width + pac->pos_x;

    board->board[index].content = ' ';

    pac->alive = 0;
}

int load_pacman(board_t *board, int points)
{
    board->board[1 * board->width + 1].content = 'P';
    board->pacmans[0].pos_x = 1;
    board->pacmans[0].pos_y = 1;
    board->pacmans[0].alive = 1;
    board->pacmans[0].points = points;
    return 0;
}

int load_ghost(board_t *board)
{
    board->board[3 * board->width + 1].content = 'M';
    board->ghosts[0].pos_x = 1;
    board->ghosts[0].pos_y = 3;
    board->ghosts[0].passo = 0;
    board->ghosts[0].waiting = 0;
    board->ghosts[0].current_move = 0;
    board->ghosts[0].n_moves = 16;
    for (int i = 0; i < 8; i++)
    {
        board->ghosts[0].moves[i].command = 'D';
        board->ghosts[0].moves[i].turns = 1;
    }
    for (int i = 8; i < 16; i++)
    {
        board->ghosts[0].moves[i].command = 'A';
        board->ghosts[0].moves[i].turns = 1;
    }

    board->board[2 * board->width + 4].content = 'M';
    board->ghosts[1].pos_x = 4;
    board->ghosts[1].pos_y = 2;
    board->ghosts[1].passo = 1;
    board->ghosts[1].waiting = 1;
    board->ghosts[1].current_move = 0;
    board->ghosts[1].n_moves = 1;
    board->ghosts[1].moves[0].command = 'R';
    board->ghosts[1].moves[0].turns = 1;

    return 0;
}

int load_level(board_t *board, int points)
{
    board->height = 5;
    board->width = 10;
    board->tempo = 10;

    board->n_ghosts = 2;
    board->n_pacmans = 1;

    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    for (int i = 0; i < board->width * board->height; i++)
    {
        pthread_mutex_init(&board->board[i].pos_mutex, NULL);
    }

    strncpy(board->level_name, "Static Level", sizeof(board->level_name) - 1);
    board->level_name[sizeof(board->level_name) - 1] = '\0';

    for (int i = 0; i < board->height; i++)
    {
        for (int j = 0; j < board->width; j++)
        {
            if (i == 0 || j == 0 || j == (board->width - 1))
            {
                board->board[i * board->width + j].content = 'W';
            }
            else if (i == 4 && j == 8)
            {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_portal = 1;
            }
            else
            {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_dot = 1;
            }
        }
    }

    load_ghost(board);
    load_pacman(board, points);

    return 0;
}

void unload_level(board_t *board)
{
    for (int i = 0; i < board->width * board->height; i++)
    {
        pthread_mutex_destroy(&board->board[i].pos_mutex);
    }
    free(board->board);
    free(board->pacmans);
    free(board->ghosts);
}

void open_debug_file(char *filename)
{
    if (!filename)
        return;
    if (debug_fd >= 0)
        close(debug_fd);
    debug_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
}

void close_debug_file()
{
    if (debug_fd >= 0)
    {
        close(debug_fd);
        debug_fd = -1;
    }
}

void debug(const char *format, ...)
{
    if (debug_fd < 0)
        return;
    va_list args;
    va_start(args, format);

    /* minimal formatter: supports %s, %d, %c */
    const char *p = format;
    char out[1024];
    size_t off = 0;
    while (*p && off + 1 < sizeof(out))
    {
        if (*p == '%')
        {
            p++;
            if (*p == 's')
            {
                const char *s = va_arg(args, const char *);
                if (!s)
                    s = "(null)";
                size_t l = strlen(s);
                size_t can = sizeof(out) - off - 1;
                if (l > can)
                    l = can;
                memcpy(out + off, s, l);
                off += l;
                p++;
                continue;
            }
            else if (*p == 'd')
            {
                int v = va_arg(args, int);
                char tmp[32];
                int tp = 0;
                if (v == 0)
                    tmp[tp++] = '0';
                else
                {
                    int neg = v < 0;
                    unsigned int u = neg ? (unsigned int)(-v) : (unsigned int)v;
                    char rev[32];
                    int r = 0;
                    while (u > 0 && r < (int)sizeof(rev))
                    {
                        rev[r++] = '0' + (u % 10);
                        u /= 10;
                    }
                    if (neg)
                        tmp[tp++] = '-';
                    for (int i = r - 1; i >= 0; --i)
                        tmp[tp++] = rev[i];
                }
                size_t can = sizeof(out) - off - 1;
                size_t copy = tp < (int)can ? tp : can;
                memcpy(out + off, tmp, copy);
                off += copy;
                p++;
                continue;
            }
            else if (*p == 'c')
            {
                int c = va_arg(args, int);
                if (off + 1 < sizeof(out))
                    out[off++] = (char)c;
                p++;
                continue;
            }
            else
            {
                /* unsupported, copy verbatim */
                if (off + 1 < sizeof(out))
                    out[off++] = '%';
                /* do not advance p here so next loop handles it */
            }
        }
        out[off++] = *p++;
    }
    va_end(args);
    if (off >= sizeof(out))
        off = sizeof(out) - 1;
    out[off] = '\0';
    write(debug_fd, out, off);
}

void print_board(board_t *board)
{
    if (!board || !board->board)
    {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    char buffer[8192];
    size_t offset = 0;
    /* Header */
    {
        char tmp[256];
        size_t t = 0;
        /* === [pid] LEVEL INFO ===\n */
        const char *h1 = "=== [";
        size_t can = sizeof(tmp) - t - 1;
        size_t l = strlen(h1) < can ? strlen(h1) : can;
        memcpy(tmp + t, h1, l);
        t += l;
        /* pid */
        {
            int pid = getpid();
            char num[32];
            int np = 0;
            if (pid == 0)
                num[np++] = '0';
            else
            {
                unsigned int u = (unsigned int)pid;
                char rev[32];
                int r = 0;
                while (u)
                {
                    rev[r++] = '0' + (u % 10);
                    u /= 10;
                }
                for (int i = r - 1; i >= 0; --i)
                    num[np++] = rev[i];
            }
            if (t + np < sizeof(tmp) - 1)
            {
                memcpy(tmp + t, num, np);
                t += np;
            }
        }
        const char *h2 = "] LEVEL INFO ===\n";
        can = sizeof(tmp) - t - 1;
        l = strlen(h2) < can ? strlen(h2) : can;
        memcpy(tmp + t, h2, l);
        t += l;
        tmp[t] = '\0';
        size_t canb = sizeof(buffer) - offset - 1;
        size_t copy = t < canb ? t : canb;
        memcpy(buffer + offset, tmp, copy);
        offset += copy;
    }

    /* Dimensions / Tempo / Pacman file */
    {
        char tmp[256];
        size_t t = 0;
        const char *s1 = "Dimensions: ";
        size_t l = strlen(s1);
        memcpy(tmp + t, s1, l);
        t += l;
        /* height */ {
            char num[32];
            int np = 0;
            int v = board->height;
            if (v == 0)
                num[np++] = '0';
            else
            {
                unsigned int u = v;
                char rev[32];
                int r = 0;
                while (u)
                {
                    rev[r++] = '0' + (u % 10);
                    u /= 10;
                }
                for (int i = r - 1; i >= 0; --i)
                    num[np++] = rev[i];
            }
            memcpy(tmp + t, num, np);
            t += np;
        }
        const char *x = " x ";
        memcpy(tmp + t, x, 3);
        t += 3;
        /* width */ {
            char num[32];
            int np = 0;
            int v = board->width;
            if (v == 0)
                num[np++] = '0';
            else
            {
                unsigned int u = v;
                char rev[32];
                int r = 0;
                while (u)
                {
                    rev[r++] = '0' + (u % 10);
                    u /= 10;
                }
                for (int i = r - 1; i >= 0; --i)
                    num[np++] = rev[i];
            }
            memcpy(tmp + t, num, np);
            t += np;
        }
        const char *nl = "\n";
        memcpy(tmp + t, nl, 1);
        t += 1;
        const char *s2 = "Tempo: ";
        memcpy(tmp + t, s2, 7);
        t += 7;
        {
            char num[32];
            int np = 0;
            int v = board->tempo;
            if (v == 0)
                num[np++] = '0';
            else
            {
                unsigned int u = v;
                char rev[32];
                int r = 0;
                while (u)
                {
                    rev[r++] = '0' + (u % 10);
                    u /= 10;
                }
                for (int i = r - 1; i >= 0; --i)
                    num[np++] = rev[i];
            }
            memcpy(tmp + t, num, np);
            t += np;
        }
        memcpy(tmp + t, "\nPacman file: ", 14);
        t += 14;
        size_t name_len = strlen(board->pacman_file);
        size_t can = sizeof(tmp) - t - 2;
        size_t copy = name_len < can ? name_len : can;
        memcpy(tmp + t, board->pacman_file, copy);
        t += copy;
        tmp[t++] = '\n';
        tmp[t] = '\0';
        size_t canb = sizeof(buffer) - offset - 1;
        size_t c = t < canb ? t : canb;
        memcpy(buffer + offset, tmp, c);
        offset += c;
    }

    /* Monster files header */
    {
        char tmp[64];
        size_t t = 0;
        const char *s = "Monster files (";
        memcpy(tmp + t, s, strlen(s));
        t += strlen(s);
        /* n_ghosts */ {
            char num[16];
            int np = 0;
            int v = board->n_ghosts;
            if (v == 0)
                num[np++] = '0';
            else
            {
                unsigned int u = v;
                char rev[16];
                int r = 0;
                while (u)
                {
                    rev[r++] = '0' + (u % 10);
                    u /= 10;
                }
                for (int i = r - 1; i >= 0; --i)
                    num[np++] = rev[i];
            }
            memcpy(tmp + t, num, np);
            t += np;
        }
        memcpy(tmp + t, "):\n", 3);
        t += 3;
        size_t canb = sizeof(buffer) - offset - 1;
        size_t c = t < canb ? t : canb;
        memcpy(buffer + offset, tmp, c);
        offset += c;
    }
    for (int i = 0; i < board->n_ghosts; i++)
    {
        const char *pref = "  - ";
        size_t l = strlen(pref);
        size_t canb = sizeof(buffer) - offset - 1;
        size_t c = l < canb ? l : canb;
        memcpy(buffer + offset, pref, c);
        offset += c;
        size_t name_len = strlen(board->ghosts_files[i]);
        canb = sizeof(buffer) - offset - 1;
        c = name_len < canb ? name_len : canb;
        memcpy(buffer + offset, board->ghosts_files[i], c);
        offset += c;
        if (offset < sizeof(buffer) - 1)
            buffer[offset++] = '\n';
    }

    /* Board */
    {
        const char *hdr = "\n=== BOARD ===\n";
        size_t l = strlen(hdr);
        size_t canb = sizeof(buffer) - offset - 1;
        size_t c = l < canb ? l : canb;
        memcpy(buffer + offset, hdr, c);
        offset += c;
    }
    for (int y = 0; y < board->height; y++)
    {
        for (int x = 0; x < board->width; x++)
        {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2)
            {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2)
        {
            buffer[offset++] = '\n';
        }
    }
    {
        const char *end = "==================\n";
        size_t l = strlen(end);
        size_t canb = sizeof(buffer) - offset - 1;
        size_t c = l < canb ? l : canb;
        memcpy(buffer + offset, end, c);
        offset += c;
    }
    buffer[offset] = '\0';
    debug("%s", buffer);
}
