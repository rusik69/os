/* invaders.c — Space Invaders */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static void delay(void) {
    for (volatile int d = 0; d < 2000000; d++);
}

int main(void) {
    int px = 38, ei = 0, w = 80, h = 24;
    int dir = 1, step = 0, score = 0;
    int ex[55], ey[55], ed[55];
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 11; c++) {
            ex[ei] = 10 + c * 5; ey[ei] = 3 + r * 2; ed[ei] = 1; ei++;
        }
    int alive = ei, bx = 0, by = 0, bactive = 0;
    char buf[16];
    printf("\033[2JINVADERS  a/d=move  space=fire  q=quit\n");
    while (alive > 0) {
        printf("\033[H");
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                char ch = ' ';
                if (r == 22 && c == px) ch = 'A';
                if (bactive && c == bx && r == by) ch = '!';
                for (int i = 0; i < ei; i++)
                    if (ed[i] && c == ex[i] && r == ey[i]) ch = 'V';
                putchar(ch);
            }
            putchar('\n');
        }
        printf("Score: %d  Aliens: %d\n", score, alive);
        int n = read(0, buf, 15);
        if (n > 0) {
            buf[n] = 0;
            if (buf[0] == 'a' && px > 2) px--;
            if (buf[0] == 'd' && px < 77) px++;
            if (buf[0] == ' ' && !bactive) { bx = px; by = 21; bactive = 1; }
            if (buf[0] == 'q') break;
        }
        if (step % 2 == 0) {
            int edge = 0;
            for (int i = 0; i < ei; i++)
                if (ed[i]) { ex[i] += dir; if (ex[i] <= 2 || ex[i] >= 78) edge = 1; }
            if (edge) { dir = -dir; for (int i = 0; i < ei; i++) if (ed[i]) ey[i]++; }
        }
        if (bactive) {
            for (int i = 0; i < ei; i++)
                if (ed[i] && bx >= ex[i] - 2 && bx <= ex[i] + 2 && by >= ey[i] - 1 && by <= ey[i] + 1) {
                    ed[i] = 0; alive--; score += 10; bactive = 0; break;
                }
            by--;
            if (by < 0) bactive = 0;
        }
        step++;
        for (int i = 0; i < ei; i++)
            if (ed[i] && ey[i] >= 22) { printf("GAME OVER! Score: %d\n", score); return 0; }
        delay();
    }
    printf("VICTORY! Score: %d\n", score);
    return 0;
}
