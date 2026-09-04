/* gui_image.c — BMP/PNG decoders + DEFLATE inflate for the GUI Image Viewer.
 *
 * Self-contained, GUI-agnostic byte->pixel decoders.  No stdio, no heap: all
 * scratch lives in file-static buffers (the GUI is freestanding).  Compiles
 * under the host toolchain too, which is how the decoder logic is verified
 * (the decoders are exercised against real BMP/PNG files by a host harness).
 *
 * DEFLATE decoder is a clean-room implementation of the classic puff
 * algorithm (public domain, Mark Adler).  Only the tri-states for this
 * consumer's inputs are handled and every mismatched input bails out with a
 * failure instead of corrupting output.
 */
#include "gui_image.h"
#include <stdio.h>
#include <string.h>

/* ---- Scratch / capability limits ---------------------------------------- */
#define IVFILE_CAP 262144U   /* max raw file accepted */
#define IVIDAT_CAP 262144U   /* max concatenated PNG IDAT */
#define IVSRC_W 480U         /* max source width  */
#define IVSRC_H 320U         /* max source height */
#define IVRAW_CAP ((IVSRC_W * 4U + 1U) * IVSRC_H) /* max inflated scanlines */
#define IVPIX_MAX (IVSRC_W * IVSRC_H)             /* max RGBA source pixels */

static unsigned char s_iv_idat[IVIDAT_CAP];
static unsigned char s_iv_raw[IVRAW_CAP];
static uint32_t s_iv_pix[IVPIX_MAX];
static unsigned char s_iv_prev[IVSRC_W * 4U]; /* previous reconstructed row */
static unsigned char s_iv_cur[IVSRC_W * 4U];  /* current reconstructed row  */

/* ---- DEFLATE (puff-style) ----------------------------------------------- */
typedef struct {
    const unsigned char *in;
    unsigned long inlen, next;
    unsigned long bitbuf, bitcnt;
    int error;
} iv_zin_t;

typedef struct {
    short count[16];    /* number of codes of each length (0..15) */
    short symbol[288];  /* symbol table */
} iv_huff_t;

static int iv_huff_build(iv_huff_t *h, const short *length, int n) {
    int offs[16], left, len, i;
    if (n > 288)
        n = 288;
    h->count[0] = 0;
    for (len = 1; len <= 15; len++)
        h->count[len] = 0;
    for (i = 0; i < n; i++)
        h->count[length[i]]++;
    if (h->count[0] == n) /* no codes assigned */
        return 0;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1; /* over-subscribed */
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (i = 0; i < n; i++)
        if (length[i] != 0)
            h->symbol[offs[length[i]]++] = (short)i;
    return 0;
}

static int iv_zin_bits(iv_zin_t *z, int need, unsigned long *val) {
    if (z->error)
        return -1;
    while (z->bitcnt < (unsigned long)need) {
        if (z->next >= z->inlen) {
            z->error = 1;
            return -1;
        }
        z->bitbuf |= (unsigned long)z->in[z->next++] << z->bitcnt;
        z->bitcnt += 8;
    }
    *val = z->bitbuf & ((need == 64) ? ~0UL : ((1UL << need) - 1));
    z->bitbuf >>= need;
    z->bitcnt -= (unsigned long)need;
    return 0;
}

static int iv_decode(iv_zin_t *z, const iv_huff_t *h) {
    int len, code, first, count, index;
    unsigned long b;
    code = first = index = 0;
    for (len = 1; len <= 15; len++) {
        if (iv_zin_bits(z, 1, &b))
            return -1;
        code |= (int)b;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->error = 1; /* ran out of codes */
    return -1;
}

static const short iv_lbase[30] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
                                    35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short iv_lext[30] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,
                                   4,4,4,4,5,5,5,5,0 };
static const short iv_dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
                                    257,385,513,769,1025,1537,2049,3073,4097,6145,
                                    8193,12289,16385,24577 };
static const short iv_dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
                                   9,9,10,10,11,11,12,12,13,13 };

