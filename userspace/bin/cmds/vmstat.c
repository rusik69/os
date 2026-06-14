#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    /* Display virtual memory statistics */
    int delay = 1;
    int count = 1;
    if (argc > 1) {
        count = atoi(argv[1]);
        if (count <= 0) count = 1;
    }
    if (argc > 2) {
        delay = atoi(argv[2]);
        if (delay <= 0) delay = 1;
    }
    printf("procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----\n");
    printf(" r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st\n");
    for (int i = 0; i < count; i++) {
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            printf(" 0  0 %5llu %6llu %6llu %6llu   0    0     0     0    0    0  0  0 100  0  0\n",
                (info.totalswap - info.freeswap) / 1024,
                info.freeram / 1024,
                info.bufferram / 1024,
                (info.totalram - info.freeram - info.bufferram - info.sharedram) / 1024);
        } else {
            printf("  0  0      0      0      0      0    0    0     0     0    0    0  0  0 100  0  0\n");
        }
        if (i + 1 < count) {
            /* sleep */
            struct timespec ts;
            ts.tv_sec = delay;
            ts.tv_nsec = 0;
            nanosleep(&ts, 0);
        }
    }
    return 0;
}
