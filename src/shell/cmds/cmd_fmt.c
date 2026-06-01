/* cmd_fmt.c -- Simple text formatter */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

static int is_paragraph_sep(const char *line, int len) {
    if (len == 0) return 1;
    for (int i = 0; i < len; i++)
        if (!isspace((unsigned char)line[i])) return 0;
    return 1;
}

int cmd_fmt(int argc, char **argv) {
    int width = 75;
    int optind = 1;

    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "-w") == 0) {
            if (optind + 1 >= argc) {
                kprintf("fmt: -w requires an argument\n");
                return 1;
            }
            width = atoi(argv[optind + 1]);
            if (width < 10) width = 10;
            optind += 2;
        } else if (strcmp(argv[optind], "--") == 0) {
            optind++; break;
        } else {
            kprintf("fmt: unknown option '%s'\n", argv[optind]);
            return 1;
        }
    }

    /* Read input */
    static char fbuf[16384];
    uint32_t fsize = 0;

    if (optind < argc) {
        char path[64];
        const char *fn = argv[optind];
        if (fn[0] != '/') { path[0] = '/'; strncpy(path + 1, fn, 61); path[62] = '\0'; }
        else { strncpy(path, fn, 63); path[63] = '\0'; }
        int pl = (int)strlen(path);
        while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';
        if (vfs_read(path, fbuf, (uint32_t)(sizeof(fbuf) - 1), &fsize) != 0) {
            kprintf("fmt: cannot read '%s'\n", fn);
            return 1;
        }
        fbuf[fsize] = '\0';
    } else {
        if (!shell_has_stdin()) {
            kprintf("Usage: fmt [-w WIDTH] [file]\n");
            return 1;
        }
        fsize = (uint32_t)shell_stdin_read(fbuf, (int)sizeof(fbuf) - 1);
        fbuf[fsize] = '\0';
    }

    /* Split input into lines */
    char *lines[2048];
    int nlines = 0;
    char *p = fbuf;
    while (*p && nlines < 2048) {
        lines[nlines++] = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = '\0'; p++; }
    }

    /* Process paragraphs */
    char word[4096];
    int  word_len = 0;
    char out_line[4096];
    int  out_col = 0;

    void flush_line(void) {
        if (out_col > 0) {
            out_line[out_col] = '\0';
            kprintf("%s\n", out_line);
            out_col = 0;
        }
    }
    void flush_word(void) {
        if (word_len == 0) return;
        word[word_len] = '\0';
        int wlen = word_len;
        if (out_col + (out_col > 0 ? 1 : 0) + wlen > width) {
            flush_line();
        }
        if (out_col > 0) {
            out_line[out_col++] = ' ';
        }
        for (int i = 0; i < wlen; i++)
            out_line[out_col++] = word[i];
        word_len = 0;
    }

    for (int i = 0; i < nlines; i++) {
        int len = (int)strlen(lines[i]);
        if (is_paragraph_sep(lines[i], len)) {
            flush_word();
            flush_line();
            kprintf("\n");
            continue;
        }
        /* Tokenize line into words */
        char *t = lines[i];
        while (*t) {
            while (*t == ' ' || *t == '\t') t++;
            if (*t == '\0') break;
            word_len = 0;
            while (*t && *t != ' ' && *t != '\t' && word_len < 4095)
                word[word_len++] = *t++;
            flush_word();
        }
    }
    flush_word();
    flush_line();
    return 0;
}