static int iv_stored(iv_zin_t *z, unsigned char *out, unsigned long outmax,
                     unsigned long *outlen) {
    unsigned long val, len, nlen, skip;
    /* align to byte boundary by consuming the residual (0..3) bits */
    skip = z->bitcnt & 7;
    while (skip) {
        if (iv_zin_bits(z, 1, &val))
            return -1;
        skip--;
    }
    len = 0;
    if (iv_zin_bits(z, 8, &val))
        return -1;
    len |= val;
    if (iv_zin_bits(z, 8, &val))
        return -1;
    len |= val << 8;
    nlen = 0;
    if (iv_zin_bits(z, 8, &val))
        return -1;
    nlen |= val;
    if (iv_zin_bits(z, 8, &val))
        return -1;
    nlen |= val << 8;
    if (nlen != (~len & 0xFFFFu) || len > outmax - *outlen)
        return -1;
    if (z->next + len > z->inlen)
        return -1;
    {
        unsigned long i;
        for (i = 0; i < len; i++)
            out[(*outlen)++] = z->in[z->next + i];
        z->next += len;
    }
    return 0;
}

static int iv_dynamic(iv_zin_t *z, iv_huff_t *ld, iv_huff_t *dd) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short codelens[19], lengths[320];
    iv_huff_t cl;
    unsigned long v;
    int hlit, hdist, hclen, sym = 0, i;
    if (iv_zin_bits(z, 5, &v))
        return -1;
    hlit = (int)v + 257;
    if (iv_zin_bits(z, 5, &v))
        return -1;
    hdist = (int)v + 1;
    if (iv_zin_bits(z, 4, &v))
        return -1;
    hclen = (int)v + 4;
    for (i = 0; i < 19; i++)
        codelens[i] = 0;
    for (i = 0; i < hclen; i++) {
        if (iv_zin_bits(z, 3, &v))
            return -1;
        codelens[order[i]] = (short)v;
    }
    if (iv_huff_build(&cl, codelens, 19))
        return -1;
    while (sym < hlit + hdist) {
        int len = iv_decode(z, &cl);
        int rep;
        if (len < 0)
            return -1;
        if (len < 16) {
            lengths[sym++] = (short)len;
        } else if (len == 16) {
            if (sym == 0 || iv_zin_bits(z, 2, &v))
                return -1;
            rep = (int)v + 3;
            while (rep--) {
                lengths[sym] = lengths[sym - 1];
                sym++;
            }
        } else if (len == 17) {
            if (iv_zin_bits(z, 3, &v))
                return -1;
            rep = (int)v + 3;
            while (rep--) {
                lengths[sym++] = 0;
            }
        } else { /* 18 */
            if (iv_zin_bits(z, 7, &v))
                return -1;
            rep = (int)v + 11;
            while (rep--) {
                lengths[sym++] = 0;
            }
        }
        if (sym > hlit + hdist)
            return -1;
    }
    if (iv_huff_build(ld, lengths, hlit))
        return -1;
    if (iv_huff_build(dd, lengths + hlit, hdist))
        return -1;
    return 0;
}

static int iv_decode_loop(iv_zin_t *z, const iv_huff_t *ld, const iv_huff_t *dd,
                          unsigned char *out, unsigned long outmax,
                          unsigned long *outlen) {
    for (;;) {
        int sym = iv_decode(z, ld);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (*outlen >= outmax)
                return -1;
            out[(*outlen)++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256)
            return 0; /* end of block */
        {
            int li = sym - 257;
            unsigned long len, dist, v, i;
            if (li >= 30)
                return -1;
            len = (unsigned long)iv_lbase[li];
            if (iv_lext[li]) {
                if (iv_zin_bits(z, iv_lext[li], &v))
                    return -1;
                len += v;
            }
            {
                int ds = iv_decode(z, dd);
                if (ds < 0 || ds >= 30)
                    return -1;
                dist = (unsigned long)iv_dbase[ds];
                if (iv_dext[ds]) {
                    if (iv_zin_bits(z, iv_dext[ds], &v))
                        return -1;
                    dist += v;
                }
            }
            if (dist == 0 || dist > *outlen)
                return -1;
            for (i = 0; i < len; i++) {
                if (*outlen >= outmax)
                    return -1;
                out[*outlen] = out[*outlen - dist];
                (*outlen)++;
            }
        }
    }
}

/* Decompress a zlib-wrapped DEFLATE stream.  Returns output length, or 0 on
 * failure.  `in` may optionally carry just a raw DEFLATE stream (no 2-byte
 * zlib header) -- emit `raw` when the caller already knows it. */
