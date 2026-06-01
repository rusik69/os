/* cmd_zdiff.c — diff two gzip files after decompression */
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

void cmd_zdiff(const char *args) {
    if (!args || !*args) { kprintf("Usage: zdiff <file1.gz> <file2.gz>\n"); return; }

    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';
    char *f1 = strtok(argbuf, " ");
    char *f2 = strtok((char *)0, " ");
    if (!f1 || !f2) { kprintf("Usage: zdiff <file1.gz> <file2.gz>\n"); return; }

    char p1[64], p2[64];
    if (f1[0] != '/') { p1[0] = '/'; strncpy(p1+1, f1, 62); p1[63] = '\0'; }
    else strncpy(p1, f1, 63); p1[63] = '\0';
    if (f2[0] != '/') { p2[0] = '/'; strncpy(p2+1, f2, 62); p2[63] = '\0'; }
    else strncpy(p2, f2, 63); p2[63] = '\0';

    static unsigned char b1[4096], b2[4096], d1[4096], d2[4096];
    uint32_t s1, s2, ds1, ds2;
    if (libc_vfs_read(p1, b1, sizeof(b1), &s1) != 0) { kprintf("zdiff: %s: not found\n", f1); return; }
    if (libc_vfs_read(p2, b2, sizeof(b2), &s2) != 0) { kprintf("zdiff: %s: not found\n", f2); return; }

    if (decompress_any(b1, s1, d1, sizeof(d1), &ds1) != 0 ||
        decompress_any(b2, s2, d2, sizeof(d2), &ds2) != 0) {
        kprintf("zdiff: decompression failed\n");
        return;
    }

    d1[ds1] = '\0'; d2[ds2] = '\0';

    char *l1 = (char *)d1, *l2 = (char *)d2;
    int lineno = 0, diffs = 0;

    while (*l1 || *l2) {
        lineno++;
        char *e1 = l1, *e2 = l2;
        while (*e1 && *e1 != '\n') e1++;
        while (*e2 && *e2 != '\n') e2++;
        int len1 = (int)(e1 - l1), len2 = (int)(e2 - l2);

        if (len1 != len2 || memcmp(l1, l2, len1) != 0) {
            diffs++;
            kprintf("%dc%d\n", (unsigned long)lineno, (unsigned long)lineno);
            kprintf("< ");
            for (int j = 0; j < len1; j++) kprintf("%c", (unsigned long)(uint8_t)l1[j]);
            kprintf("\n---\n> ");
            for (int j = 0; j < len2; j++) kprintf("%c", (unsigned long)(uint8_t)l2[j]);
            kprintf("\n");
        }
        l1 = (*e1 == '\n') ? e1 + 1 : e1;
        l2 = (*e2 == '\n') ? e2 + 1 : e2;
    }

    if (diffs == 0) kprintf("Files are identical\n");
}
