/* grep.c — search for substring in files or stdin */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void search(const char *path, const char *pattern, int show_lineno) {
    int fd = STDIN_FILENO;
    int should_close = 0;
    if (path) {
        fd = open(path, O_RDONLY, 0);
        if (fd < 0) { printf("grep: %s: No such file\n", path); return; }
        should_close = 1;
    }
    char line[1024];
    int line_len = 0;
    int lineno = 0;
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                lineno++;
                if (strstr(line, pattern)) {
                    if (show_lineno) printf("%d:", lineno);
                    printf("%s\n", line);
                }
                line_len = 0;
            } else if (line_len < 1023) {
                line[line_len++] = buf[i];
            }
        }
    }
    if (line_len > 0) {
        line[line_len] = '\0';
        lineno++;
        if (strstr(line, pattern)) {
            if (show_lineno) printf("%d:", lineno);
            printf("%s\n", line);
        }
    }
    if (should_close) close(fd);
}

int main(int argc, char *argv[]) {
    int optind = 1, show_lineno = 0;
    if (argc < 2) { printf("Usage: grep [-n] <pattern> [file...]\n"); return 1; }
    if (strcmp(argv[1], "-n") == 0) { show_lineno = 1; optind = 2; }
    if (optind >= argc) { printf("Usage: grep [-n] <pattern> [file...]\n"); return 1; }
    const char *pattern = argv[optind];
    if (optind + 1 >= argc) search(NULL, pattern, show_lineno);
    else for (int i = optind + 1; i < argc; i++) search(argv[i], pattern, show_lineno);
    return 0;
}
