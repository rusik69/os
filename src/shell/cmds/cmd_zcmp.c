/* cmd_zcmp.c — compare two gzip files after decompression */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static int decompress_gzip(const unsigned char *in, uint32_t in_size,
                           unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (in_size < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return -1;
    uint32_t pos = 10;
    uint8_t flg = in[3];
    if (flg & 4) { if (pos + 2 > in_size) return -1; pos += 2 + (in[pos] | (in[pos+1] << 8)); }
    if (flg & 8) { while (pos < in_size && in[pos]) pos++; pos++; }
    if (flg & 16) { while (pos < in_size && in[pos]) pos++; pos++; }
    if (flg & 2) pos += 2;
    if (pos >= in_size) return -1;

    uint32_t opos = 0;
    while (pos < in_size - 8 && opos < out_max) {
        if (in[pos] == 0x00 && pos + 2 < in_size - 8) {
            unsigned char run_len = in[pos + 1];
            unsigned char byte_val = in[pos + 2];
            for (int k = 0; k < run_len && opos < out_max; k++)
                out[opos++] = byte_val;
            pos += 3;
        } else {
            out[opos++] = in[pos++];
        }
    }
    *out_size = opos;
    return 0;
}

static int decompress_rle1(const unsigned char *in, uint32_t in_size,
                           unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (in_size < 4 || in[0] != 'R' || in[1] != 'L' || in[2] != 'E' || in[3] != '1') return -1;
    uint32_t opos = 0, i = 4;
    while (i < in_size && opos < out_max) {
        if (in[i] == 0x00 && i + 2 < in_size) {
            unsigned char run_len = in[i + 1], byte_val = in[i + 2];
            for (int k = 0; k < run_len && opos < out_max; k++) out[opos++] = byte_val;
            i += 3;
        } else out[opos++] = in[i++];
    }
    *out_size = opos;
    return 0;
}

static int decompress_any(const unsigned char *in, uint32_t in_size,
                          unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (in_size >= 2 && in[0] == 0x1f && in[1] == 0x8b)
        return decompress_gzip(in, in_size, out, out_max, out_size);
    if (in_size >= 4 && in[0] == 'R' && in[1] == 'L' && in[2] == 'E')
        return decompress_rle1(in, in_size, out, out_max, out_size);
    /* Not compressed — copy as-is */
    uint32_t copy = in_size < out_max ? in_size : out_max;
    memcpy(out, in, copy);
    *out_size = copy;
    return 0;
}

void cmd_zcmp(const char *args) {
    if (!args || !*args) { kprintf("Usage: zcmp <file1.gz> <file2.gz>\n"); return; }

    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';
    char *f1 = strtok(argbuf, " ");
    char *f2 = strtok((char *)0, " ");
    if (!f1 || !f2) { kprintf("Usage: zcmp <file1.gz> <file2.gz>\n"); return; }

    char p1[64], p2[64];
    if (f1[0] != '/') { p1[0] = '/'; strncpy(p1+1, f1, 62); p1[63] = '\0'; }
    else strncpy(p1, f1, 63); p1[63] = '\0';
    if (f2[0] != '/') { p2[0] = '/'; strncpy(p2+1, f2, 62); p2[63] = '\0'; }
    else strncpy(p2, f2, 63); p2[63] = '\0';

    static unsigned char b1[4096], b2[4096], d1[4096], d2[4096];
    uint32_t s1, s2, ds1, ds2;
    if (libc_vfs_read(p1, b1, sizeof(b1), &s1) != 0) { kprintf("zcmp: %s: not found\n", f1); return; }
    if (libc_vfs_read(p2, b2, sizeof(b2), &s2) != 0) { kprintf("zcmp: %s: not found\n", f2); return; }

    if (decompress_any(b1, s1, d1, sizeof(d1), &ds1) != 0 ||
        decompress_any(b2, s2, d2, sizeof(d2), &ds2) != 0) {
        kprintf("zcmp: decompression failed\n");
        return;
    }

    uint32_t min = ds1 < ds2 ? ds1 : ds2;
    for (uint32_t j = 0; j < min; j++) {
        if (d1[j] != d2[j]) {
            kprintf("%s %s differ: byte %u\n", f1, f2, (uint64_t)(j+1));
            return;
        }
    }
    if (ds1 != ds2)
        kprintf("%s %s differ: size %u vs %u\n", f1, f2, (uint64_t)ds1, (uint64_t)ds2);
    else
        kprintf("%s %s are identical\n", f1, f2);
}
