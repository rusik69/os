/* column.c — columnate text: read stdin, format into columns */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_LINES 4096
#define MAX_COLS 16

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char *lines[MAX_LINES];
    int count = 0;
    char buf[4096];
    int n;
    while ((n = read(0, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        char *p = buf;
        while (*p && count < MAX_LINES) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            lines[count] = malloc(strlen(p) + 1);
            if (lines[count]) strcpy(lines[count], p);
            count++;
            if (nl) {
                *nl = '\n';
                p = nl + 1;
            } else {
                break;
            }
        }
    }
    if (count == 0) return 0;
    unsigned long maxlen = 0;
    for (int i = 0; i < count; i++) {
        unsigned long len = strlen(lines[i]);
        if (len > maxlen) maxlen = len;
    }
    int cols = 80 / (maxlen + 2);
    if (cols < 1) cols = 1;
    if (cols > MAX_COLS) cols = MAX_COLS;
    int rows = (count + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = c * rows + r;
            if (idx >= count) break;
            printf("%s", lines[idx]);
            if (c < cols - 1) {
                for (unsigned long s = strlen(lines[idx]); s < maxlen + 2; s++)
                    printf(" ");
            }
        }
        printf("\n");
    }
    for (int i = 0; i < count; i++) free(lines[i]);
    return 0;
}
