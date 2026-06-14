/* pidof.c — find PID of running program */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: pidof PROGRAM\n");
        return 1;
    }
    int found = 0;
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) {
        printf("pidof: cannot open /proc\n");
        return 1;
    }
    char buf[4096];
    int n = getdents64(fd, buf, 4096);
    close(fd);
    if (n <= 0) return 1;
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        /* Check if name is numeric */
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
                    /* Get just the program name (first null-terminated string) */
                    char *name = cmdline;
                    /* Extract basename */
                    char *slash = strrchr(name, '/');
                    if (slash) name = slash + 1;
                    for (int i = 0; i < argc; i++) {
                        if (strcmp(name, argv[i]) == 0) {
                            if (found) printf(" ");
                            printf("%s", d->d_name);
                            found = 1;
                            break;
                        }
                    }
                }
            }
        }
        pos += d->d_reclen;
    }
    if (found) printf("\n");
    return found ? 0 : 1;
}
