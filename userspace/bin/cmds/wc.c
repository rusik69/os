/* wc.c — word count (lines, words, chars) */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

static void wc_file(const char *path, unsigned long *lines,
                    unsigned long *words, unsigned long *chars) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;

    char buf[4096];
    int nread;
    int in_word = 0;

    while ((nread = read(fd, buf, 4096)) > 0) {
        for (int i = 0; i < nread; i++) {
            (*chars)++;
            if (buf[i] == '\n') (*lines)++;
            if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                (*words)++;
            }
        }
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* Read stdin */
        unsigned long lines = 0, words = 0, chars = 0;
        char buf[4096];
        int nread;
        int in_word = 0;
        while ((nread = read(0, buf, 4096)) > 0) {
            for (int i = 0; i < nread; i++) {
                chars++;
                if (buf[i] == '\n') lines++;
                if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    words++;
                }
            }
        }
        printf("%6lu %6lu %6lu\n", lines, words, chars);
        return 0;
    }

    unsigned long t_lines = 0, t_words = 0, t_chars = 0;
    for (int i = 1; i < argc; i++) {
        unsigned long lines = 0, words = 0, chars = 0;
        wc_file(argv[i], &lines, &words, &chars);
        printf("%6lu %6lu %6lu %s\n", lines, words, chars, argv[i]);
        t_lines += lines;
        t_words += words;
        t_chars += chars;
    }
    if (argc > 2) {
        printf("%6lu %6lu %6lu total\n", t_lines, t_words, t_chars);
    }
    return 0;
}
