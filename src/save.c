#define _POSIX_C_SOURCE 200809L
#include "save.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

/* Serialization format (simple):
 * [magic 4 bytes] 'P','S','V','1'
 * int width
 * int height
 * int tempo
 * int n_pacmans
 * int n_ghosts
 * board_pos_t array (width*height entries)
 * pacman_t array (n_pacmans entries)
 * ghost_t array (n_ghosts entries)
 * level_name (256 bytes)
 * pacman_file (256 bytes)
 * ghosts_files (MAX_GHOSTS * 256 bytes)
 */

static ssize_t full_write(int fd, const void *buf, size_t count)
{
    const char *p = buf;
    size_t written = 0;
    while (written < count)
    {
        ssize_t w = write(fd, p + written, count - written);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        written += w;
    }
    return written;
}

static ssize_t full_read(int fd, void *buf, size_t count)
{
    char *p = buf;
    size_t got = 0;
    while (got < count)
    {
        ssize_t r = read(fd, p + got, count - got);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        got += r;
    }
    return got;
}

int save_game(const char *path, board_t *board)
{
    if (!path || !board)
        return -1;
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return -errno;

    char magic[4] = {'P', 'S', 'V', '1'};
    if (full_write(fd, magic, sizeof(magic)) < 0)
        goto err;

    int tmp;
    tmp = board->width;
    if (full_write(fd, &tmp, sizeof(tmp)) < 0)
        goto err;
    tmp = board->height;
    if (full_write(fd, &tmp, sizeof(tmp)) < 0)
        goto err;
    tmp = board->tempo;
    if (full_write(fd, &tmp, sizeof(tmp)) < 0)
        goto err;
    tmp = board->n_pacmans;
    if (full_write(fd, &tmp, sizeof(tmp)) < 0)
        goto err;
    tmp = board->n_ghosts;
    if (full_write(fd, &tmp, sizeof(tmp)) < 0)
        goto err;

    size_t npos = (size_t)board->width * board->height;
    if (full_write(fd, board->board, sizeof(board_pos_t) * npos) < 0)
        goto err;

    if (board->n_pacmans > 0 && board->pacmans)
    {
        if (full_write(fd, board->pacmans, sizeof(pacman_t) * board->n_pacmans) < 0)
            goto err;
    }
    if (board->n_ghosts > 0 && board->ghosts)
    {
        if (full_write(fd, board->ghosts, sizeof(ghost_t) * board->n_ghosts) < 0)
            goto err;
    }

    char tmpbuf[256];
    memset(tmpbuf, 0, sizeof(tmpbuf));
    strncpy(tmpbuf, board->level_name, sizeof(tmpbuf) - 1);
    if (full_write(fd, tmpbuf, sizeof(tmpbuf)) < 0)
        goto err;
    memset(tmpbuf, 0, sizeof(tmpbuf));
    strncpy(tmpbuf, board->pacman_file, sizeof(tmpbuf) - 1);
    if (full_write(fd, tmpbuf, sizeof(tmpbuf)) < 0)
        goto err;

    for (int i = 0; i < MAX_GHOSTS; i++)
    {
        memset(tmpbuf, 0, sizeof(tmpbuf));
        strncpy(tmpbuf, board->ghosts_files[i], sizeof(tmpbuf) - 1);
        if (full_write(fd, tmpbuf, sizeof(tmpbuf)) < 0)
            goto err;
    }

    close(fd);
    return 0;
err:
    close(fd);
    unlink(path);
    return -1;
}

int load_game(const char *path, board_t *board)
{
    if (!path || !board)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;

    char magic[4];
    if (full_read(fd, magic, sizeof(magic)) != sizeof(magic))
        goto err;
    if (magic[0] != 'P' || magic[1] != 'S' || magic[2] != 'V')
        goto err;

    int width, height, tempo, n_pacmans, n_ghosts;
    if (full_read(fd, &width, sizeof(width)) != sizeof(width))
        goto err;
    if (full_read(fd, &height, sizeof(height)) != sizeof(height))
        goto err;
    if (full_read(fd, &tempo, sizeof(tempo)) != sizeof(tempo))
        goto err;
    if (full_read(fd, &n_pacmans, sizeof(n_pacmans)) != sizeof(n_pacmans))
        goto err;
    if (full_read(fd, &n_ghosts, sizeof(n_ghosts)) != sizeof(n_ghosts))
        goto err;

    size_t npos = (size_t)width * height;

    board->width = width;
    board->height = height;
    board->tempo = tempo;
    board->n_pacmans = n_pacmans;
    board->n_ghosts = n_ghosts;

    /* allocate or reallocate arrays */
    if (board->board)
        free(board->board);
    board->board = calloc(npos, sizeof(board_pos_t));
    if (!board->board)
        goto err;
    if (full_read(fd, board->board, sizeof(board_pos_t) * npos) != (ssize_t)(sizeof(board_pos_t) * npos))
        goto err;

    if (board->pacmans)
        free(board->pacmans);
    if (n_pacmans > 0)
    {
        board->pacmans = calloc(n_pacmans, sizeof(pacman_t));
        if (!board->pacmans)
            goto err;
        if (full_read(fd, board->pacmans, sizeof(pacman_t) * n_pacmans) != (ssize_t)(sizeof(pacman_t) * n_pacmans))
            goto err;
    }
    else
    {
        board->pacmans = NULL;
    }

    if (board->ghosts)
        free(board->ghosts);
    if (n_ghosts > 0)
    {
        board->ghosts = calloc(n_ghosts, sizeof(ghost_t));
        if (!board->ghosts)
            goto err;
        if (full_read(fd, board->ghosts, sizeof(ghost_t) * n_ghosts) != (ssize_t)(sizeof(ghost_t) * n_ghosts))
            goto err;
    }
    else
    {
        board->ghosts = NULL;
    }

    char tmpbuf[256];
    if (full_read(fd, tmpbuf, sizeof(tmpbuf)) != sizeof(tmpbuf))
        goto err;
    strncpy(board->level_name, tmpbuf, sizeof(board->level_name) - 1);
    if (full_read(fd, tmpbuf, sizeof(tmpbuf)) != sizeof(tmpbuf))
        goto err;
    strncpy(board->pacman_file, tmpbuf, sizeof(board->pacman_file) - 1);

    for (int i = 0; i < MAX_GHOSTS; i++)
    {
        if (full_read(fd, tmpbuf, sizeof(tmpbuf)) != sizeof(tmpbuf))
            goto err;
        strncpy(board->ghosts_files[i], tmpbuf, sizeof(board->ghosts_files[i]) - 1);
    }

    close(fd);
    return 0;
err:
    close(fd);
    return -1;
}
