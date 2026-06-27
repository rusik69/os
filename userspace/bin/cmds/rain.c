/* rain.c — matrix rain effect */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void delay(void) {
    for (volatile int d = 0; d < 800000; d++);
}

int main(void) {
    int w = 80, drop[80], speed[80];
    for (int i = 0; i < w; i++) { drop[i] = rand() % 24; speed[i] = 1 + (rand() % 3); }
    printf("\033[2JMatrix Rain\n");
    for (int f = 0; f < 200; f++) {
        printf("\033[H");
        for (int c = 0; c < w; c++) {
            for (int r = 0; r < 24; r++) {
                int dist = r - drop[c];
                putchar((dist >= 0 && dist < 3) ? '0' + (rand() % 10) : ' ');
            }
        }
        printf("\n");
        for (int c = 0; c < w; c++) drop[c] = (drop[c] + speed[c]) % 29;
        delay();
    }
    return 0;
}
