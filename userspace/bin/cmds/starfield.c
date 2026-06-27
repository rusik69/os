/* starfield.c — starfield simulation */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void delay(void) {
    for (volatile int d = 0; d < 500000; d++);
}

int main(void) {
    int n = 100, sx[100], sy[100], sz[100];
    for (int i = 0; i < n; i++) { sx[i] = rand() % 200 - 100; sy[i] = rand() % 200 - 100; sz[i] = rand() % 100 + 1; }
    printf("\033[2JStarfield\n");
    for (int f = 0; f < 200; f++) {
        printf("\033[H");
        for (int i = 0; i < n; i++) {
            int px = 40 + sx[i] * 50 / sz[i], py = 12 + sy[i] * 12 / sz[i];
            if (px >= 0 && px < 80 && py >= 0 && py < 24) {
                printf("\033[%d;%df", py + 1, px + 1);
                putchar(sz[i] < 20 ? '*' : (sz[i] < 50 ? '.' : ' '));
            }
            sz[i] -= 2;
            if (sz[i] <= 0) { sx[i] = rand() % 200 - 100; sy[i] = rand() % 200 - 100; sz[i] = 100; }
        }
        delay();
    }
    printf("\033[24;1fDone.\n");
    return 0;
}
