/* cmd_zcat.c — decompress gzip to stdout */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static int gzip_decompress(const unsigned char *in, uint32_t in_size,
                           unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (in_size < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return -1;

    uint32_t pos = 10;
    uint8_t flg = in[3];
    if (flg & 4) {
        if (pos + 2 > in_size) return -1;
        uint16_t xlen = (uint16_t)(in[pos] | (in[pos+1] << 8));
        pos += 2 + xlen;
    }
    if (flg & 8) { while (pos < in_size && in[pos]) pos++; pos++; }
    if (flg & 16) { while (pos < in_size && in[pos]) pos++; pos++; }
    if (flg & 2) { pos += 2; }
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

static int rle1_decompress(const unsigned char *in, uint32_t in_size,
                           unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (in_size < 4 || in[0] != 'R' || in[1] != 'L' || in[2] != 'E' || in[3] != '1')
        return -1;
    uint32_t opos = 0;
    uint32_t i = 4;
    while (i < in_size && opos < out_max) {
        if (in[i] == 0x00 && i + 2 < in_size) {
            unsigned char run_len = in[i + 1];
            unsigned char byte_val = in[i + 2];
            for (int k = 0; k < run_len && opos < out_max; k++)
                out[opos++] = byte_val;
            i += 3;
        } else {
            out[opos++] = in[i++];
        }
    }
    *out_size = opos;
    return 0;
}

void cmd_zcat(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: zcat <file.gz>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char buf[8192];
    uint32_t size = 0;
    if (libc_vfs_read(path, buf, sizeof(buf), &size) != 0) {
        kprintf("zcat: %s: not found\n", path);
        return;
    }

    static unsigned char out[4096];
    uint32_t opos = 0;

    if (buf[0] == 0x1f && buf[1] == 0x8b && buf[2] == 8) {
        if (gzip_decompress(buf, size, out, sizeof(out), &opos) != 0) {
            kprintf("zcat: decompression failed\n");
            return;
        }
    } else if (buf[0] == 'R' && buf[1] == 'L' && buf[2] == 'E' && buf[3] == '1') {
        if (rle1_decompress(buf, size, out, sizeof(out), &opos) != 0) {
            kprintf("zcat: decompression failed\n");
            return;
        }
    } else {
        kprintf("zcat: unknown format\n");
        return;
    }

    /* Write decompressed data to stdout */
    libc_vfs_write("/dev/stdout", out, opos);
}
