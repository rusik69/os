/* look.c — look for lines starting with prefix */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: look <prefix> [file]\n"); return 1; }
    const char *prefix = argv[1];
    int fd = STDIN_FILENO;
    int should_close = 0;
    if (argc > 2) {
        fd = open(argv[2], O_RDONLY, 0);
        if (fd < 0) { printf("look: %s: No such file\n", argv[2]); return 1; }
        should_close = 1;
    }
    char buf[4096];
    char line[1024];
    int line_len = 0, nread;
    int plen = 0;
    while (prefix[plen]) plen++;
    while ((nread = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                if (line_len >= plen && strncmp(line, prefix, plen) == 0)
                    printf("%s\n", line);
                line_len = 0;
            } else if (line_len < 1023) {
                line[line_len++] = buf[i];
            }
        }
    }
    if (line_len >= plen && strncmp(line, prefix, plen) == 0) {
        line[line_len] = '\0';
        printf("%s\n", line);
    }
    if (should_close) close(fd);
    return 0;
}
