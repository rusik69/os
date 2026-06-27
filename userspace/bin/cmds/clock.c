/* clock.c — live clock display */
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static void delay(void) {
    for (volatile int d = 0; d < 10000000; d++);
}

int main(void) {
    printf("\033[2JLive Tick Display\n");
    for (int i = 0; i < 60; i++) {
        printf("\033[H");
        printf("Tick %d/60\n", i + 1);
        delay();
    }
    printf("Done.\n");
    return 0;
}
