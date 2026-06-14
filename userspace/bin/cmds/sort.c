/* sort.c — sort lines of text files */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

#define MAX_LINES 4096
#define MAX_LINE 1024

static char lines[MAX_LINES][MAX_LINE];
static int num_lines = 0;

static void sort(void) {
    for (int i = 0; i < num_lines - 1; i++) {
        for (int j = 0; j < num_lines - 1 - i; j++) {
            if (strcmp(lines[j], lines[j + 1]) > 0) {
                char tmp[MAX_LINE];
                strcpy(tmp, lines[j]);
                strcpy(lines[j], lines[j + 1]);
                strcpy(lines[j + 1], tmp);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int fd = 0; /* stdin */
    if (argc > 1) {
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            printf("sort: cannot open '%s'\n", argv[1]);
            return 1;
        }
    }
    char buf[4096];
    int nread;
    int pos = 0;
    while ((nread = read(fd, buf + pos, 4096 - pos)) > 0) {
        nread += pos;
        int start = 0;
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                if (num_lines < MAX_LINES) {
                    unsigned long len = strlen(buf + start);
                    if (len >= MAX_LINE) len = MAX_LINE - 1;
                    for (unsigned long k = 0; k < len; k++)
                        lines[num_lines][k] = buf[start + k];
                    lines[num_lines][len] = '\0';
                    num_lines++;
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
    if (argc > 1) close(fd);

    sort();
    for (int i = 0; i < num_lines; i++) {
        write(1, lines[i], strlen(lines[i]));
        write(1, "\n", 1);
    }
    return 0;
}
