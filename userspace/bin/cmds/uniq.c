/* uniq.c — report or omit repeated lines */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

#define MAX_LINE 1024

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char buf[4096];
    char prev[MAX_LINE];
    int has_prev = 0;
    int nread;
    int pos = 0;

    while ((nread = read(0, buf + pos, 4096 - pos)) > 0) {
        nread += pos;
        int start = 0;
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                char *line = buf + start;
                if (!has_prev || strcmp(line, prev) != 0) {
                    write(1, line, strlen(line));
                    write(1, "\n", 1);
                    unsigned long len = strlen(line);
                    if (len >= MAX_LINE) len = MAX_LINE - 1;
                    for (unsigned long k = 0; k < len; k++) prev[k] = line[k];
                    prev[len] = '\0';
                    has_prev = 1;
                }
                start = i + 1;
            }
        }
        pos = nread - start;
        if (start < nread) {
            for (int i = 0; i < pos; i++) buf[i] = buf[start + i];
        } else {
            pos = 0;
        }
    }
    return 0;
}
