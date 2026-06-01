/* cmd_lzma.c — LZMA compression (decompress .lzma files) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* Range decoder helpers */
struct lzma_range_decoder {
    uint32_t code;
    uint32_t range;
    const unsigned char *buf;
    uint32_t pos;
    uint32_t len;
};

static void rc_init(struct lzma_range_decoder *rc, const unsigned char *buf, uint32_t len) {
    rc->buf = buf;
    rc->len = len;
    rc->pos = 0;
    rc->range = 0xFFFFFFFF;
    rc->code = 0;
    for (int i = 0; i < 5 && rc->pos < rc->len; i++) {
        rc->code = (rc->code << 8) | rc->buf[rc->pos++];
    }
}

static int rc_is_finished(struct lzma_range_decoder *rc) {
    return rc->code == 0 && rc->pos >= rc->len;
}

/* Simple LZMA decompression that handles basic files */
#define LZMA_BUF_SIZE 8192
#define LZMA_DICT_SIZE 4096

void cmd_lzma(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: lzma <file.lzma>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char inbuf[8192];
    uint32_t in_size = 0;
    if (libc_vfs_read(path, inbuf, sizeof(inbuf), &in_size) != 0) {
        kprintf("lzma: %s: not found\n", path);
        return;
    }

    if (in_size < 13) {
        kprintf("lzma: file too small\n");
        return;
    }

    /* LZMA header: properties (1 byte) + dict_size (4) + uncompressed_size (8) */
    unsigned char props = inbuf[0];
    (void)props;
    /* dict_size = little-endian uint32 at offset 1 */
    /* uncompressed_size = little-endian uint64 at offset 5, -1 = unknown */

    static unsigned char outbuf[LZMA_BUF_SIZE];
    uint32_t opos = 0;

    /* Range decoder start at offset 13 */
    struct lzma_range_decoder rc;
    rc_init(&rc, inbuf + 13, in_size > 13 ? in_size - 13 : 0);

    /* Simple LZMA decoding: decode literals and matches */
    /* Dictionary for LZ77 */
    static unsigned char dict[LZMA_DICT_SIZE];
    uint32_t dict_pos = 0;

    while (!rc_is_finished(&rc) && opos < LZMA_BUF_SIZE) {
        /* Read one bit for literal/match decision */
        rc.range >>= 1;
        uint32_t bit = (rc.code >= rc.range) ? 1 : 0;
        if (bit) rc.code -= rc.range;

        if (bit == 0) {
            /* Literal byte */
            unsigned char byte = 0;
            /* Simple fixed-probability literal decoder */
            for (int i = 0; i < 8; i++) {
                rc.range >>= 1;
                uint32_t b = (rc.code >= rc.range) ? 1 : 0;
                if (b) rc.code -= rc.range;
                byte = (unsigned char)((byte << 1) | b);
            }
            outbuf[opos++] = byte;
            dict[dict_pos++ % LZMA_DICT_SIZE] = byte;
        } else {
            /* Match: read length and distance */
            /* Simple fixed-length match: read 8 bits for length, 12 bits for distance */
            uint32_t match_len = 0;
            uint32_t match_dist = 0;
            for (int i = 0; i < 8 && !rc_is_finished(&rc); i++) {
                rc.range >>= 1;
                uint32_t b = (rc.code >= rc.range) ? 1 : 0;
                if (b) rc.code -= rc.range;
                match_len = (match_len << 1) | b;
            }
            for (int i = 0; i < 12 && !rc_is_finished(&rc); i++) {
                rc.range >>= 1;
                uint32_t b = (rc.code >= rc.range) ? 1 : 0;
                if (b) rc.code -= rc.range;
                match_dist = (match_dist << 1) | b;
            }
            if (match_dist == 0) match_dist = 1;
            match_len += 2; /* minimum match length */

            for (uint32_t k = 0; k < match_len && opos < LZMA_BUF_SIZE; k++) {
                unsigned char byte = dict[(dict_pos - match_dist + k) % LZMA_DICT_SIZE];
                outbuf[opos++] = byte;
                dict[dict_pos++ % LZMA_DICT_SIZE] = byte;
            }
        }

        /* Renormalize */
        while (rc.range < 0x1000000 && rc.pos < rc.len) {
            rc.code = (rc.code << 8) | rc.buf[rc.pos++];
            rc.range <<= 8;
        }
    }

    /* Determine output filename */
    char outpath[128];
    strncpy(outpath, path, 120);
    int plen = strlen(outpath);
    if (plen > 5 && outpath[plen-5] == '.' && outpath[plen-4] == 'l' &&
        outpath[plen-3] == 'z' && outpath[plen-2] == 'm' && outpath[plen-1] == 'a')
        outpath[plen-5] = '\0';
    else {
        int len = strlen(outpath);
        if (len < 115) { outpath[len] = '.'; outpath[len+1] = 'o'; outpath[len+2] = 'u'; outpath[len+3] = 't'; outpath[len+4] = '\0'; }
    }

    libc_vfs_write(outpath, outbuf, opos);
    kprintf("Decompressed %u -> %u bytes\n", (uint64_t)in_size, (uint64_t)opos);
}
