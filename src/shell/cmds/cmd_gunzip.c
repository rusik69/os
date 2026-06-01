/* cmd_gunzip.c — decompress gzip files */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static int gzip_decompress(const unsigned char *in, uint32_t in_size,
                           unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    /* Expect gzip header: 1f 8b 08 */
    if (in_size < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return -1; /* not gzip */

    uint32_t pos = 10; /* skip header */

    /* Skip optional headers (FEXTRA, FNAME, FCOMMENT, FHCRC) */
    uint8_t flg = in[3];
    if (flg & 4) { /* FEXTRA */
        if (pos + 2 > in_size) return -1;
        uint16_t xlen = (uint16_t)(in[pos] | (in[pos+1] << 8));
        pos += 2 + xlen;
    }
    if (flg & 8) { /* FNAME */
        while (pos < in_size && in[pos]) pos++;
        pos++; /* skip NUL */
    }
    if (flg & 16) { /* FCOMMENT */
        while (pos < in_size && in[pos]) pos++;
        pos++;
    }
    if (flg & 2) { /* FHCRC */
        pos += 2;
    }

    if (pos >= in_size) return -1;

    /* We only handle stored blocks (BTYPE=0) and our simple RLE */
    /* Try RLE decompression (our custom format between gzip header and trailer) */
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

    /* Skip trailing DEFLATE block marker and CRC/ISIZE if present */
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

void cmd_gunzip(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: gunzip <file.gz>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char buf[8192];
    uint32_t size = 0;
    if (libc_vfs_read(path, buf, sizeof(buf), &size) != 0) {
        kprintf("gunzip: %s: not found\n", path);
        return;
    }

    static unsigned char out[4096];
    uint32_t opos = 0;

    if (buf[0] == 0x1f && buf[1] == 0x8b && buf[2] == 8) {
        if (gzip_decompress(buf, size, out, sizeof(out), &opos) != 0) {
            kprintf("gunzip: decompression failed\n");
            return;
        }
    } else if (buf[0] == 'R' && buf[1] == 'L' && buf[2] == 'E' && buf[3] == '1') {
        if (rle1_decompress(buf, size, out, sizeof(out), &opos) != 0) {
            kprintf("gunzip: decompression failed\n");
            return;
        }
    } else {
        kprintf("gunzip: unknown format\n");
        return;
    }

    char outpath[128];
    strncpy(outpath, path, 120);
    int plen = strlen(outpath);
    if (plen > 3 && outpath[plen-3] == '.' && outpath[plen-2] == 'g' && outpath[plen-1] == 'z')
        outpath[plen-3] = '\0';

    libc_vfs_write(outpath, out, opos);
    kprintf("Decompressed %u -> %u bytes (%s)\n", (uint64_t)size, (uint64_t)opos, outpath);
}
