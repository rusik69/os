/* edit.c — simple line-based text editor using stdin/stdout */
#include "unistd.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

#define MAX_LINES 4096
#define MAX_LINE_LEN 4096

static char *lines[MAX_LINES];
static int line_count = 0;

static void add_line(const char *s, int len) {
    if (line_count >= MAX_LINES) return;
    char *p = malloc(len + 1);
    if (!p) return;
    memcpy(p, s, len);
    p[len] = 0;
    lines[line_count++] = p;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        write(2, "usage: edit <file>\n", 19);
        return 1;
    }
    const char *filename = argv[1];

    /* Load existing file if it exists */
    int fd = open(filename, O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        char linebuf[MAX_LINE_LEN];
        int linelen = 0;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            for (int i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    add_line(linebuf, linelen);
                    linelen = 0;
                } else {
                    if (linelen < MAX_LINE_LEN - 1)
                        linebuf[linelen++] = buf[i];
                }
            }
        }
        if (linelen > 0 || line_count == 0)
            add_line(linebuf, linelen);
        close(fd);

        /* Display existing content */
        char numbuf[16];
        for (int i = 0; i < line_count; i++) {
            int num = i + 1;
            int numlen = 0;
            int tmp = num;
            do { tmp /= 10; numlen++; } while (tmp > 0);
            if (numlen < 4) numlen = 4;
            numbuf[numlen] = 0;
            tmp = num;
            for (int j = numlen - 1; j >= 0; j--) {
                numbuf[j] = '0' + (tmp % 10);
                tmp /= 10;
            }
            write(1, " ", 1);
            write(1, numbuf, numlen);
            write(1, "| ", 2);
            write(1, lines[i], strlen(lines[i]));
            write(1, "\n", 1);
        }
        write(1, "\n--- Edit below. Ctrl+D to save and exit ---\n", 45);
    } else {
        write(1, "--- New file. Enter lines. Ctrl+D to save and exit ---\n", 55);
    }

    /* Read lines from stdin until EOF */
    char input[MAX_LINE_LEN];
    while (1) {
        write(1, "> ", 2);
        int inpos = 0;
        while (inpos < MAX_LINE_LEN - 1) {
            char c;
            int r = read(0, &c, 1);
            if (r <= 0) {
                /* EOF (Ctrl+D) */
                goto save_and_exit;
            }
            if (c == '\n') break;
            if (c == '\b' || c == 127) {
                if (inpos > 0) inpos--;
            } else {
                input[inpos++] = c;
            }
        }
        input[inpos] = 0;
        add_line(input, inpos);
    }

save_and_exit:
    /* Write buffer to file */
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        write(2, "edit: cannot write ", 19);
        write(2, filename, strlen(filename));
        write(2, "\n", 1);
        return 1;
    }
    for (int i = 0; i < line_count; i++) {
        write(fd, lines[i], strlen(lines[i]));
        write(fd, "\n", 1);
    }
    close(fd);

    write(1, "\nedit: wrote ", 13);
    char nbuf[16];
    int nlen = 0;
    int tmp = line_count;
    do { tmp /= 10; nlen++; } while (tmp > 0);
    nbuf[nlen] = 0;
    tmp = line_count;
    for (int j = nlen - 1; j >= 0; j--) { nbuf[j] = '0' + (tmp % 10); tmp /= 10; }
    write(1, nbuf, nlen);
    write(1, " lines to ", 10);
    write(1, filename, strlen(filename));
    write(1, "\n", 1);

    /* Free lines */
    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    return 0;
}
