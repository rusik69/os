/* pgrep.c — look up processes by name */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: pgrep PATTERN\n");
        return 1;
    }
    int found = 0;
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) {
        printf("pgrep: cannot open /proc\n");
        return 1;
    }
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
                    /* Check if pattern matches process name */
                    for (int i = 1; i < argc; i++) {
                        if (strstr(name, argv[i]) != NULL) {
                            printf("%s\n", d->d_name);
                            found = 1;
                            break;
                        }
                    }
                }
            }
        }
        pos += d->d_reclen;
    }
    return found ? 0 : 1;
}
