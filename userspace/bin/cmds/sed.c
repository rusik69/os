/* sed.c — Simple stream editor: s/pattern/replacement/ */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: sed s/pattern/replacement/ [file]\n");
        return 1;
    }
    /* Parse s/pattern/replacement/ */
    char *expr = argv[1];
    if (expr[0] != 's' || expr[1] != '/') {
        printf("sed: invalid expression (expected s/pattern/replacement/)\n");
        return 1;
    }
    char delim = '/';
    char pattern[256], replacement[256];
    int pi = 0, ri = 0;
    int i = 2; /* skip s/ */
    while (expr[i] && expr[i] != delim && pi < 255) pattern[pi++] = expr[i++];
    if (expr[i] == delim) i++;
    while (expr[i] && expr[i] != delim && ri < 255) replacement[ri++] = expr[i++];
    pattern[pi] = 0;
    replacement[ri] = 0;

    /* Read stdin or file */
    char buf[1024];
    int fd = 0; /* stdin */
    if (argc >= 3) {
        fd = open(argv[2], O_RDONLY, 0);
        if (fd < 0) { printf("sed: cannot open '%s'\n", argv[2]); return 1; }
    }
    int off = 0;
    int n;
    while ((n = read(fd, buf + off, (int)sizeof(buf) - 1 - off)) > 0) {
        off += n;
        buf[off] = 0;
        /* Process line by line */
        char *line = buf;
        while (1) {
            char *nl = strchr(line, '\n');
            if (!nl) {
                /* Move remaining to start */
                int remaining = buf + off - line;
                if (remaining > 0 && line != buf) {
                    memcpy(buf, line, remaining);
                    off = remaining;
                }
                break;
            }
            *nl = 0;
            /* Try to replace pattern in line */
            char *pos = strstr(line, pattern);
            if (pos) {
                write(1, line, pos - line);
                write(1, replacement, strlen(replacement));
                write(1, pos + strlen(pattern), strlen(pos + strlen(pattern)));
            } else {
                write(1, line, strlen(line));
            }
            write(1, "\n", 1);
            line = nl + 1;
        }
    }
    if (fd != 0) close(fd);
    return 0;
}
