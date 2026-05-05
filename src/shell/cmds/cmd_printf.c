/* cmd_printf.c — printf command: formatted output with escape sequences */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_printf(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: printf <format> [args...]\n");
        kprintf("  Escape sequences: \\n \\t \\\\\n");
        kprintf("  Format specs:     %%s %%d\n");
        return;
    }

    /* First space-delimited token is the format string */
    char fmt[256];
    int fi = 0;
    const char *p = args;
    while (*p && *p != ' ' && fi < 255) fmt[fi++] = *p++;
    fmt[fi] = '\0';
    while (*p == ' ') p++;

    /* Collect remaining tokens as substitution args */
    char argbuf[256];
    strncpy(argbuf, p, 255);
    argbuf[255] = '\0';

    char *words[16];
    int nwords = 0;
    char *w = argbuf;
    while (*w && nwords < 16) {
        while (*w == ' ') w++;
        if (!*w) break;
        words[nwords++] = w;
        while (*w && *w != ' ') w++;
        if (*w) *w++ = '\0';
    }
    int wi = 0;

    /* Walk format string */
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] == '\\') {
            i++;
            if      (fmt[i] == 'n')  kprintf("\n");
            else if (fmt[i] == 't')  kprintf("\t");
            else if (fmt[i] == '\\') kprintf("\\");
            else { kprintf("\\"); kprintf("%c", (uint64_t)(unsigned char)fmt[i]); }
        } else if (fmt[i] == '%') {
            i++;
            if (fmt[i] == 's') {
                if (wi < nwords) kprintf("%s", words[wi++]);
            } else if (fmt[i] == 'd') {
                if (wi < nwords) {
                    const char *s = words[wi++];
                    int neg = 0;
                    if (*s == '-') { neg = 1; s++; }
                    int64_t val = 0;
                    while (*s >= '0' && *s <= '9') val = val * 10 + (*s++ - '0');
                    if (neg) val = -val;
                    kprintf("%d", (uint64_t)val);
                }
            } else {
                kprintf("%%");
                kprintf("%c", (uint64_t)(unsigned char)fmt[i]);
            }
        } else {
            kprintf("%c", (uint64_t)(unsigned char)fmt[i]);
        }
    }
}
