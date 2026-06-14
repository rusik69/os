/* minesweeper.c — Text Minesweeper: 10x10 grid */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define SIZE 10
#define MINES 10

static char visible[SIZE][SIZE];
static char field[SIZE][SIZE];
static int revealed;

static void init_field(void) {
    revealed = 0;
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++) {
            visible[r][c] = '?';
            field[r][c] = '0';
        }
    unsigned int seed = 42;
    srand(seed);
    int placed = 0;
    while (placed < MINES) {
        int r = rand() % SIZE;
        int c = rand() % SIZE;
        if (field[r][c] != 'M') {
            field[r][c] = 'M';
            placed++;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < SIZE && nc >= 0 && nc < SIZE && field[nr][nc] != 'M')
                        field[nr][nc]++;
                }
        }
    }
}

static void print_board(void) {
    printf("\033[H\033[J  ");
    for (int c = 0; c < SIZE; c++) printf("%d ", c);
    printf("\n");
    for (int r = 0; r < SIZE; r++) {
        printf("%d ", r);
        for (int c = 0; c < SIZE; c++) {
            char ch = visible[r][c];
            if (ch == '?') ch = (field[r][c] == 'M') ? '.' : '?';
            printf("%c ", ch);
        }
        printf("\n");
    }
}

static int reveal(int r, int c) {
    if (r < 0 || r >= SIZE || c < 0 || c >= SIZE) return 0;
    if (visible[r][c] != '?') return 0;
    visible[r][c] = field[r][c];
    revealed++;
    if (field[r][c] == 'M') return -1;
    if (field[r][c] == '0') {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                reveal(r + dr, c + dc);
    }
    return 0;
}

int main(void) {
    char buf[32];
    init_field();
    while (1) {
        print_board();
        printf("Enter 'r row col' or 'f row col': ");
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        /* Manual parse: format is "X Y Z" or "X Y" */
        char *p = buf;
        while (*p == ' ') p++;
        char cmd = *p++;
        while (*p == ' ') p++;
        int r = 0;
        while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; }
        while (*p == ' ') p++;
        int c = 0;
        while (*p >= '0' && *p <= '9') { c = c * 10 + (*p - '0'); p++; }
        if (r < 0 || r >= SIZE || c < 0 || c >= SIZE) continue;
        if (cmd == 'f') {
            visible[r][c] = (visible[r][c] == 'F') ? '?' : 'F';
        } else if (cmd == 'r') {
            if (reveal(r, c) < 0) {
                print_board();
                printf("BOOM! You hit a mine.\n");
                return 0;
            }
            if (revealed >= SIZE * SIZE - MINES) {
                print_board();
                printf("You win!\n");
                return 0;
            }
        }
    }
    return 0;
}
