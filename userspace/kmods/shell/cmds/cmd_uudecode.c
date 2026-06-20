/* cmd_uudecode.c — decode uuencoded text to binary */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static int uu_char(char c) {
    return (c - '`') & 0x3F;
}

void cmd_uudecode(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: uudecode <file>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static char buf[4096];
    uint32_t size = 0;
    if (libc_vfs_read(path, buf, sizeof(buf) - 1, &size) != 0) {
        kprintf("uudecode: %s: not found\n", path);
        return;
    }
    buf[size] = '\0';

    /* Find "begin" line */
    char *p = buf;
    while (*p && *p != 'b') p++;
    if (*p == '\0' || strncmp(p, "begin", 5) != 0) {
        kprintf("uudecode: not a valid uuencoded file\n");
        return;
    }
    /* Skip to after begin line */
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;

    static unsigned char out[3072];
    uint32_t outpos = 0;
    while (*p && *p != '`' && *p != 'e') {
        if (*p == '\n' || *p == '\r') { p++; continue; }
        int line_len = uu_char(*p); p++;
        int written = 0;
        while (written < line_len && *p && *p != '\n' && *p != '\r') {
            unsigned char a = uu_char(p[0]);
            unsigned char b = (p[1] && p[1] != '\n' && p[1] != '\r') ? uu_char(p[1]) : 0;
            unsigned char c = (p[2] && p[2] != '\n' && p[2] != '\r') ? uu_char(p[2]) : 0;
            unsigned char d = (p[3] && p[3] != '\n' && p[3] != '\r') ? uu_char(p[3]) : 0;
            if (outpos < sizeof(out)) out[outpos++] = (a << 2) | (b >> 4);
            if (written + 1 < line_len && outpos < sizeof(out))
                out[outpos++] = (b << 4) | (c >> 2);
            if (written + 2 < line_len && outpos < sizeof(out))
                out[outpos++] = (c << 6) | d;
            p += 4;
            written += 3;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    kprintf("Decoded %u bytes to stdout\n", outpos);
}
