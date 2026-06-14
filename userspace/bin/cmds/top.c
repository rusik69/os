/* top.c — Process monitor: read /proc, print process table */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_PROC 256

int main(void) {
    char buf[4096];
    while (1) {
        printf("\033[H\033[J");
        printf("PID  PPID NAME\n");
        printf("---  ---- ----\n");
        /* Try reading /proc/stat or /proc directly */
        int fd = open("/proc", O_RDONLY, 0);
        if (fd >= 0) {
            int n = getdents64(fd, buf, sizeof(buf));
            if (n > 0) {
                struct dirent *d;
                int off = 0;
                while (off < n) {
                    d = (struct dirent *)(buf + off);
                    off += d->d_reclen;
                    int pid = atoi(d->d_name);
                    if (pid > 0) {
                        printf("%d    -    %s\n", pid, d->d_name);
                    }
                }
            }
            close(fd);
        }
        printf("\nPress q to quit, any other key to refresh...");
        char ch;
        read(0, &ch, 1);
        if (ch == 'q') break;
    }
    return 0;
}
