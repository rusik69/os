/* sleep.c — sleep for a given number of seconds */

#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: sleep <seconds>\n");
        return 1;
    }
    int secs = atoi(argv[1]);
    if (secs < 0) secs = 0;
    struct timespec ts;
    ts.tv_sec = secs;
    ts.tv_nsec = 0;
    if (nanosleep(&ts, 0) < 0)
        return 1;
    return 0;
}
