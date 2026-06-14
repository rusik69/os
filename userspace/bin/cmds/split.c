/* split.c — split file into pieces */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    const char *prefix = "x";
    unsigned long lines_per_file = 1000;
    int optind = 1;
    if (argc > optind && argv[optind][0] == '-' && argv[optind][1] == 'l') {
        if (argv[optind][2]) {
            lines_per_file = 0;
            char *p = argv[optind] + 2;
            while (*p >= '0' && *p <= '9') {
                lines_per_file = lines_per_file * 10 + (*p - '0');
                p++;
            }
        } else if (optind + 1 < argc) {
            optind++;
            lines_per_file = atoi(argv[optind]);
        }
        optind++;
    }
    /* Input file */
    int fd = STDIN_FILENO;
    if (optind < argc) {
        fd = open(argv[optind], O_RDONLY, 0);
        if (fd < 0) {
            printf("split: cannot open '%s'\n", argv[optind]);
            return 1;
        }
    }
    (void)prefix;
    /* Simple split by lines */
    char line[4096];
    unsigned long line_count = 0;
    int file_num = 0;
    int out_fd = -1;
    char outname[32];
    int in_pos = 0;
    char ch;
    while (read(fd, &ch, 1) == 1) {
        if (in_pos < 4095) line[in_pos++] = ch;
        if (ch == '\n' || in_pos >= 4095) {
            line[in_pos] = '\0';
            if (line_count == 0) {
                if (out_fd >= 0) close(out_fd);
                /* Generate output filename: x00, x01, ... */
                outname[0] = 'x';
                outname[1] = '0' + (file_num / 100) % 10;
                outname[2] = '0' + (file_num / 10) % 10;
                outname[3] = '0' + (file_num) % 10;
                outname[4] = '\0';
                out_fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out_fd < 0) {
                    printf("split: cannot create '%s'\n", outname);
                    close(fd);
                    return 1;
                }
                file_num++;
            }
            if (out_fd >= 0) write(out_fd, line, in_pos);
            line_count++;
            if (line_count >= lines_per_file) {
                line_count = 0;
            }
            in_pos = 0;
        }
    }
    if (out_fd >= 0) close(out_fd);
    if (fd != STDIN_FILENO) close(fd);
    return 0;
}
