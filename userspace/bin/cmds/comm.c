/* comm.c — compare two sorted files line by line */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_LINES 65536
#define MAX_LINE_LEN 4096

static int read_lines(const char *path, char lines[][MAX_LINE_LEN], int *n) {
    int fd = STDIN_FILENO, close_fd = 0;
    if (path) {
        fd = open(path, O_RDONLY, 0);
        if (fd < 0) return -1;
        close_fd = 1;
    }
    char buf[4096];
    int nread;
    char line[MAX_LINE_LEN];
    int line_len = 0;
    while ((nread = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                if (*n < MAX_LINES) {
                    int j = 0;
                    while (line[j]) { lines[*n][j] = line[j]; j++; }
                    lines[*n][j] = '\0';
                    (*n)++;
                }
                line_len = 0;
            } else if (line_len < MAX_LINE_LEN - 1) {
                line[line_len++] = buf[i];
            }
        }
    }
    if (line_len > 0) {
        line[line_len] = '\0';
        if (*n < MAX_LINES) {
            int j = 0;
            while (line[j]) { lines[*n][j] = line[j]; j++; }
            lines[*n][j] = '\0';
            (*n)++;
        }
    }
    if (close_fd) close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) { printf("Usage: comm <file1> <file2>\n"); return 1; }
    static char lines1[MAX_LINES][MAX_LINE_LEN];
    static char lines2[MAX_LINES][MAX_LINE_LEN];
    int n1 = 0, n2 = 0;
    if (read_lines(argv[1], lines1, &n1) < 0) { printf("comm: cannot open '%s'\n", argv[1]); return 1; }
    if (read_lines(argv[2], lines2, &n2) < 0) { printf("comm: cannot open '%s'\n", argv[2]); return 1; }
    int i = 0, j = 0;
    while (i < n1 || j < n2) {
        int cmp;
        if (i >= n1) cmp = 1;
        else if (j >= n2) cmp = -1;
        else cmp = strcmp(lines1[i], lines2[j]);
        if (cmp < 0) {
            printf("%s\n", lines1[i]);
            i++;
        } else if (cmp > 0) {
            printf("\t%s\n", lines2[j]);
            j++;
        } else {
            printf("\t\t%s\n", lines1[i]);
            i++; j++;
        }
    }
    return 0;
}
