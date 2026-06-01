/* cmd_zgrep.c — grep inside gzip files */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static int decompress_any(const unsigned char *in, uint32_t in_size,
                          unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (in_size >= 2 && in[0] == 0x1f && in[1] == 0x8b) {
        uint32_t pos = 10;
        if (pos >= in_size) return -1;
        uint8_t flg = in[3];
        if (flg & 4) { if (pos + 2 > in_size) return -1; pos += 2 + (in[pos] | (in[pos+1] << 8)); }
        if (flg & 8) { while (pos < in_size && in[pos]) pos++; pos++; }
        if (flg & 16) { while (pos < in_size && in[pos]) pos++; pos++; }
        if (flg & 2) pos += 2;
        if (pos >= in_size) return -1;
        uint32_t opos = 0;
        while (pos < in_size - 8 && opos < out_max) {
            if (in[pos] == 0x00 && pos + 2 < in_size - 8) {
                unsigned char rl = in[pos+1], bv = in[pos+2];
                for (int k = 0; k < rl && opos < out_max; k++) out[opos++] = bv;
                pos += 3;
            } else out[opos++] = in[pos++];
        }
        *out_size = opos;
        return 0;
    }
    if (in_size >= 4 && in[0] == 'R' && in[1] == 'L' && in[2] == 'E') {
        uint32_t opos = 0, i = 4;
        while (i < in_size && opos < out_max) {
            if (in[i] == 0x00 && i + 2 < in_size) {
                unsigned char rl = in[i+1], bv = in[i+2];
                for (int k = 0; k < rl && opos < out_max; k++) out[opos++] = bv;
                i += 3;
            } else out[opos++] = in[i++];
        }
        *out_size = opos;
        return 0;
    }
    uint32_t copy = in_size < out_max ? in_size : out_max;
    memcpy(out, in, copy);
    *out_size = copy;
    return 0;
}

void cmd_zgrep(const char *args) {
    if (!args || !*args) { kprintf("Usage: zgrep <pattern> <file.gz> [files...]\n"); return; }

    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';

    char *pattern = strtok(argbuf, " ");
    if (!pattern) { kprintf("Usage: zgrep <pattern> <file.gz>\n"); return; }

    char *f;
    int found = 0;

    while ((f = strtok((char *)0, " ")) != (char *)0) {
        char path[64];
        if (f[0] != '/') { path[0] = '/'; strncpy(path + 1, f, 62); path[63] = '\0'; }
        else strncpy(path, f, 63); path[63] = '\0';

        static unsigned char buf[8192], decomp[4096];
        uint32_t size = 0, dsize = 0;
        if (libc_vfs_read(path, buf, sizeof(buf), &size) != 0) {
            kprintf("zgrep: %s: not found\n", f);
            continue;
        }
        if (decompress_any(buf, size, decomp, sizeof(decomp), &dsize) != 0) continue;
        decomp[dsize] = '\0';

        /* Search line by line */
        char *line = (char *)decomp;
        int lineno = 0;
        for (uint32_t i = 0; i <= dsize; i++) {
            if (decomp[i] == '\n' || i == dsize) {
                decomp[i] = '\0';
                lineno++;
                if (strstr(line, pattern) != (char *)0) {
                    kprintf("%s:%d:%s\n", f, lineno, line);
                    found++;
                }
                line = (char *)&decomp[i + 1];
            }
        }
    }

    if (found == 0) kprintf("No matches\n");
}
