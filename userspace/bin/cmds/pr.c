/* pr.c — convert text files for printing (simple page breaks) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define LINES_PER_PAGE 66
#define HEADER_LINES 5

int main(int argc, char *argv[]) {
    int fd = STDIN_FILENO;
    int should_close = 0;
    if (argc > 1) {
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) { printf("pr: cannot open '%s'\n", argv[1]); return 1; }
        should_close = 1;
    }
    char buf[4096];
    int line_count = 0;
    int page = 1;
    char line[1024];
    int line_len = 0;
    int nread;
    while ((nread = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                if (line_count == 0) {
                    /* Page header */
                    printf("\n\n\n"); /* 3 blank lines */
                    printf("Page %d\n\n", page);
                    line_count = HEADER_LINES;
                }
                printf("%s\n", line);
                line_count++;
                line_len = 0;
                if (line_count >= LINES_PER_PAGE) {
                    printf("\f"); /* form feed marker */
                    page++;
                    line_count = 0;
                }
            } else if (line_len < 1023) {
                line[line_len++] = buf[i];
            }
        }
    }
    if (line_len > 0) {
        line[line_len] = '\0';
        if (line_count == 0) {
            printf("\n\n\nPage %d\n\n", page);
        }
        printf("%s\n", line);
    }
    if (should_close) close(fd);
    return 0;
}
