/* loader.c - POSIX-based loader implementation
 * Scans a directory for .lvl files and parses .lvl, .m, .p files using
 * POSIX file APIs (`open`, `read`) to populate `board_t` structures.
 */

#include "loader.h"
#include "board.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#define MAX_LEVEL_FILES 128

static int initialized = 0;
static char *loader_dir = NULL;
static char *level_paths[MAX_LEVEL_FILES];
static int level_count = 0;
static int current_level = 0;

static int has_suffix(const char *name, const char *suf)
{
    size_t n = strlen(name);
    size_t m = strlen(suf);
    if (n < m)
        return 0;
    return strcmp(name + n - m, suf) == 0;
}

/* Compare two level paths by optional numeric prefix in the basename.
 * If both basenames start with a number, compare numerically; otherwise
 * fall back to lexicographic compare of the basename.
 */
static int cmp_level_paths(const void *a, const void *b)
{
    const char *A = *(const char **)a;
    const char *B = *(const char **)b;

    /* get basenames (after last '/') */
    const char *ba = strrchr(A, '/');
    const char *bb = strrchr(B, '/');
    ba = ba ? ba + 1 : A;
    bb = bb ? bb + 1 : B;

    /* try parse leading integers */
    char *endptr;
    long na = strtol(ba, &endptr, 10);
    int a_has_num = (endptr != ba);
    char *endptr2;
    long nb = strtol(bb, &endptr2, 10);
    int b_has_num = (endptr2 != bb);

    if (a_has_num && b_has_num)
    {
        if (na < nb)
            return -1;
        if (na > nb)
            return 1;
        /* equal numeric prefix: fall back to strcmp on whole basename */
        return strcmp(ba, bb);
    }

    /* If only one has a number, put the numbered one first */
    if (a_has_num && !b_has_num)
        return -1;
    if (!a_has_num && b_has_num)
        return 1;

    /* neither have numbers: lexicographic compare of basenames */
    return strcmp(ba, bb);
}

int init_level_loader(const char *dirpath)
{
    if (!dirpath)
        return -1;

    struct stat st;
    if (stat(dirpath, &st) != 0)
    {
        return -errno;
    }

    if (!S_ISDIR(st.st_mode))
    {
        return -1;
    }

    DIR *d = opendir(dirpath);
    if (!d)
        return -errno;

    /* Log which directory we are scanning for debugging */
    debug("init_level_loader: scanning dir '%s'\n", dirpath);

    /* Save a copy of the directory */
    loader_dir = strdup(dirpath);
    if (!loader_dir)
    {
        closedir(d);
        return -1;
    }

    /* Collect .lvl files */
    struct dirent *ent;
    level_count = 0;
    while ((ent = readdir(d)) != NULL)
    {
        /* Some filesystems/platforms may not provide d_type; skip '.' and '..' and rely
         * on suffix checks for regular files. This avoids using DT_DIR which may be
         * undefined on some systems. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (has_suffix(ent->d_name, ".lvl"))
        {
            if (level_count >= MAX_LEVEL_FILES)
                break;
            size_t len = strlen(dirpath) + 1 + strlen(ent->d_name) + 1;
            char *full = malloc(len);
            if (!full)
                continue;
            /* construct path: dirpath + '/' + name + '\0' */
            char *fp = full;
            strcpy(fp, dirpath);
            fp += strlen(dirpath);
            *fp++ = '/';
            strcpy(fp, ent->d_name);
            level_paths[level_count++] = full;
        }
    }
    closedir(d);

    /* Report discovered levels to debug log */
    debug("init_level_loader: found %d .lvl files\n", level_count);
    for (int i = 0; i < level_count; i++)
    {
        debug("  - %s\n", level_paths[i]);
    }

    if (level_count == 0)
    {
        /* no levels found */
        free(loader_dir);
        loader_dir = NULL;
        return -2;
    }

    /* sort level paths by numeric prefix if present, otherwise lexicographically */
    qsort(level_paths, level_count, sizeof(char *), cmp_level_paths);

    initialized = 1;
    current_level = 0;
    return 0;
}

void reset_level_iterator(void)
{
    current_level = 0;
}

void cleanup_level_loader(void)
{
    for (int i = 0; i < level_count; i++)
    {
        free(level_paths[i]);
        level_paths[i] = NULL;
    }
    level_count = 0;
    current_level = 0;
    if (loader_dir)
    {
        free(loader_dir);
        loader_dir = NULL;
    }
    initialized = 0;
}

int loader_is_initialized(void)
{
    return initialized;
}

/* Helper to read an entire file into a nul-terminated buffer using POSIX calls.
 * Caller must free the returned pointer. Returns NULL on error.
 */
