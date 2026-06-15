/* nano.c — minimal line-based text editor */
#include "unistd.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

#define MAX_LINES 4096
#define MAX_LINE_LEN 4096

static char *lines[MAX_LINES];
static int line_count = 0;
static char filename[256];

static void add_line(const char *s, int len) {
    if (line_count >= MAX_LINES) return;
    char *p = malloc(len + 1);
    if (!p) return;
    memcpy(p, s, len);
    p[len] = 0;
    lines[line_count++] = p;
}

static void delete_line(int n) {
    if (n < 0 || n >= line_count) return;
    free(lines[n]);
    for (int i = n; i < line_count - 1; i++)
        lines[i] = lines[i + 1];
    line_count--;
}

static void load_file(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;
    char buf[4096];
    int n;
    int pos = 0;
    char linebuf[MAX_LINE_LEN];
    int linelen = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (linelen > 0 || pos > 0) {
                    add_line(linebuf, linelen);
                } else {
                    add_line("", 0);
                }
                linelen = 0;
                pos++;
            } else {
                if (linelen < MAX_LINE_LEN - 1)
                    linebuf[linelen++] = buf[i];
            }
        }
    }
    if (linelen > 0 || line_count == 0)
        add_line(linebuf, linelen);
    close(fd);
}

static void save_file(void) {
    if (filename[0] == 0) return;
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        write(2, "nano: cannot write ", 19);
        write(2, filename, strlen(filename));
        write(2, "\n", 1);
        return;
    }
    for (int i = 0; i < line_count; i++) {
        write(fd, lines[i], strlen(lines[i]));
        write(fd, "\n", 1);
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        int len = strlen(argv[1]);
        if (len >= 255) len = 255;
        memcpy(filename, argv[1], len);
        filename[len] = 0;
        load_file(filename);
    }
    if (line_count == 0)
        add_line("", 0);

    int dirty = 0;
    char input[MAX_LINE_LEN];

    for (;;) {
        /* Clear screen via escape sequence */
        write(1, "\033[2J\033[H", 7);

        /* Show status bar */
        write(1, "\033[44;37m", 8); /* blue bg, white fg */
        write(1, " Nano ", 6);
        if (filename[0]) {
            write(1, filename, strlen(filename));
        } else {
            write(1, "(New Buffer)", 12);
        }
        if (dirty) write(1, " [Modified]", 11);
        write(1, " \033[0m\n", 6);

        /* Display buffer with line numbers */
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

        write(1, "\n", 1);

        /* Status bar line */
        write(1, "\033[44;37m", 8);
        write(1, " :q=quit :w=save :wq=save+quit i=insert dd=delete-line", 55);
        write(1, " \033[0m", 5);
        write(1, "\n> ", 3);

        /* Read command */
        int inpos = 0;
        while (inpos < MAX_LINE_LEN - 1) {
            char c;
            int r = read(0, &c, 1);
            if (r <= 0) break;
            if (c == '\n') break;
            if (c == '\b' || c == 127) {
                if (inpos > 0) inpos--;
            } else {
                input[inpos++] = c;
            }
        }
        input[inpos] = 0;

        /* Parse command */
        if (strcmp(input, ":q") == 0) {
            break;
        } else if (strcmp(input, ":w") == 0) {
            if (filename[0] == 0) {
                write(1, "\nNo filename set. Use :w <filename>\n", 36);
            } else {
                save_file();
                dirty = 0;
            }
        } else if (strcmp(input, ":wq") == 0) {
            if (filename[0]) {
                save_file();
            }
            break;
        } else if (input[0] == ':' && input[1] == 'w' && input[2] == ' ') {
            /* :w filename */
            int fnlen = strlen(input + 3);
            if (fnlen >= 255) fnlen = 255;
            memcpy(filename, input + 3, fnlen);
            filename[fnlen] = 0;
            save_file();
            dirty = 0;
        } else if (strcmp(input, "i") == 0) {
            /* Insert mode — read lines and append */
            write(1, "\033[44;37m Insert mode (enter '.' on blank line to stop) \033[0m\n", 60);
            char ibuf[MAX_LINE_LEN];
            for (;;) {
                write(1, "> ", 2);
                int ilen = 0;
                while (ilen < MAX_LINE_LEN - 1) {
                    char c;
                    int r = read(0, &c, 1);
                    if (r <= 0 || c == '\n') break;
                    if (c == '\b' || c == 127) { if (ilen > 0) ilen--; }
                    else { ibuf[ilen++] = c; }
                }
                ibuf[ilen] = 0;
                if (strcmp(ibuf, ".") == 0) break;
                add_line(ibuf, ilen);
                dirty = 1;
            }
        } else if (strcmp(input, "dd") == 0) {
            if (line_count > 1) {
                /* Delete last line */
                delete_line(line_count - 1);
                dirty = 1;
            }
        } else if (input[0] == 'd' && input[1] == 'd' && input[2] >= '0' && input[2] <= '9') {
            /* d<N> to delete line N */
            int n = 0;
            for (int i = 2; input[i] >= '0' && input[i] <= '9'; i++)
                n = n * 10 + (input[i] - '0');
            if (n >= 1 && n <= line_count) {
                delete_line(n - 1);
                dirty = 1;
            }
        } else if (strcmp(input, "dd") == 0) {
            /* dd with no number deletes last line - already handled above */
        }
    }

    /* Free lines */
    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    return 0;
}
