/* pong.c — Pong game */
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    int px = 20, py = 10, bx = 40, by = 10, bdx = -1, bdy = 1, score = 0, w = 80, h = 24;
    char buf[16];
    printf("\033[2JPONG!  w/s = move  q = quit\n");
    while (1) {
        printf("\033[H");
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                char ch = ' ';
                if (r == by && c == bx) ch = 'O';
                if (c == px && (r == py || r == py - 1 || r == py + 1 || r == py - 2)) ch = '|';
                putchar(ch);
            }
            putchar('\n');
        }
        printf("Score: %d\n", score);
        int n = read(0, buf, 15);
        if (n > 0) {
            buf[n] = 0;
            if (buf[0] == 'w' && py > 2) py--;
            if (buf[0] == 's' && py < h - 3) py++;
            if (buf[0] == 'q') break;
        }
        bx += bdx;
        by += bdy;
        if (by <= 0 || by >= h - 1) bdy = -bdy;
        if (bx == px + 1 && by >= py - 2 && by <= py + 2) { bdx = -bdx; score++; }
        if (bx >= w) { printf("GAME OVER  Score: %d\n", score); break; }
        if (bx < 0) break;
    }
    return 0;
}