static char *read_file_to_string(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0)
    {
        size_t sz = (size_t)st.st_size;
        char *buf = malloc(sz + 1);
        if (!buf)
        {
            close(fd);
            return NULL;
        }
        ssize_t r = read(fd, buf, sz);
        if (r < 0)
        {
            free(buf);
            close(fd);
            return NULL;
        }
        buf[r] = '\0';
        close(fd);
        return buf;
    }

    /* fallback to dynamic read */
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf)
    {
        close(fd);
        return NULL;
    }
    size_t len = 0;
    while (1)
    {
        if (len + 1024 > cap)
        {
            cap *= 2;
            char *n = realloc(buf, cap);
            if (!n)
            {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = n;
        }
        ssize_t r = read(fd, buf + len, 1024);
        if (r < 0)
        {
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        len += r;
    }
    buf[len] = '\0';
    close(fd);
    return buf;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

/* Parse behavior file (.m or .p) and populate the provided move structure
 * on the target pacman_t or ghost_t. We will parse PASSO, POS and the
 * cyclic commands. Returns 0 on success.
 */
static int parse_behavior(const char *base_dir, const char *filename, int is_pacman, void *target)
{
    /* Security: validate filename doesn't contain path separators */
    if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL)
    {
        debug("parse_behavior: rejecting filename with path separators: %s\n", filename);
        return -1;
    }

    size_t len = strlen(base_dir) + 1 + strlen(filename) + 1;
    char *path = malloc(len);
    if (!path)
        return -1;
    char *pp = path;
    strcpy(pp, base_dir);
    pp += strlen(base_dir);
    *pp++ = '/';
    strcpy(pp, filename);
    char *content = read_file_to_string(path);
    free(path);
    if (!content)
        return -1;

    char *saveptr;
    char *line = strtok_r(content, "\n", &saveptr);
    int passo = 0;
    int pos_r = -1, pos_c = -1;
    int move_idx = 0;

    while (line)
    {
        char *t = trim(line);
        if (*t == '\0' || *t == '#')
        {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (strncmp(t, "PASSO", 5) == 0)
        {
            const char *q = t + 5;
            while (*q && isspace((unsigned char)*q))
                q++;
            char *endptr = NULL;
            long v = strtol(q, &endptr, 10);
            /* Validate PASSO is reasonable */
            if (endptr != q && v >= 0 && v <= MAX_PASSO_VALUE)
                passo = (int)v;
        }
        else if (strncmp(t, "POS", 3) == 0)
        {
            const char *q = t + 3;
            while (*q && isspace((unsigned char)*q))
                q++;
            char *endptr = NULL;
            long r = strtol(q, &endptr, 10);
            if (endptr != q)
            {
                q = endptr;
                while (*q && isspace((unsigned char)*q))
                    q++;
                long c = strtol(q, &endptr, 10);
                if (endptr != q)
                {
                    pos_r = (int)r;
                    pos_c = (int)c;
                }
            }
        }
        else
        {
            /* movement command line */
            if (move_idx < MAX_MOVES)
            {
                command_t cmd;
                cmd.turns = 1;
                cmd.turns_left = 1;
                if (t[0] == 'T')
                {
                    const char *q = t + 1;
                    while (*q && isspace((unsigned char)*q))
                        q++;
                    char *endptr = NULL;
                    long n = strtol(q, &endptr, 10);
                    int ni = (endptr != q && n > 0) ? (int)n : 1;
                    cmd.command = 'T';
                    cmd.turns = ni;
                    cmd.turns_left = cmd.turns;
                }
                else
                {
                    cmd.command = t[0];
                }
                if (is_pacman)
                {
                    pacman_t *p = (pacman_t *)target;
                    p->moves[move_idx] = cmd;
                }
                else
                {
                    ghost_t *g = (ghost_t *)target;
                    g->moves[move_idx] = cmd;
                }
                move_idx++;
            }
            else
            {
                /* Log when moves exceed MAX_MOVES */
                debug("parse_behavior: move count exceeded MAX_MOVES (%d), truncating\n", MAX_MOVES);
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (is_pacman)
    {
        pacman_t *p = (pacman_t *)target;
        p->passo = passo;
        p->n_moves = move_idx;
        p->current_move = 0;
        p->waiting = 0;
        if (pos_r >= 0 && pos_c >= 0)
        {
            p->pos_x = pos_c;
            p->pos_y = pos_r;
        }
    }
    else
    {
        ghost_t *g = (ghost_t *)target;
        g->passo = passo;
        g->n_moves = move_idx;
        g->current_move = 0;
        g->waiting = 0;
        g->charged = 0;
        if (pos_r >= 0 && pos_c >= 0)
        {
            g->pos_x = pos_c;
            g->pos_y = pos_r;
        }
    }

    free(content);
    return 0;
}

/* Parse a .lvl file and populate board_t accordingly. Returns 0 on success. */
static int parse_lvl_to_board(const char *lvlpath, board_t *board, int accumulated_points)
{
    char *content = read_file_to_string(lvlpath);
    if (!content)
        return -1;

    debug("parse_lvl_to_board: parsing '%s'\n", lvlpath);

    /* Initialize defaults */
    int rows = 0, cols = 0;
    int tempo = 0;
    char pacfile[MAX_FILENAME] = "";
    char monfiles[MAX_GHOSTS][MAX_FILENAME];
    int mon_count = 0;
    for (int i = 0; i < MAX_GHOSTS; i++)
        monfiles[i][0] = '\0';

    char *saveptr;
    char *line = strtok_r(content, "\n", &saveptr);
    /* We'll collect matrix lines after headers */
    char **matrix_lines = NULL;
    int matrix_lines_count = 0;

    while (line)
    {
        char *t = trim(line);
        if (*t == '\0' || *t == '#')
        {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (strncmp(t, "DIM", 3) == 0)
        {
            const char *q = t + 3;
            while (*q && isspace((unsigned char)*q))
                q++;
            char *endptr = NULL;
            long r = strtol(q, &endptr, 10);
            if (endptr != q)
            {
                q = endptr;
                while (*q && isspace((unsigned char)*q))
                    q++;
                long c = strtol(q, &endptr, 10);
                if (endptr != q)
                {
                    rows = (int)r;
                    cols = (int)c;
                }
            }
        }
        else if (strncmp(t, "TEMPO", 5) == 0)
        {
            const char *q = t + 5;
            while (*q && isspace((unsigned char)*q))
                q++;
            char *endptr = NULL;
            long v = strtol(q, &endptr, 10);
            if (endptr != q)
            {
                /* Validate tempo is reasonable */
                if (v >= 0 && v <= MAX_TEMPO_MS)
                    tempo = (int)v;
                else
                    debug("parse_lvl_to_board: ignoring invalid TEMPO value: %d\n", (int)v);
            }
        }
        else if (strncmp(t, "PAC", 3) == 0)
        {
            const char *q = t + 3;
            while (*q && isspace((unsigned char)*q))
                q++;
            /* copy token */
            int i = 0;
            while (*q && !isspace((unsigned char)*q) && i < MAX_FILENAME - 1)
            {
                pacfile[i++] = *q++;
            }
            pacfile[i] = '\0';
        }
        else if (strncmp(t, "MON", 3) == 0)
        {
            /* parse multiple filenames */
            char *p = t + 3;
            char *tok = strtok_r(p, " \t", &p);
            while (tok)
            {
                if (mon_count >= MAX_GHOSTS)
                {
                    debug("parse_lvl_to_board: too many ghosts (>%d) in level '%s'\n", MAX_GHOSTS, lvlpath);
                    /* hard error: free and abort parsing */
                    for (int i = 0; i < matrix_lines_count; i++)
                        free(matrix_lines[i]);
                    free(matrix_lines);
                    free(content);
                    return -1;
                }
                strncpy(monfiles[mon_count++], tok, MAX_FILENAME - 1);
                tok = strtok_r(NULL, " \t", &p);
            }
        }
        else
        {
            /* matrix line */
            char *copy = strdup(t);
            if (!copy)
            {
                /* Out of memory: cleanup and abort */
                for (int i = 0; i < matrix_lines_count; i++)
                    free(matrix_lines[i]);
                free(matrix_lines);
                free(content);
                return -1;
            }
            char **new_matrix = realloc(matrix_lines, sizeof(char *) * (matrix_lines_count + 1));
            if (!new_matrix)
            {
                /* realloc failed: cleanup including the new copy */
                free(copy);
                for (int i = 0; i < matrix_lines_count; i++)
                    free(matrix_lines[i]);
                free(matrix_lines);
                free(content);
                return -1;
            }
            matrix_lines = new_matrix;
            matrix_lines[matrix_lines_count++] = copy;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (rows == 0 || cols == 0 || matrix_lines_count == 0)
    {
        debug("parse_lvl_to_board: invalid level - rows=%d cols=%d matrix_lines=%d\n", rows, cols, matrix_lines_count);
        /* invalid level */
        for (int i = 0; i < matrix_lines_count; i++)
            free(matrix_lines[i]);
        free(matrix_lines);
        free(content);
        return -1;
    }

    /* Allocate board */
    board->height = rows;
    board->width = cols;
    board->tempo = tempo;
    board->n_pacmans = 1;
    board->n_ghosts = mon_count;

    /* Validate dimensions to prevent integer overflow */
    if (rows < 0 || cols < 0 || rows > MAX_BOARD_DIMENSION || cols > MAX_BOARD_DIMENSION)
    {
        debug("parse_lvl_to_board: invalid dimensions - rows=%d cols=%d\n", rows, cols);
        for (int i = 0; i < matrix_lines_count; i++)
            free(matrix_lines[i]);
        free(matrix_lines);
        free(content);
        return -1;
    }

    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    if (!board->board)
    {
        debug("parse_lvl_to_board: calloc failed for board\n");
        for (int i = 0; i < matrix_lines_count; i++)
            free(matrix_lines[i]);
        free(matrix_lines);
        free(content);
        return -1;
    }

    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    if (!board->pacmans)
    {
        debug("parse_lvl_to_board: calloc failed for pacmans\n");
        free(board->board);
        for (int i = 0; i < matrix_lines_count; i++)
            free(matrix_lines[i]);
        free(matrix_lines);
        free(content);
        return -1;
    }

    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));
    if (!board->ghosts)
    {
        debug("parse_lvl_to_board: calloc failed for ghosts\n");
        free(board->board);
        free(board->pacmans);
        for (int i = 0; i < matrix_lines_count; i++)
            free(matrix_lines[i]);
        free(matrix_lines);
        free(content);
        return -1;
    }

    /* initialize board positions */
    for (int y = 0; y < rows; y++)
    {
        const char *row = matrix_lines[y];
        for (int x = 0; x < cols; x++)
        {
            board_pos_t *pos = &board->board[y * cols + x];
            pos->content = ' ';
            pos->has_dot = 0;
            pos->has_portal = 0;
            char ch = (x < (int)strlen(row)) ? row[x] : 'o';
            if (ch == 'X')
            {
                pos->content = 'W';
            }
            else if (ch == 'o')
            {
                pos->content = ' ';
                pos->has_dot = 1;
            }
            else if (ch == '@')
            {
                pos->content = ' ';
                pos->has_portal = 1;
            }
            else
            {
                /* default to empty */
                pos->content = ' ';
            }
        }
    }

    /* set level name to basename of lvlpath */
    const char *p = strrchr(lvlpath, '/');
    if (!p)
        p = lvlpath;
    else
        p++;
    strncpy(board->level_name, p, sizeof(board->level_name) - 1);

    /* set pacman default */
    board->pacmans[0].n_moves = 0;
    board->pacmans[0].current_move = 0;
    board->pacmans[0].alive = 1;
    board->pacmans[0].waiting = 0;
    board->pacmans[0].passo = 0;
    board->pacmans[0].points = accumulated_points;
    /* default pacman position (1,1) if not provided by .p file */
    board->pacmans[0].pos_x = 1;
    board->pacmans[0].pos_y = 1;

    /* parse pacman file if present */
    if (pacfile[0] != '\0')
    {
        parse_behavior(loader_dir, pacfile, 1, &board->pacmans[0]);
        strncpy(board->pacman_file, pacfile, sizeof(board->pacman_file) - 1);
        /* place pacman on board */
        int px = board->pacmans[0].pos_x;
        int py = board->pacmans[0].pos_y;
        if (px >= 0 && px < board->width && py >= 0 && py < board->height)
        {
            board->board[py * board->width + px].content = 'P';
        }
    }
    else
    {
        strncpy(board->pacman_file, "", sizeof(board->pacman_file));
        /* place default pacman */
        board->board[1 * board->width + 1].content = 'P';
    }

    /* parse monsters */
    for (int i = 0; i < mon_count; i++)
    {
        strncpy(board->ghosts_files[i], monfiles[i], sizeof(board->ghosts_files[i]) - 1);
        parse_behavior(loader_dir, monfiles[i], 0, &board->ghosts[i]);
        /* place ghost on board */
        int gx = board->ghosts[i].pos_x;
        int gy = board->ghosts[i].pos_y;
        if (gx >= 0 && gx < board->width && gy >= 0 && gy < board->height)
        {
            board->board[gy * board->width + gx].content = 'M';
        }
    }

    /* free matrix lines and content */
    for (int i = 0; i < matrix_lines_count; i++)
        free(matrix_lines[i]);
    free(matrix_lines);
    free(content);
    return 0;
}

int load_next_level(board_t *board, int accumulated_points)
{
    if (!initialized)
        return -1;
    if (current_level >= level_count)
        return 1; /* no more levels */

    const char *lvlpath = level_paths[current_level++];
    int r = parse_lvl_to_board(lvlpath, board, accumulated_points);
    return r == 0 ? 0 : -1;
}

int get_current_level(void)
{
    return current_level;
}

void set_current_level(int level)
{
    current_level = level;
}
