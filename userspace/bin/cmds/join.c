/* join.c — join lines on common field */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_LINES 16384
#define MAX_LINE_LEN 4096

static int read_lines(const char *path, char lines[][MAX_LINE_LEN], int *n) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    char buf[4096];
    char line[MAX_LINE_LEN];
    int line_len = 0, nread;
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
    close(fd);
    return 0;
}

static const char *first_field(const char *line, int *len) {
    while (*line == ' ' || *line == '\t') line++;
    const char *start = line;
    while (*line && *line != ' ' && *line != '\t') line++;
    *len = line - start;
    return start;
}

int main(int argc, char *argv[]) {
    if (argc < 3) { printf("Usage: join <file1> <file2>\n"); return 1; }
    static char lines1[MAX_LINES][MAX_LINE_LEN];
    static char lines2[MAX_LINES][MAX_LINE_LEN];
    int n1 = 0, n2 = 0;
    if (read_lines(argv[1], lines1, &n1) < 0) { printf("join: cannot open '%s'\n", argv[1]); return 1; }
    if (read_lines(argv[2], lines2, &n2) < 0) { printf("join: cannot open '%s'\n", argv[2]); return 1; }
    for (int i = 0; i < n1; i++) {
        int len1;
        const char *f1 = first_field(lines1[i], &len1);
        for (int j = 0; j < n2; j++) {
            int len2;
            const char *f2 = first_field(lines2[j], &len2);
            if (len1 == len2 && strncmp(f1, f2, len1) == 0) {
                /* Print joined line: field, rest of line1, rest of line2 */
                int k;
                for (k = 0; k < len1; k++) write(STDOUT_FILENO, f1 + k, 1);
                write(STDOUT_FILENO, " ", 1);
                const char *rest = lines1[i] + (f1 - lines1[i]) + len1;
                while (*rest == ' ' || *rest == '\t') rest++;
                while (*rest) { write(STDOUT_FILENO, rest, 1); rest++; }
                write(STDOUT_FILENO, " ", 1);
                rest = lines2[j] + (f2 - lines2[j]) + len2;
                while (*rest == ' ' || *rest == '\t') rest++;
                while (*rest) { write(STDOUT_FILENO, rest, 1); rest++; }
                write(STDOUT_FILENO, "\n", 1);
            }
        }
    }
    return 0;
}
