/* cmd_tr.c — Translate characters */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

void cmd_tr(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: tr <from> <to> <file>\n");
        kprintf("  Translate characters: tr a-z A-Z file\n");
        return;
    }

    /* Parse: from_set to_set filename */
    char from_set[64], to_set[64], path[64];
    int ai = 0, wi = 0;
    char *words[3] = {from_set, to_set, path};
    int wlens[3] = {63, 63, 63};

    const char *p = args;
    while (*p && wi < 3) {
        while (*p == ' ') p++;
        ai = 0;
        while (*p && *p != ' ' && ai < wlens[wi]) words[wi][ai++] = *p++;
        words[wi][ai] = '\0';
        wi++;
    }
    if (wi < 3) { kprintf("Usage: tr <from> <to> <file>\n"); return; }

    /* Expand ranges like a-z → abc...z */
    char from_exp[128], to_exp[128];
    int fi = 0, ti = 0;

    for (int i = 0; from_set[i] && fi < 126; i++) {
        if (from_set[i+1] == '-' && from_set[i+2]) {
            char start = from_set[i], end = from_set[i+2];
            int step = (end >= start) ? 1 : -1;
            for (char c = start; ; c += step) {
                if (fi < 126) from_exp[fi++] = c;
                if (c == end) break;
            }
            i += 2;
        } else {
            from_exp[fi++] = from_set[i];
        }
    }
    from_exp[fi] = '\0';

    for (int i = 0; to_set[i] && ti < 126; i++) {
        if (to_set[i+1] == '-' && to_set[i+2]) {
            char start = to_set[i], end = to_set[i+2];
            int step = (end >= start) ? 1 : -1;
            for (char c = start; ; c += step) {
                if (ti < 126) to_exp[ti++] = c;
                if (c == end) break;
            }
            i += 2;
        } else {
            to_exp[ti++] = to_set[i];
        }
    }
    to_exp[ti] = '\0';

    /* Read file */
    char filepath[64];
    if (path[0] != '/') { filepath[0] = '/'; strncpy(filepath + 1, path, 62); }
    else strncpy(filepath, path, 63);
    filepath[63] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(filepath, buf, 4095, &size) != 0) {
        kprintf("tr: cannot read '%s'\n", filepath);
        return;
    }
    buf[size] = '\0';

    /* Translate and print */
    for (uint32_t i = 0; i < size; i++) {
        char c = buf[i];
        /* Find in from_exp */
        for (int j = 0; from_exp[j]; j++) {
            if (from_exp[j] == c && j < ti) {
                c = to_exp[j];
                break;
            }
        }
        kprintf("%c", c);
    }
}
