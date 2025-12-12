#include "display.h"
#include "board.h"
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

int terminal_init()
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    if (has_colors())
    {
        start_color();
        init_pair(1, COLOR_YELLOW, COLOR_BLACK);
        init_pair(2, COLOR_RED, COLOR_BLACK);
        init_pair(3, COLOR_BLUE, COLOR_BLACK);
        init_pair(4, COLOR_WHITE, COLOR_BLACK);
        init_pair(5, COLOR_GREEN, COLOR_BLACK);
        init_pair(6, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(7, COLOR_CYAN, COLOR_BLACK);
    }

    clear();

    return 0;
}

void draw_board(board_t *board, int mode)
{
    clear();

    attron(COLOR_PAIR(5));
    mvprintw(0, 0, "=== PACMAN GAME ===");
    switch (mode)
    {
    case DRAW_GAME_OVER:
        mvprintw(1, 0, " GAME OVER ");
        break;

    case DRAW_WIN:
        mvprintw(1, 0, " VICTORY ");
        break;

    case DRAW_MENU:
        mvprintw(1, 0, "Level: %s | Use W/A/S/D to move | Q to quit | G to quicksave ", board->level_name);
        break;
    }

    int start_row = 3;

    for (int y = 0; y < board->height; y++)
    {
        for (int x = 0; x < board->width; x++)
        {
            int index = y * board->width + x;
            char ch = board->board[index].content;
            int ghost_charged = 0;

            for (int g = 0; g < board->n_ghosts; g++)
            {
                ghost_t *ghost = &board->ghosts[g];
                if (ghost->pos_x == x && ghost->pos_y == y)
                {
                    if (ghost->charged)
                        ghost_charged = 1;
                    break;
                }
            }

            move(start_row + y, x);

            switch (ch)
            {
            case 'W':
                attron(COLOR_PAIR(3));
                addch('#');
                attroff(COLOR_PAIR(3));
                break;

            case 'P':
                attron(COLOR_PAIR(1) | A_BOLD);
                addch('C');
                attroff(COLOR_PAIR(1) | A_BOLD);
                break;

            case 'M':
                attron((COLOR_PAIR(2) | A_BOLD) | ((ghost_charged) ? (A_DIM) : (0)));
                addch('M');
                attroff((COLOR_PAIR(2) | A_BOLD) | ((ghost_charged) ? (A_DIM) : (0)));
                break;

            case ' ':
                if (board->board[index].has_portal)
                {
                    attron(COLOR_PAIR(6));
                    addch('@');
                    attroff(COLOR_PAIR(6));
                }
                else if (board->board[index].has_dot)
                {
                    attron(COLOR_PAIR(4));
                    addch('.');
                    attroff(COLOR_PAIR(4));
                }
                else
                    addch(' ');
                break;

            default:
                addch(ch);
                break;
            }
        }
    }

    attron(COLOR_PAIR(5));
    mvprintw(start_row + board->height + 1, 0, "Points: %d",
             board->pacmans[0].points);
    attroff(COLOR_PAIR(5));
}

void draw(char c, int colour_i, int pos_x, int pos_y)
{
    move(pos_y, pos_x);
    attron(COLOR_PAIR(colour_i) | A_BOLD);
    addch(c);
    attroff(COLOR_PAIR(colour_i) | A_BOLD);
}

void refresh_screen()
{
    refresh();
}

char get_input()
{
    int ch = getch();

    if (ch == ERR)
    {
        int err = errno;
        debug("get_input: raw ch=%d errno=%d\n", ch, err);
        return '\0';
    }

    debug("get_input: raw ch=%d\n", ch);

    if (ch == KEY_UP)
        return 'W';
    if (ch == KEY_DOWN)
        return 'S';
    if (ch == KEY_LEFT)
        return 'A';
    if (ch == KEY_RIGHT)
        return 'D';

    ch = toupper((char)ch);

    switch ((char)ch)
    {
    case 'W':
    case 'S':
    case 'A':
    case 'D':
    case 'Q':
    case 'G':
    case 'L':
        debug("get_input: mapped to '%c'\n", (char)ch);
        return (char)ch;

    default:
        return '\0';
    }
}

void terminal_cleanup()
{
    endwin();
}
