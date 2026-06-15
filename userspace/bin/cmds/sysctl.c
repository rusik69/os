/* sysctl.c — system control */
#include "unistd.h"
#include "string.h"
int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: sysctl [-a] [key=value...]\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    if (argv[1][0] == '-') {
        const char *msg = "sysctl: use -a for all\n";
        write(1, msg, strlen(msg));
        return 0;
    }
    char path[256];int pos = 0;
    for (int i = 0; argv[1][i] && pos < 200; i++) {
        if (argv[1][i] == '.') { path[pos++] = '/'; }
        else { path[pos++] = argv[1][i]; }
    }
    path[pos] = 0;
    char full[512];int fpos = 0;
    const char *prefix = "/proc/sys/";
    for (int i = 0; prefix[i]; i++) full[fpos++] = prefix[i];
    for (int i = 0; path[i]; i++) full[fpos++] = path[i];
    full[fpos] = 0;
    int fd = open(full, O_RDONLY, 0);
    if (fd < 0) {
        const char *msg = "sysctl: cannot read ";
        write(1, msg, strlen(msg));
        write(1, full, strlen(full));
        write(1, "\n", 1);
        return 1;
    }
    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        write(1, argv[1], strlen(argv[1]));
        write(1, " = ", 3);
        write(1, buf, strlen(buf));
    }
    close(fd);
    return 0;
}