static unsigned long iv_inflate(const unsigned char *in, unsigned long inlen,
                                unsigned char *out, unsigned long outmax,
                                int raw_stream) {
    iv_zin_t z;
    unsigned long v, outlen = 0;
    int last = 0;
    z.in = in;
    z.inlen = inlen;
    z.next = 0;
    z.bitbuf = 0;
    z.bitcnt = 0;
    z.error = 0;

    if (!raw_stream) {
        unsigned cmf, flg;
        if (inlen < 2)
            return 0;
        cmf = in[0];
        flg = in[1];
        if ((cmf & 0x0f) != 8) /* not DEFLATE */
            return 0;
        if (((cmf << 8) | flg) % 31) /* header check */
            return 0;
        z.next = 2;
        if (flg & 0x20) { /* preset dictionary */
            if (inlen < 2 + 4)
                return 0;
            z.next += 4;
        }
    }

    while (!last) {
        int type;
        if (iv_zin_bits(&z, 1, &v))
            return 0;
        last = (int)v;
        if (iv_zin_bits(&z, 2, &v))
            return 0;
        type = (int)v;
        if (type == 0) {
            if (iv_stored(&z, out, outmax, &outlen))
                return 0;
        } else {
            iv_huff_t ld, dd;
            if (type == 1) {
                short ll[288], dl[30];
                int i;
                for (i = 0; i < 288; i++)
                    ll[i] = (short)((i <= 143 || i >= 280) ? 8 : (i <= 255) ? 9 : 7);
                for (i = 0; i < 30; i++)
                    dl[i] = 5;
                if (iv_huff_build(&ld, ll, 288) || iv_huff_build(&dd, dl, 30))
                    return 0;
            } else if (type == 2) {
                if (iv_dynamic(&z, &ld, &dd))
                    return 0;
            } else {
                return 0; /* reserved block type */
            }
            if (iv_decode_loop(&z, &ld, &dd, out, outmax, &outlen))
                return 0;
        }
    }
    return outlen;
}

/* ---- shared helpers ----------------------------------------------------- */
static uint32_t iv_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t iv_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int iv_scaledown_fit(uint32_t *fb, uint32_t fbw, uint32_t fbh,
                            const uint32_t *src, uint32_t sw, uint32_t sh) {
    float sx, sy, s, dw, dh, ox, oy;
    uint32_t y, x;
    if (sw == 0 || sh == 0)
        return -1;
    sx = (float)fbw / (float)sw;
    sy = (float)fbh / (float)sh;
    s = (sx < sy) ? sx : sy;
    dw = (float)sw * s;
    dh = (float)sh * s;
    ox = ((float)fbw - dw) / 2.0f;
    oy = ((float)fbh - dh) / 2.0f;
    for (y = 0; y < fbh; y++) {
        uint32_t srcy = 0;
        if ((float)y >= oy && (float)y < oy + dh)
            srcy = (uint32_t)(((float)y - oy) * (float)sh / dh);
        for (x = 0; x < fbw; x++) {
            uint32_t srcx = 0;
            if ((float)x >= ox && (float)x < ox + dw)
                srcx = (uint32_t)(((float)x - ox) * (float)sw / dw);
            if ((float)x >= ox && (float)x < ox + dw && (float)y >= oy && (float)y < oy + dh)
                fb[y * fbw + x] = src[srcy * sw + srcx];
            else
                fb[y * fbw + x] = 0xFFDCDCDC; /* light gray background */
        }
    }
    return 0;
}

