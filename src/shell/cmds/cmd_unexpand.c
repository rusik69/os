/* cmd_unexpand.c -- Convert spaces to tabs */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_unexpand(int argc, char **argv) {
    int tabstop = 8;
    int all = 0;
    int optind = 1;

    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "--") == 0) { optind++; break; }
        char *p = argv[optind] + 1;
        while (*p) {
            if (*p == 'a')      all = 1;
            else if (*p == 't') {
                if (p[1]) {
                    tabstop = atoi(p + 1);
                    p += strlen(p) - 1;
                } else if (optind + 1 < argc) {
                    tabstop = atoi(argv[optind + 1]);
                    optind++;
                    p += strlen(p) - 1;
                } else {
                    kprintf("unexpand: -t requires argument\n");
                    return 1;
                }
            } else {
                kprintf("unexpand: unknown option -- '%c'\n", *p);
                return 1;
            }
            p++;
        }
        optind++;
    }
    if (tabstop < 1) tabstop = 1;

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
            kprintf("unexpand: cannot read '%s'\n", fn);
            return 1;
        }
        fbuf[fsize] = '\0';
    } else {
        if (!shell_has_stdin()) {
            kprintf("Usage: unexpand [-a] [-t N] [file]\n");
            return 1;
        }
        fsize = (uint32_t)shell_stdin_read(fbuf, (int)sizeof(fbuf) - 1);
        fbuf[fsize] = '\0';
    }

    /* Process each line */
    char *p = fbuf;
    while (*p) {
        char line[4096];
        int li = 0;
        while (*p && *p != '\n' && li < 4095)
            line[li++] = *p++;
        line[li] = '\0';
        if (*p == '\n') p++;

        if (all) {
            /* Convert all runs of spaces to tabs */
            char out[8192];
            int oi = 0, col = 0;
            for (int si = 0; si < li; ) {
                if (line[si] == ' ') {
                    int sc = 0;
                    while (si + sc < li && line[si + sc] == ' ') sc++;
                    /* Emit tabs greedily */
                    int remain = sc;
                    while (remain > 0) {
                        int next_stop = ((col / tabstop) + 1) * tabstop;
                        int need = next_stop - col;
                        if (need <= remain && need > 0) {
                            out[oi++] = '\t';
                            col = next_stop;
                            remain -= need;
                        } else {
                            break;
                        }
                    }
                    while (remain-- > 0) { out[oi++] = ' '; col++; }
                    si += sc;
                } else if (line[si] == '\t') {
                    out[oi++] = '\t';
                    col = ((col / tabstop) + 1) * tabstop;
                    si++;
                } else {
                    out[oi++] = line[si];
                    col++;
                    si++;
                }
            }
            out[oi] = '\0';
            kprintf("%s\n", out);
        } else {
            /* Only convert leading spaces to tabs */
            int sc = 0;
            while (sc < li && line[sc] == ' ') sc++;
            int col = 0;
            int remain = sc;
            while (remain > 0) {
                int next_stop = ((col / tabstop) + 1) * tabstop;
                int need = next_stop - col;
                if (need <= remain && need > 0) {
                    kprintf("\t");
                    col = next_stop;
                    remain -= need;
                } else {
                    break;
                }
            }
            while (remain-- > 0) { kprintf(" "); col++; }
            /* Output rest of line (original pointer) */
            kprintf("%s\n", line + sc);
        }
    }
    return 0;
}
