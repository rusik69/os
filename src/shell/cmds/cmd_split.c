/* cmd_split.c — split a file into line-count pieces */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

void cmd_split(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: split [-l lines] <file> [prefix]\n");
        return;
    }

    int chunk = 100;
    const char *p = args;

    if (*p == '-' && *(p+1) == 'l') {
        p += 2;
        while (*p == ' ') p++;
        chunk = (int)strtol(p, (char **)&p, 10);
        if (chunk < 1) chunk = 100;
        while (*p == ' ') p++;
    }

    if (!*p) { kprintf("split: no file specified\n"); return; }

    char inpath[64], prefix[32];
    int i = 0;
    while (*p && *p != ' ' && i < 63) inpath[i++] = *p++;
    inpath[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 31) prefix[i++] = *p++;
    prefix[i] = '\0';
    if (!prefix[0]) strcpy(prefix, "x");

    char path[64];
    if (inpath[0] != '/') { path[0] = '/'; strncpy(path+1, inpath, 62); }
    else strncpy(path, inpath, 63);
    path[63] = '\0';

    static char buf[8192];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("split: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    int part = 0;
    int line = 0;
    char *start = buf;
    char *q = buf;

    while (*q || q > start) {
        /* Find next line end */
        char *nl = strchr(q, '\n');
        if (!nl) nl = buf + size;
        else nl++;
        q = nl;
        line++;

        if (line >= chunk || !*q) {
            /* Write this chunk */
            char outname[96];
            char suffix[8];
            itoa(part, suffix, 10);
            /* zero-pad to 2 digits */
            snprintf(outname, sizeof(outname), "/%s%s%s", prefix,
                     (part < 10 ? "0" : ""), suffix);
            uint32_t len = (uint32_t)(q - start);
            vfs_create(outname, 1);
            vfs_write(outname, start, len);
            kprintf("  %s (%u bytes)\n", outname, (uint64_t)len);
            start = q;
            line = 0;
            part++;
        }
    }
    if (part == 0) kprintf("split: empty file\n");
}
