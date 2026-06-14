/* pkill.c — kill processes by name */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int sig = 15; /* SIGTERM */
    int first_arg = 1;
    if (argc >= 3 && strcmp(argv[1], "-") == 0 && argv[1][1] != '\0') {
        sig = atoi(argv[1] + 1);
        first_arg = 2;
    }
    if (argc < first_arg + 1) {
        printf("Usage: pkill [-SIGNAL] PATTERN\n");
        return 1;
    }
    int found = 0;
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) return 1;
    char buf[4096];
    int n = getdents64(fd, buf, 4096);
    close(fd);
    if (n <= 0) return 1;
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
                if (r > 0) {
                    cmdline[r] = '\0';
                    char *name = cmdline;
                    char *slash = strrchr(name, '/');
                    if (slash) name = slash + 1;
                    for (int i = first_arg; i < argc; i++) {
                        if (strstr(name, argv[i]) != NULL) {
                            int pid = 0;
                            char *np = d->d_name;
                            while (*np) { pid = pid * 10 + (*np - '0'); np++; }
                            kill(pid, sig);
                            found = 1;
                        }
                    }
                }
            }
        }
        pos += d->d_reclen;
    }
    return found ? 0 : 1;
}