/* ---- BMP ---------------------------------------------------------------- */
static int iv_bmp(const unsigned char *d, unsigned long len,
                  uint32_t *sw, uint32_t *sh, uint32_t *bpp) {
    int32_t w, h;
    unsigned comp, pixoff, rowsize, bytespp, x, y;
    if (len < 54 || d[0] != 'B' || d[1] != 'M')
        return -1;
    comp = iv_le32(d + 30);
    if (comp != 0) /* only BI_RGB */
        return -1;
    pixoff = iv_le32(d + 10);
    w = (int32_t)iv_le32(d + 18);
    h = (int32_t)iv_le32(d + 22);
    *bpp = iv_le32(d + 28);
    if (w <= 0 || h == 0)
        return -1;
    if ((uint32_t)w > IVSRC_W || (uint32_t)(h < 0 ? -h : h) > IVSRC_H)
        return -1;
    if (*bpp != 24 && *bpp != 32)
        return -1;
    bytespp = *bpp / 8;
    rowsize = ((uint32_t)w * *bpp + 3) & ~3u;
    if (pixoff + (uint32_t)(h < 0 ? -h : h - 1) * rowsize + (uint32_t)w * bytespp > len)
        return -1;
    {
        uint32_t abs_h = (uint32_t)(h < 0 ? -h : h);
        int topdown = (h < 0);
        for (y = 0; y < abs_h; y++) {
            uint32_t srcrow = topdown ? y : (abs_h - 1 - y);
            unsigned long base = (unsigned long)pixoff + (unsigned long)srcrow * rowsize;
            for (x = 0; x < (uint32_t)w; x++) {
                unsigned long o = base + (unsigned long)x * bytespp;
                uint32_t r = d[o + 2], g = d[o + 1], bl = d[o + 0];
                s_iv_pix[y * (uint32_t)w + x] = 0xFF000000u | (r << 16) | (g << 8) | bl;
            }
        }
    }
    *sw = (uint32_t)w;
    *sh = (uint32_t)(h < 0 ? -h : h);
    return 0;
}

