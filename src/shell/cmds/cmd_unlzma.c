/* cmd_unlzma.c — decompress .lzma files */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

struct lzma_rc {
    uint32_t code;
    uint32_t range;
    const unsigned char *buf;
    uint32_t pos;
    uint32_t len;
};

static void rc_init(struct lzma_rc *rc, const unsigned char *buf, uint32_t len) {
    rc->buf = buf;
    rc->len = len;
    rc->pos = 0;
    rc->range = 0xFFFFFFFF;
    rc->code = 0;
    for (int i = 0; i < 5 && rc->pos < rc->len; i++)
        rc->code = (rc->code << 8) | rc->buf[rc->pos++];
}

static int rc_finished(struct lzma_rc *rc) {
    return rc->code == 0 && rc->pos >= rc->len;
}

static void rc_norm(struct lzma_rc *rc) {
    while (rc->range < 0x1000000 && rc->pos < rc->len) {
        rc->code = (rc->code << 8) | rc->buf[rc->pos++];
        rc->range <<= 8;
    }
}

#define LZMA_BUF 8192
#define LZMA_DICT 4096

void cmd_unlzma(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: unlzma <file.lzma>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char inbuf[8192];
    uint32_t in_size = 0;
    if (libc_vfs_read(path, inbuf, sizeof(inbuf), &in_size) != 0) {
        kprintf("unlzma: %s: not found\n", path);
        return;
    }

    if (in_size < 13) { kprintf("unlzma: file too small\n"); return; }

    static unsigned char outbuf[LZMA_BUF];
    uint32_t opos = 0;

    struct lzma_rc rc;
    rc_init(&rc, inbuf + 13, in_size > 13 ? in_size - 13 : 0);

    static unsigned char dict[LZMA_DICT];
    uint32_t dict_pos = 0;

    while (!rc_finished(&rc) && opos < LZMA_BUF) {
        rc.range >>= 1;
        uint32_t bit = (rc.code >= rc.range) ? 1 : 0;
        if (bit) rc.code -= rc.range;

        if (bit == 0) {
            unsigned char byte = 0;
            for (int i = 0; i < 8; i++) {
                rc_norm(&rc);
                rc.range >>= 1;
                uint32_t b = (rc.code >= rc.range) ? 1 : 0;
                if (b) rc.code -= rc.range;
                byte = (unsigned char)((byte << 1) | b);
            }
            outbuf[opos++] = byte;
            dict[dict_pos++ % LZMA_DICT] = byte;
        } else {
            uint32_t match_len = 0;
            uint32_t match_dist = 0;
            for (int i = 0; i < 8 && !rc_finished(&rc); i++) {
                rc_norm(&rc);
                rc.range >>= 1;
                uint32_t b = (rc.code >= rc.range) ? 1 : 0;
                if (b) rc.code -= rc.range;
                match_len = (match_len << 1) | b;
            }
            for (int i = 0; i < 12 && !rc_finished(&rc); i++) {
                rc_norm(&rc);
                rc.range >>= 1;
                uint32_t b = (rc.code >= rc.range) ? 1 : 0;
                if (b) rc.code -= rc.range;
                match_dist = (match_dist << 1) | b;
            }
            if (match_dist == 0) match_dist = 1;
            match_len += 2;

            for (uint32_t k = 0; k < match_len && opos < LZMA_BUF; k++) {
                unsigned char byte = dict[(dict_pos - match_dist + k) % LZMA_DICT];
                outbuf[opos++] = byte;
                dict[dict_pos++ % LZMA_DICT] = byte;
            }
        }
        rc_norm(&rc);
    }

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
    kprintf("Decompressed %u -> %u bytes (%s)\n", (uint64_t)in_size, (uint64_t)opos, outpath);
}
