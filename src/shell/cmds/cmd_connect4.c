/* cmd_connect4.c — Connect Four terminal game */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "keyboard.h"

#define C4_ROWS  6
#define C4_COLS  7
#define C4_EMPTY 0
#define C4_P1    1  /* red */
#define C4_P2    2  /* yellow */

static uint8_t c4_board[C4_ROWS][C4_COLS];
static int c4_turn;        /* C4_P1 or C4_P2 */
static int c4_game_over;
static int c4_winner;
static int c4_col;         /* current column selector */

static void c4_init(void) {
    memset(c4_board, 0, sizeof(c4_board));
    c4_turn = C4_P1;
    c4_game_over = 0;
    c4_winner = 0;
    c4_col = C4_COLS / 2;
}

/* Drop a piece in column col (0-indexed). Returns row or -1 if full. */
static int c4_drop(int col) {
    for (int r = C4_ROWS - 1; r >= 0; r--) {
        if (c4_board[r][col] == C4_EMPTY) {
            c4_board[r][col] = (uint8_t)c4_turn;
            return r;
        }
    }
    return -1;
}

/* Check if the last drop at (row, col) won. */
static int c4_check_win(int row, int col) {
    uint8_t p = c4_board[row][col];
    if (p == C4_EMPTY) return 0;

    /* Directions: horizontal, vertical, diagonal \, diagonal / */
    int dr[] = { 0, 1, 1, -1 };
    int dc[] = { 1, 0, 1, 1 };

    for (int d = 0; d < 4; d++) {
        int count = 1;
        /* Positive direction */
        for (int i = 1; i < 4; i++) {
            int r = row + dr[d] * i;
            int c = col + dc[d] * i;
            if (r < 0 || r >= C4_ROWS || c < 0 || c >= C4_COLS) break;
            if (c4_board[r][c] != p) break;
            count++;
        }
        /* Negative direction */
        for (int i = 1; i < 4; i++) {
            int r = row - dr[d] * i;
            int c = col - dc[d] * i;
            if (r < 0 || r >= C4_ROWS || c < 0 || c >= C4_COLS) break;
            if (c4_board[r][c] != p) break;
            count++;
        }
        if (count >= 4) return 1;
    }
    return 0;
}

/* Check if board is full (draw) */
static int c4_check_draw(void) {
    for (int c = 0; c < C4_COLS; c++)
        if (c4_board[0][c] == C4_EMPTY) return 0;
    return 1;
}

static void c4_draw(void) {
    kprintf("\033[H\033[J");  /* home + clear */

    kprintf("\033[1;34m  Connect Four\033[0m\n\n");

    /* Selector arrow */
    kprintf("   ");
    for (int c = 0; c < C4_COLS; c++) {
        if (!c4_game_over && c == c4_col)
            kprintf(" \033[1;31;5mv\033[0m");  /* blinking arrow */
        else
            kprintf("  ");
    }
    kprintf("\n");

    /* Board */
    for (int r = 0; r < C4_ROWS; r++) {
        kprintf("  \033[1;34m|\033[0m");
        for (int c = 0; c < C4_COLS; c++) {
            uint8_t p = c4_board[r][c];
            if (p == C4_P1)
                kprintf("\033[31;1m@\033[0m\033[1;34m|\033[0m");
            else if (p == C4_P2)
                kprintf("\033[33;1m@\033[0m\033[1;34m|\033[0m");
            else
                kprintf(" \033[1;34m|\033[0m");
        }
        kprintf("\n");
    }

    /* Bottom border */
    kprintf("  \033[1;34m+");
    for (int c = 0; c < C4_COLS; c++) kprintf("-+");
    kprintf("\033[0m\n");

    /* Status */
    if (c4_winner == C4_P1)
        kprintf("\n\033[31;1m*** Red wins! ***\033[0m\n");
    else if (c4_winner == C4_P2)
        kprintf("\n\033[33;1m*** Yellow wins! ***\033[0m\n");
    else if (c4_check_draw() && !c4_game_over) {
        c4_game_over = 1;
        kprintf("\n\033[1;37m*** Draw! ***\033[0m\n");
    } else if (!c4_game_over) {
        kprintf("\n\033[1;37m%s's turn\033[0m\n",
                c4_turn == C4_P1 ? "\033[31mRed\033[0m" : "\033[33mYellow\033[0m");
        kprintf("\033[33m<- -> move   Enter: drop   Q: quit\033[0m\n");
    } else {
        kprintf("\nPress any key...\n");
    }
}

void cmd_connect4(const char *args) {
    (void)args;
    c4_init();

    while (!c4_game_over) {
        c4_draw();
        if (c4_winner) break;
        if (c4_check_draw()) break;

        char ch = keyboard_getchar();

        if (ch == 'q' || ch == 'Q') {
            kprintf("Quit.\n");
            return;
        }

        if (ch == KEY_LEFT && c4_col > 0) c4_col--;
        if (ch == KEY_RIGHT && c4_col < C4_COLS - 1) c4_col++;

        if (ch == '\n' || ch == '\r') {
            int row = c4_drop(c4_col);
            if (row < 0) continue; /* column full */

            if (c4_check_win(row, c4_col)) {
                c4_winner = c4_turn;
                c4_game_over = 1;
                break;
            }

            if (c4_check_draw()) {
                c4_game_over = 1;
                break;
            }

            /* Switch turns */
            c4_turn = (c4_turn == C4_P1) ? C4_P2 : C4_P1;
        }
    }

    c4_draw();
    if (c4_game_over) {
        kprintf("Press any key...\n");
        keyboard_getchar();
    }
}
