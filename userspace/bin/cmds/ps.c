/* ps.c — list processes by reading /proc directory */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) {
        printf("ps: cannot open /proc\n");
        return 1;
    }
    char buf[4096];
    int n = getdents64(fd, buf, 4096);
    close(fd);
    if (n <= 0) {
        printf("ps: no entries\n");
        return 1;
    }

    printf("PID    NAME\n");
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        int is_num = 1;
        char *p = d->d_name;
        while (*p) { if (*p < '0' || *p > '9') { is_num = 0; break; } p++; }
        if (is_num) {
            char procpath[64];
            int plen = 0;
            char *sp = "/proc/";
            while (*sp) procpath[plen++] = *sp++;
            p = d->d_name;
            while (*p) procpath[plen++] = *p++;
            char *cmds = "/cmdline";
            while (*cmds) procpath[plen++] = *cmds++;
            procpath[plen] = '\0';

            int pfd = open(procpath, O_RDONLY, 0);
            if (pfd >= 0) {
                char cmdline[256];
                int r = read(pfd, cmdline, 255);
                close(pfd);
                cmdline[r] = '\0';
                for (int i = 0; i < r; i++) if (cmdline[i] == '\0') cmdline[i] = ' ';
                printf("%-5s %s\n", d->d_name, cmdline);
            } else {
                printf("%-5s [kernel thread]\n", d->d_name);
            }
        }
        pos += d->d_reclen;
    }
    return 0;
}
