/* cmd_expand.c -- Convert tabs to spaces */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_expand(int argc, char **argv) {
    int tabstop = 8;
    int optind = 1;

    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "-t") == 0) {
            if (optind + 1 >= argc) {
                kprintf("expand: -t requires an argument\n");
                return 1;
            }
            tabstop = atoi(argv[optind + 1]);
            if (tabstop < 1) tabstop = 1;
            optind += 2;
        } else if (strcmp(argv[optind], "--") == 0) {
            optind++;
            break;
        } else {
            kprintf("expand: unknown option '%s'\n", argv[optind]);
            return 1;
        }
    }

    /* Determine input source */
    const char *input = NULL;
    static char fbuf[4096];
    uint32_t fsize = 0;

    if (optind < argc) {
        /* Read from file */
        char path[64];
        const char *fn = argv[optind];
        if (fn[0] != '/') { path[0] = '/'; strncpy(path + 1, fn, 61); path[62] = '\0'; }
        else { strncpy(path, fn, 63); path[63] = '\0'; }
        int pl = (int)strlen(path);
        while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';
        if (vfs_read(path, fbuf, 4095, &fsize) != 0) {
            kprintf("expand: cannot read '%s'\n", fn);
            return 1;
        }
        fbuf[fsize] = '\0';
        input = fbuf;
    } else {
        /* Read from stdin */
        if (!shell_has_stdin()) {
            kprintf("Usage: expand [-t N] [file]\n");
            return 1;
        }
        fsize = (uint32_t)shell_stdin_read(fbuf, 4095);
        fbuf[fsize] = '\0';
        input = fbuf;
    }

    /* Process character by character */
    int col = 0;
    for (uint32_t i = 0; i < fsize; i++) {
        char c = input[i];
        if (c == '\t') {
            int spaces = tabstop - (col % tabstop);
            for (int s = 0; s < spaces; s++) {
                kprintf(" ");
                col++;
            }
        } else if (c == '\n') {
            kprintf("\n");
            col = 0;
        } else {
            kprintf("%c", c);
            col++;
        }
    }
    return 0;
}
