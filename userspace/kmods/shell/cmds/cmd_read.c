/* cmd_read.c — read builtin: read a line from stdin into variables */
#include "shell_cmds.h"
#include "shell.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_read(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: read [-r] <variable> [<variable>...]\n");
        return;
    }

    int raw = 0;
    const char *p = args;

    /* Parse options */
    if (*p == '-') {
        p++;
        while (*p && *p != ' ') {
            if (*p == 'r') raw = 1;
            p++;
        }
        while (*p == ' ') p++;
    }

    /* Read a line from keyboard */
    char buf[512];
    int pos = 0;
    for (;;) {
        int c = keyboard_getchar();
        if (c == '\n' || c == '\r') {
            kprintf("\n");
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) { pos--; kprintf("\b \b"); }
            continue;
        }
        if (c >= 32 && c <= 126 && pos < (int)sizeof(buf) - 1) {
            buf[pos++] = (char)c;
            kprintf("%c", (char)c);
        }
    }
    buf[pos] = '\0';

    /* Parse line into words (IFS splitting) */
    const char *words[16];
    int nwords = 0;
    char *s = buf;

    while (*s && nwords < 16) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        words[nwords++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) { *s++ = '\0'; }
    }

    /* Assign to variables */
    int wi = 0;
    const char *vp = args;
    if (*vp == '-') {
        while (*vp && *vp != ' ') vp++;
        while (*vp == ' ') vp++;
    }

    while (*vp && wi < nwords) {
        char varname[32];
        int vi = 0;
        while (*vp && *vp != ' ' && vi < 31) varname[vi++] = *vp++;
        varname[vi] = '\0';
        while (*vp == ' ') vp++;

        if (vi > 0) {
            shell_var_set(varname, words[wi]);
            wi++;
        }
    }

    /* If there are remaining words, assign to last variable */
    if (wi < nwords) {
        /* Find last variable name */
        const char *last = args;
        if (*last == '-') {
            while (*last && *last != ' ') last++;
            while (*last == ' ') last++;
        }
        const char *last_start = last;
        while (*last) {
            if (*last == ' ') last_start = last + 1;
            last++;
        }
        char lastvar[32];
        int lvi = 0;
        while (*last_start && *last_start != ' ' && lvi < 31)
            lastvar[lvi++] = *last_start++;
        lastvar[lvi] = '\0';

        if (lvi > 0) {
            char remaining[512];
            int ri = 0;
            while (wi < nwords && ri < 511) {
                const char *w = words[wi++];
                while (*w && ri < 511) remaining[ri++] = *w++;
                if (wi < nwords && ri < 511) remaining[ri++] = ' ';
            }
            remaining[ri] = '\0';
            shell_var_set(lastvar, remaining);
        }
    }
}
