/* maze.c — Maze game */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    int w = 20, h = 15;
    char m[15][20];
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            m[r][c] = (r == 0 || r == h - 1 || c == 0 || c == w - 1) ? '#' : ' ';
    m[1][1] = ' ';
    int x = 1, y = 1;
    char buf[16];
    printf("\033[2JMAZE  #=wall  wasd=move  q=quit\n");
    while (1) {
        printf("\033[H");
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                if (r == y && c == x) putchar('@');
                else if (r == h - 2 && c == w - 2) putchar('X');
                else putchar(m[r][c]);
            }
            putchar('\n');
        }
        if (x == w - 2 && y == h - 2) { printf("ESCAPED!\n"); break; }
        int n = read(0, buf, 15);
        if (n > 0) {
            buf[n] = 0;
            int nx = x, ny = y;
            if (buf[0] == 'w') ny--;
            if (buf[0] == 's') ny++;
            if (buf[0] == 'a') nx--;
            if (buf[0] == 'd') nx++;
            if (buf[0] == 'q') break;
            if (nx >= 0 && nx < w && ny >= 0 && ny < h && m[ny][nx] == ' ') { x = nx; y = ny; }
        }
    }
    return 0;
}
