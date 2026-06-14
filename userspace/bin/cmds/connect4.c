/* connect4.c — Text Connect Four: 6x7 grid, two-player */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define ROWS 6
#define COLS 7

static char board[ROWS][COLS];

static void init_board(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            board[r][c] = ' ';
}

static void print_board(void) {
    printf("\033[H\033[J");
    printf(" 1 2 3 4 5 6 7\n");
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++)
            printf("|%c", board[r][c]);
        printf("|\n");
    }
    printf("---------------\n");
}

static int drop_piece(int col, char piece) {
    if (col < 0 || col >= COLS) return -1;
    for (int r = ROWS - 1; r >= 0; r--) {
        if (board[r][col] == ' ') {
            board[r][col] = piece;
            return r;
        }
    }
    return -1;
}

static int check_win(int row, int col, char piece) {
    int dr[] = {0, 1, 1, 1};
    int dc[] = {1, 0, 1, -1};
    for (int d = 0; d < 4; d++) {
        int count = 1;
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int steps = 1; steps < 4; steps++) {
                int r = row + dr[d] * steps * dir;
                int c = col + dc[d] * steps * dir;
                if (r < 0 || r >= ROWS || c < 0 || c >= COLS) break;
                if (board[r][c] != piece) break;
                count++;
            }
        }
        if (count >= 4) return 1;
    }
    return 0;
}

int main(void) {
    char buf[16];
    int col;
    char players[] = {'X', 'O'};
    int turn = 0;
    int moves = 0;

    init_board();
    while (1) {
        print_board();
        printf("Player %c's turn (1-7): ", players[turn]);
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        col = atoi(buf) - 1;
        int row = drop_piece(col, players[turn]);
        if (row < 0) {
            printf("Invalid move!\n");
            continue;
        }
        moves++;
        if (check_win(row, col, players[turn])) {
            print_board();
            printf("Player %c wins!\n", players[turn]);
            return 0;
        }
        if (moves >= ROWS * COLS) {
            print_board();
            printf("Draw!\n");
            return 0;
        }
        turn = 1 - turn;
    }
    return 0;
}