/* ---- PNG ---------------------------------------------------------------- */
static int iv_png(const unsigned char *d, unsigned long len,
                  uint32_t *sw, uint32_t *sh) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    unsigned long pos = 8;
    uint32_t w, h, bitdepth = 0, colortype = 0;
    unsigned char palette[256][3];
    unsigned char trns[256];
    int has_trns = 0;   /* number of tRNS alpha entries (0 = none) */
    int has_pal = 0;
    int idat_len = 0, rawlen;
    int bpp, linesz, stride, y;

    if (len < 8 || memcmp(d, sig, 8))
        return -1;
    if (len < 16 || memcmp(d + 12, "IHDR", 4))
        return -1;
    w = iv_be32(d + 16);
    h = iv_be32(d + 20);
    if (w == 0 || h == 0 || w > IVSRC_W || h > IVSRC_H)
        return -1;
    bitdepth = d[24];
    colortype = d[25];
    if (d[26] != 0 || d[27] != 0 || d[28] != 0) /* comp/filter/interlace */
        return -1;
    /* parse chunks */
    pos = 33; /* past IHDR data start (8 sig + IHDR name + 13 data) */
    while (pos + 8 <= len) {
        uint32_t clen = iv_be32(d + pos);
        const unsigned char *cname = d + pos + 4;
        const unsigned char *cdata = d + pos + 8;
        if (clen > len - (pos + 12))
            return -1;
        if (memcmp(cname, "PLTE", 4) == 0) {
            if (clen % 3 || clen / 3 > 256)
                return -1;
            for (uint32_t i = 0; i < clen / 3; i++) {
                palette[i][0] = cdata[i * 3 + 0];
                palette[i][1] = cdata[i * 3 + 1];
                palette[i][2] = cdata[i * 3 + 2];
            }
            has_pal = 1;
        } else if (memcmp(cname, "tRNS", 4) == 0) {
            uint32_t n = clen < 256 ? clen : 256;
            for (uint32_t i = 0; i < n; i++)
                trns[i] = cdata[i];
            has_trns = (int)n;
        } else if (memcmp(cname, "IDAT", 4) == 0) {
            if (clen > IVIDAT_CAP - (uint32_t)idat_len)
                return -1;
            memcpy(s_iv_idat + idat_len, cdata, clen);
            idat_len += (int)clen;
        } else if (memcmp(cname, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }
    if (idat_len == 0)
        return -1;

    /* channels */
    if (colortype == 2) {
        bpp = 3;
    } else if (colortype == 6) {
        bpp = 4;
    } else if (colortype == 0) {
        bpp = 1;
    } else if (colortype == 3) {
        bpp = 1;
        if (!has_pal)
            return -1;
    } else {
        return -1; /* gray+alpha (4) unsupported here */
    }
    if (bitdepth != 8)
        return -1;

    stride = (int)w * bpp;
    linesz = stride + 1;
    rawlen = iv_inflate(s_iv_idat, (unsigned long)idat_len, s_iv_raw, IVRAW_CAP, 0);
    if (rawlen == 0 || rawlen != linesz * (int)h)
        return -1;

    memset(s_iv_prev, 0, (unsigned long)stride);
    for (y = 0; y < (int)h; y++) {
        const unsigned char *row = s_iv_raw + (unsigned long)y * linesz + 1;
        int filt = s_iv_raw[(unsigned long)y * linesz];
        int x;
        if (filt < 0 || filt > 4)
            return -1;
        for (x = 0; x < stride; x++) {
            int pred;
            unsigned char a = (x >= bpp) ? s_iv_cur[x - bpp] : 0;
            unsigned char b = s_iv_prev[x];
            unsigned char cc = (x >= bpp) ? s_iv_prev[x - bpp] : 0;
            int raw = row[x];
            switch (filt) {
            case 0: pred = raw; break;
            case 1: pred = raw + a; break;
            case 2: pred = raw + b; break;
            case 3: pred = raw + ((a + b) >> 1); break;
            default: {
                int p = a + b - cc, pa = p - a, pb = p - b, pc = p - cc;
                int pa2 = pa < 0 ? -pa : pa, pb2 = pb < 0 ? -pb : pb, pc2 = pc < 0 ? -pc : pc;
                pred = (pa2 <= pb2 && pa2 <= pc2) ? a : (pb2 <= pc2) ? b : cc;
                pred += raw;
            } break;
            }
            s_iv_cur[x] = (unsigned char)(pred & 0xFF);
        }
        /* convert row to RGBA */
        for (x = 0; x < (int)w; x++) {
            uint32_t r, g, b, al = 0xFF;
            if (colortype == 2) {
                r = s_iv_cur[x * 3 + 0]; g = s_iv_cur[x * 3 + 1]; b = s_iv_cur[x * 3 + 2];
            } else if (colortype == 6) {
                r = s_iv_cur[x * 4 + 0]; g = s_iv_cur[x * 4 + 1]; b = s_iv_cur[x * 4 + 2];
                al = s_iv_cur[x * 4 + 3];
            } else if (colortype == 0) {
                r = g = b = s_iv_cur[x];
            } else { /* colortype 3 */
                uint32_t idx = s_iv_cur[x];
                if (idx >= 256)
                    return -1;
                r = palette[idx][0]; g = palette[idx][1]; b = palette[idx][2];
                if (has_trns && idx < (uint32_t)has_trns)
                    al = trns[idx];
            }
            s_iv_pix[(unsigned long)y * w + x] = (al << 24) | (r << 16) | (g << 8) | b;
        }
        memcpy(s_iv_prev, s_iv_cur, (unsigned long)stride);
    }
    *sw = w;
    *sh = h;
    return 0;
}

/* ---- public API --------------------------------------------------------- */

int gui_img_decode(const unsigned char *data, unsigned long len,
                   uint32_t *fb, uint32_t fbw, uint32_t fbh,
                   char *status, unsigned long status_sz) {
    uint32_t sw, sh, bpp;
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
        if (iv_bmp(data, len, &sw, &sh, &bpp) == 0) {
            iv_scaledown_fit(fb, fbw, fbh, s_iv_pix, sw, sh);
            snprintf(status, status_sz, "BMP %ux%u %ubpp", sw, sh, bpp);
            return 1;
        }
        snprintf(status, status_sz, "BMP unsupported");
        return 0;
    }
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        if (iv_png(data, len, &sw, &sh) == 0) {
            iv_scaledown_fit(fb, fbw, fbh, s_iv_pix, sw, sh);
            snprintf(status, status_sz, "PNG %ux%u", sw, sh);
            return 1;
        }
        snprintf(status, status_sz, "PNG unsupported");
        return 0;
    }
    snprintf(status, status_sz, "Not an image");
    return 0;
}

void gui_img_demo(uint32_t *fb, uint32_t fbw, uint32_t fbh) {
    uint32_t y, x;
    for (y = 0; y < fbh; y++) {
        for (x = 0; x < fbw; x++) {
            uint32_t r, g, b;
            /* hue sweep left->right, brightness bands top->bottom */
            r = (x * 255) / (fbw ? fbw : 1);
            g = (y * 255) / (fbh ? fbh : 1);
            b = 255 - ((x + y) / 2 * 255) / ((fbw + fbh) ? fbw + fbh : 1);
            /* grid lines */
            if ((x < 2) || (y < 2) || (x >= fbw - 2) || (y >= fbh - 2) ||
                (x + 16) % 48 < 1 || (y + 16) % 48 < 1) {
                r = 32; g = 32; b = 64;
            }
            fb[y * fbw + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}