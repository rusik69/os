/* watch.c — execute a command periodically */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int interval = 2;
    int optind = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        interval = 0;
        const char *s = argv[2];
        while (*s >= '0' && *s <= '9') { interval = interval * 10 + (*s - '0'); s++; }
        if (interval < 1) interval = 1;
        optind = 3;
    }
    if (optind >= argc) { printf("Usage: watch [-n seconds] <command> [args...]\n"); return 1; }
    const char *cmd = argv[optind];
    char *exec_args[256];
    int nargs = 0;
    for (int i = optind; i < argc && nargs < 255; i++)
        exec_args[nargs++] = argv[i];
    exec_args[nargs] = 0;
    for (;;) {
        printf("Every %ds: ", interval);
        for (int i = 0; exec_args[i]; i++) {
            const char *p = exec_args[i];
            while (*p) { write(STDOUT_FILENO, p, 1); p++; }
            if (exec_args[i+1]) write(STDOUT_FILENO, " ", 1);
        }
        write(STDOUT_FILENO, "\n\n", 2);
        int ret = posix_spawn(cmd, exec_args, 0);
        if (ret < 0) {
            printf("watch: exec failed\n");
            return 1;
        }
        struct timespec ts;
        ts.tv_sec = interval;
        ts.tv_nsec = 0;
        nanosleep(&ts, 0);
    }
    return 0;
}
