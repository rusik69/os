/* shuf.c — shuffle lines randomly */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_LINES 65536
#define MAX_LINE_LEN 4096

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Read all lines from stdin */
    char *lines[MAX_LINES];
    int num_lines = 0;
    char line[MAX_LINE_LEN];
    int pos = 0;
    char ch;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
        if (pos < MAX_LINE_LEN - 1) line[pos++] = ch;
        if (ch == '\n') {
            line[pos] = '\0';
            char *dup = malloc(pos + 1);
            if (!dup) break;
            memcpy(dup, line, pos + 1);
            lines[num_lines++] = dup;
            if (num_lines >= MAX_LINES) break;
            pos = 0;
        }
    }
    /* Handle last line without newline */
    if (pos > 0) {
        line[pos] = '\0';
        char *dup = malloc(pos + 1);
        if (dup) {
            memcpy(dup, line, pos + 1);
            lines[num_lines++] = dup;
        }
    }
    /* Fisher-Yates shuffle */
    struct timespec ts;
    clock_gettime(0, &ts);
    unsigned int seed = (unsigned int)(ts.tv_sec ^ ts.tv_nsec);
    seed ^= (unsigned int)getpid();
    srand(seed);
    for (int i = num_lines - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char *tmp = lines[i];
        lines[i] = lines[j];
        lines[j] = tmp;
    }
    /* Output */
    for (int i = 0; i < num_lines; i++) {
        write(STDOUT_FILENO, lines[i], strlen(lines[i]));
        free(lines[i]);
    }
    return 0;
}
