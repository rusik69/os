/* bounce.c — bouncing ball animation */
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static void delay(void) {
    for (volatile int d = 0; d < 3000000; d++);
}

int main(void) {
    int w = 60, h = 20, x = 30, y = 5, dx = 1, dy = 1;
    printf("\033[2JBouncing Ball\n");
    for (int i = 0; i < 200; i++) {
        printf("\033[H");
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++)
                putchar((r == y && c == x) ? 'O' : ' ');
            putchar('\n');
        }
        x += dx; y += dy;
        if (x <= 0 || x >= w - 1) dx = -dx;
        if (y <= 0 || y >= h - 1) dy = -dy;
        delay();
    }
    return 0;
}
