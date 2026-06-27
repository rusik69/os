/* breakout.c — Breakout game */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static void delay(void) {
    for (volatile int d = 0; d < 2000000; d++);
}

int main(void) {
    int px = 38, bx = 40, by = 18, bdx = 1, bdy = -1, w = 80, h = 24;
    char buf[16];
    int blocks[4][20];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 20; c++)
            blocks[r][c] = 1;
    int remain = 80, score = 0;
    printf("\033[2JBREAKOUT  a/d=move  q=quit\n");
    while (remain > 0) {
        printf("\033[H");
        char grid[24][80];
        memset(grid, 0, sizeof(grid));
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 20; c++)
                if (blocks[r][c]) grid[r][c + 10] = '#';
        grid[by][bx] = 'O';
        for (int i = -3; i <= 3; i++) grid[22][px + i] = '=';
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++)
                putchar(grid[r][c] ? grid[r][c] : ' ');
            putchar('\n');
        }
        printf("Score: %d  Remain: %d\n", score, remain);
        int n = read(0, buf, 15);
        if (n > 0) {
            buf[n] = 0;
            if (buf[0] == 'a' && px > 3) px--;
            if (buf[0] == 'd' && px < 76) px++;
            if (buf[0] == 'q') break;
        }
        bx += bdx;
        by += bdy;
        if (by < 4 && bx >= 10 && bx < 90) {
            int c = (bx - 10) / 4;
            if (c >= 0 && c < 20) {
                int r = by;
                if (r >= 0 && r < 4 && blocks[r][c]) {
                    blocks[r][c] = 0;
                    remain--;
                    score += 10;
                    bdy = -bdy;
                }
            }
        }
        if (by <= 0) bdy = -bdy;
        if (bx <= 0 || bx >= w - 1) bdx = -bdx;
        if (by >= 22) {
            if (bx >= px - 3 && bx <= px + 3) {
                bdy = -bdy;
            } else {
                printf("GAME OVER! Score: %d\n", score);
                break;
            }
        }
        delay();
    }
    if (remain == 0) printf("YOU WIN! Score: %d\n", score);
    return 0;
}
