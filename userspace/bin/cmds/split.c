/* split.c — split file into pieces by lines (-l) or bytes (-b)
 * Numeric suffixes: 1k=1024, 1m=1024^2, 1g=1024^3. Output files named
 * x00, x01, ... in the current directory. */
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define BUF 8192

/* Parse a decimal size with optional k/m/g (KiB) multiplier. */
static unsigned long parse_size(const char *s) {
    unsigned long n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    if (*s == 'k' || *s == 'K')
        return n * 1024;
    if (*s == 'm' || *s == 'M')
        return n * 1024 * 1024;
    if (*s == 'g' || *s == 'G')
        return n * 1024 * 1024 * 1024;
    return n;
}

/* Write name of the next output file (x00, x01, ...) into out. */
static void next_name(char *out, int num) {
    out[0] = 'x';
    out[1] = '0' + (num / 100) % 10;
    out[2] = '0' + (num / 10) % 10;
    out[3] = '0' + num % 10;
    out[4] = '\0';
}

int main(int argc, char *argv[]) {
    int by_bytes = 0;
    unsigned long limit = 1000;
    int optind = 1;

    if (argc > 1 && argv[1][0] == '-') {
        char c = argv[1][1];
        char *val = argv[1] + 2;
        if (c == 'l' || c == 'b') {
            by_bytes = (c == 'b');
            if (*val) {
                limit = parse_size(val);
            } else if (optind + 1 < argc) {
                optind++;
                limit = parse_size(argv[optind]);
            }
            optind++;
        }
    }

    int fd = STDIN_FILENO;
    if (optind < argc) {
        fd = open(argv[optind], O_RDONLY, 0);
        if (fd < 0) {
            printf("split: cannot open '%s'\n", argv[optind]);
            return 1;
        }
    }

    char outname[8];
    int file_num = 0;
    int out_fd = -1;

    if (by_bytes) {
        static char buf[BUF];
        unsigned long in_file = 0;
        int n;
        while ((n = read(fd, buf, BUF)) > 0) {
            unsigned long i = 0;
            while (i < (unsigned long)n) {
                if (out_fd < 0) {
                    next_name(outname, file_num);
                    out_fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (out_fd < 0) {
                        printf("split: cannot create '%s'\n", outname);
                        if (fd != STDIN_FILENO)
                            close(fd);
                        return 1;
                    }
                    file_num++;
                }
                unsigned long take = limit - in_file;
                if (take > (unsigned long)n - i)
                    take = (unsigned long)n - i;
                write(out_fd, buf + i, take);
                i += take;
                in_file += take;
                if (in_file >= limit) {
                    close(out_fd);
                    out_fd = -1;
                    in_file = 0;
                }
            }
        }
    } else {
        /* Split by lines. */
        char line[4096];
        unsigned long line_count = 0;
        unsigned long in_pos = 0;
        char ch;
        while (read(fd, &ch, 1) == 1) {
            if (in_pos < 4095)
                line[in_pos++] = ch;
            if (ch == '\n' || in_pos >= 4095) {
                line[in_pos] = '\0';
                if (out_fd < 0) {
                    next_name(outname, file_num);
                    out_fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (out_fd < 0) {
                        printf("split: cannot create '%s'\n", outname);
                        if (fd != STDIN_FILENO)
                            close(fd);
                        return 1;
                    }
                    file_num++;
                }
                write(out_fd, line, in_pos);
                line_count++;
                if (line_count >= limit) {
                    close(out_fd);
                    out_fd = -1;
                    line_count = 0;
                }
                in_pos = 0;
            }
        }
    }

    if (out_fd >= 0)
        close(out_fd);
    if (fd != STDIN_FILENO)
        close(fd);
    return 0;
}