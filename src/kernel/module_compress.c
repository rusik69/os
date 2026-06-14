/*
 * module_compress.c — Module compression detection and decompression
 *
 * Detects gzip (.ko.gz) and xz (.ko.xz) compressed kernel modules and
 * decompresses them before passing to the ELF module loader.
 *
 * GZIP format: magic 1f 8b, DEFLATE (RFC 1951) inside a gzip wrapper (RFC 1952).
 * XZ format:  magic fd 37 7a 58 5a 00, LZMA2 inside an XZ container.
 *
 * For a hobby OS we provide a minimal DEFLATE inflator for gzip.
 * The xz format is detected but falls back to a stub (full LZMA2
 * decompression is deferred).
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "module_compress.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"

/* ── Magic constants ───────────────────────────────────────────────── */

#define GZIP_MAGIC_0     0x1f
#define GZIP_MAGIC_1     0x8b
#define GZIP_CM_DEFLATE  8

#define XZ_MAGIC_0       0xfd
#define XZ_MAGIC_1       0x37
#define XZ_MAGIC_2       0x7a
#define XZ_MAGIC_3       0x58
#define XZ_MAGIC_4       0x5a
#define XZ_MAGIC_5       0x00

/* ── Detection ─────────────────────────────────────────────────────── */

int module_is_gzip(const uint8_t *data, uint64_t size)
{
    if (!data || size < 2)
        return 0;
    return (data[0] == GZIP_MAGIC_0 && data[1] == GZIP_MAGIC_1);
}

int module_is_xz(const uint8_t *data, uint64_t size)
{
    if (!data || size < 6)
        return 0;
    return (data[0] == XZ_MAGIC_0 && data[1] == XZ_MAGIC_1 &&
            data[2] == XZ_MAGIC_2 && data[3] == XZ_MAGIC_3 &&
            data[4] == XZ_MAGIC_4 && data[5] == XZ_MAGIC_5);
}

int module_is_compressed(const uint8_t *data, uint64_t size,
                         enum module_compress_type *out_type)
{
    if (!out_type)
        return 0;

    if (module_is_gzip(data, size)) {
        *out_type = MODULE_COMPRESS_GZIP;
        return 1;
    }
    if (module_is_xz(data, size)) {
        *out_type = MODULE_COMPRESS_XZ;
        return 1;
    }
    *out_type = MODULE_COMPRESS_NONE;
    return 0;
}

/* ── GZIP header parsing ──────────────────────────────────────────────
 *
 * GZIP member format (RFC 1952):
 *   +---+---+---+---+---+---+---+---+---+---+
 *   |ID1|ID2|CM |FLG|     MTIME     |XFL|OS |
 *   +---+---+---+---+---+---+---+---+---+---+
 *   +---+
 *   |XLEN|  (if FLG.FEXTRA)
 *   +---+
 *   +----------+
 *   |  FNAME   |  (if FLG.FNAME, null-terminated)
 *   +----------+
 *   +----------+
 *   |  FCOMMENT|  (if FLG.FCOMMENT, null-terminated)
 *   +----------+
 *   +----+
 *   |FHCRC|  (if FLG.FHCRC, 2 bytes)
 *   +----+
 *   +=======================+
 *   |   compressed data     |  (DEFLATE)
 *   +=======================+
 *   +---+---+---+---+
 *   |   CRC32      |
 *   +---+---+---+---+
 *   +---+---+---+---+
 *   |   ISIZE      |
 *   +---+---+---+---+
 */

struct gzip_header {
    uint8_t  id1;
    uint8_t  id2;
    uint8_t  cm;
    uint8_t  flg;
    uint32_t mtime;
    uint8_t  xfl;
    uint8_t  os;
} __attribute__((packed));

#define GZIP_FLG_FTEXT     0x01
#define GZIP_FLG_FHCRC     0x02
#define GZIP_FLG_FEXTRA    0x04
#define GZIP_FLG_FNAME     0x08
#define GZIP_FLG_FCOMMENT  0x10

/* ── Minimal DEFLATE inflator ─────────────────────────────────────────
 *
 * Implements RFC 1951 decompression supporting:
 *   - Stored (no compression) blocks  (BTYPE=00)
 *   - Fixed Huffman codes             (BTYPE=01)
 *   - Dynamic Huffman codes           (BTYPE=10)
 *
 * This is a minimal implementation sufficient to decompress typical
 * gzip-compressed kernel modules.
 */

/* Bit reader state */
struct bit_reader {
    const uint8_t *data;
    uint64_t       size;
    uint64_t       pos;      /* byte position */
    uint32_t       bitbuf;   /* buffered bits */
    int            bits;     /* number of bits in buffer */
};

static inline int br_init(struct bit_reader *br, const uint8_t *data, uint64_t size)
{
    br->data = data;
    br->size = size;
    br->pos = 0;
    br->bitbuf = 0;
    br->bits = 0;
    return 0;
}

static inline int br_ensure_bits(struct bit_reader *br, int n)
{
    while (br->bits < n) {
        if (br->pos >= br->size)
            return -1;
        br->bitbuf |= (uint32_t)br->data[br->pos] << br->bits;
        br->pos++;
        br->bits += 8;
    }
    return 0;
}

static inline uint32_t br_get_bits(struct bit_reader *br, int n)
{
    if (br_ensure_bits(br, n) < 0)
        return 0xFFFFFFFF;
    uint32_t val = br->bitbuf & ((1U << n) - 1);
    br->bitbuf >>= n;
    br->bits -= n;
    return val;
}

static inline uint32_t br_peek_bits(struct bit_reader *br, int n)
{
    if (br_ensure_bits(br, n) < 0)
        return 0xFFFFFFFF;
    return br->bitbuf & ((1U << n) - 1);
}

static inline void br_drop_bits(struct bit_reader *br, int n)
{
    br->bitbuf >>= n;
    br->bits -= n;
}

/* ── Fixed Huffman code tables (RFC 1951 section 3.2.6) ───────────── */

/* For a simple inflator, we use a lookup-table-based Huffman decoder.
 * Maximum code length is 15 bits (RFC 1951). */

#define HUFF_MAX_BITS  15
#define HUFF_LUT_SIZE  (1 << HUFF_MAX_BITS)

struct huff_table {
    uint16_t lut[HUFF_LUT_SIZE]; /* direct lookup: 16k entries */
    int      max_bits;
};

/*
 * Build a Huffman decode table from a list of (symbol, code_length).
 * Uses the standard package-merge algorithm simplified for RFC 1951.
 * Returns 0 on success, -1 on error.
 */
struct huff_sym_len {
    uint16_t sym;
    uint8_t  len;
};

static int huff_build(struct huff_table *ht,
                       const struct huff_sym_len *syms, int num_syms)
{
    if (!ht || !syms)
        return -1;

    /* Count codes per length */
    int bl_count[HUFF_MAX_BITS + 1];
    memset(bl_count, 0, sizeof(bl_count));

    for (int i = 0; i < num_syms; i++) {
        if (syms[i].len > 0 && syms[i].len <= HUFF_MAX_BITS)
            bl_count[syms[i].len]++;
    }

    /* Compute starting code for each length (canonical Huffman) */
    uint16_t next_code[HUFF_MAX_BITS + 1];
    uint16_t code = 0;
    for (int bits = 1; bits <= HUFF_MAX_BITS; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    /* Build lookup table */
    ht->max_bits = HUFF_MAX_BITS;
    memset(ht->lut, 0xFF, sizeof(ht->lut)); /* 0xFFFF = invalid */

    for (int i = 0; i < num_syms; i++) {
        int len = syms[i].len;
        if (len == 0 || len > HUFF_MAX_BITS)
            continue;

        uint16_t c = next_code[len];
        next_code[len]++;

        /* Fill all entries with this prefix */
        int step = 1 << len;
        for (uint32_t j = 0; j < (uint32_t)(1U << (HUFF_MAX_BITS - len)); j++) {
            uint32_t idx = ((uint32_t)c << (HUFF_MAX_BITS - len)) | j;
            if (idx < HUFF_LUT_SIZE)
                ht->lut[idx] = syms[i].sym;
        }
    }

    return 0;
}

/* Decode one symbol using the lookup table */
static inline int huff_decode(struct huff_table *ht, struct bit_reader *br)
{
    if (br_ensure_bits(br, HUFF_MAX_BITS) < 0)
        return -1;

    uint32_t bits = br_peek_bits(br, HUFF_MAX_BITS);
    uint16_t sym = ht->lut[bits];
    if (sym == 0xFFFF)
        return -1; /* invalid code */

    /* Determine actual code length by checking length-limited decode.
     * We walk back to find the smallest bit length that matches. */
    int len = 1;
    while (len <= HUFF_MAX_BITS) {
        uint32_t prefix = bits & ((1U << len) - 1);
        uint32_t idx = (uint32_t)prefix << (HUFF_MAX_BITS - len);
        if (idx < HUFF_LUT_SIZE && ht->lut[idx] == sym)
            break;
        len++;
    }

    br_drop_bits(br, len);
    return (int)sym;
}

/* ── Fixed Huffman codes (RFC 1951 section 3.2.6) ──────────────────── */

static struct huff_table huff_fixed_lit;
static struct huff_table huff_fixed_dist;
static int huff_fixed_built = 0;

static int build_fixed_huff_tables(void)
{
    /* Literal/Length: 0-143 (8 bits), 144-255 (9 bits), 256-279 (7 bits), 280-287 (8 bits) */
    struct huff_sym_len lit_syms[288];
    for (int i = 0; i < 288; i++) {
        lit_syms[i].sym = (uint16_t)i;
        if (i <= 143)
            lit_syms[i].len = 8;
        else if (i <= 255)
            lit_syms[i].len = 9;
        else if (i <= 279)
            lit_syms[i].len = 7;
        else
            lit_syms[i].len = 8;
    }
    if (huff_build(&huff_fixed_lit, lit_syms, 288) < 0)
        return -1;

    /* Distance: 0-29 (5 bits each) */
    struct huff_sym_len dist_syms[30];
    for (int i = 0; i < 30; i++) {
        dist_syms[i].sym = (uint16_t)i;
        dist_syms[i].len = 5;
    }
    if (huff_build(&huff_fixed_dist, dist_syms, 30) < 0)
        return -1;

    huff_fixed_built = 1;
    return 0;
}

/* ── Length / distance extra bits tables (RFC 1951 section 3.2.5) ─── */

static const uint8_t length_extra_bits[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t length_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t dist_extra_bits[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const uint16_t dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};

/* ── Inflate a single stored (BTYPE=00) block ──────────────────────── */

static int inflate_stored(struct bit_reader *br,
                           uint8_t *out, uint64_t out_size,
                           uint64_t *out_pos)
{
    /* Skip to byte boundary */
    br->bits = 0;
    br->bitbuf = 0;

    /* Read LEN and NLEN (2 bytes each, little-endian) */
    if (br->pos + 4 > br->size)
        return -1;

    uint16_t len = (uint16_t)br->data[br->pos] |
                   ((uint16_t)br->data[br->pos + 1] << 8);
    uint16_t nlen = (uint16_t)br->data[br->pos + 2] |
                    ((uint16_t)br->data[br->pos + 3] << 8);
    br->pos += 4;

    if (len != (uint16_t)(~nlen))
        return -1; /* length check failed */

    if (*out_pos + len > out_size)
        return -1;

    if (br->pos + len > br->size)
        return -1;

    memcpy(out + *out_pos, br->data + br->pos, len);
    *out_pos += len;
    br->pos += len;

    return 0;
}

/* ── Inflate data with given literal and distance Huffman tables ────── */

static int inflate_huffman(struct bit_reader *br,
                            struct huff_table *lit_table,
                            struct huff_table *dist_table,
                            uint8_t *out, uint64_t out_size,
                            uint64_t *out_pos)
{
    for (;;) {
        int sym = huff_decode(lit_table, br);
        if (sym < 0)
            return -1;

        if (sym < 256) {
            /* Literal byte */
            if (*out_pos >= out_size)
                return -1;
            out[(*out_pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            /* End of block */
            break;
        } else {
            /* Length code (257..285) */
            int len_idx = sym - 257;
            if (len_idx >= 29)
                return -1;

            int extra_len = (int)length_extra_bits[len_idx];
            uint16_t match_len = length_base[len_idx];
            if (extra_len > 0)
                match_len += (uint16_t)br_get_bits(br, extra_len);

            /* Distance code */
            int dist_sym = huff_decode(dist_table, br);
            if (dist_sym < 0 || dist_sym >= 30)
                return -1;

            int extra_dist = (int)dist_extra_bits[dist_sym];
            uint16_t distance = dist_base[dist_sym];
            if (extra_dist > 0)
                distance += (uint16_t)br_get_bits(br, extra_dist);

            /* Copy match (may overlap) */
            if (*out_pos < distance || *out_pos + match_len > out_size)
                return -1;

            for (uint16_t i = 0; i < match_len; i++) {
                out[*out_pos] = out[*out_pos - distance];
                (*out_pos)++;
            }
        }
    }

    return 0;
}

/* ── Dynamic Huffman block header parsing (BTYPE=10) ───────────────── */

static int inflate_dynamic(struct bit_reader *br,
                            uint8_t *out, uint64_t out_size,
                            uint64_t *out_pos)
{
    int hlit  = (int)br_get_bits(br, 5) + 257;  /* 257..286 literal/length codes */
    int hdist = (int)br_get_bits(br, 5) + 1;    /* 1..32 distance codes */
    int hclen = (int)br_get_bits(br, 4) + 4;    /* 4..19 code length codes */

    /* Code length alphabet order */
    static const uint8_t cl_order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    /* Read code lengths for the code length Huffman table */
    uint8_t cl_lengths[19];
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (int i = 0; i < hclen; i++) {
        cl_lengths[cl_order[i]] = (uint8_t)br_get_bits(br, 3);
    }

    /* Build the code length Huffman table */
    struct huff_sym_len cl_syms[19];
    int cl_num = 0;
    for (int i = 0; i < 19; i++) {
        if (cl_lengths[i] > 0) {
            cl_syms[cl_num].sym = (uint16_t)i;
            cl_syms[cl_num].len = cl_lengths[i];
            cl_num++;
        }
    }

    struct huff_table cl_table;
    if (huff_build(&cl_table, cl_syms, cl_num) < 0)
        return -1;

    /* Read literal/length + distance code lengths */
    int total = hlit + hdist;
    uint8_t code_lengths[320]; /* max: 286 + 32 */
    int cl_idx = 0;

    while (cl_idx < total) {
        int sym = huff_decode(&cl_table, br);
        if (sym < 0)
            return -1;

        if (sym < 16) {
            code_lengths[cl_idx++] = (uint8_t)sym;
        } else if (sym == 16) {
            /* Repeat previous length 3..6 times */
            int repeat = (int)br_get_bits(br, 2) + 3;
            uint8_t prev = cl_idx > 0 ? code_lengths[cl_idx - 1] : 0;
            for (int j = 0; j < repeat && cl_idx < total; j++)
                code_lengths[cl_idx++] = prev;
        } else if (sym == 17) {
            /* Repeat zero 3..10 times */
            int repeat = (int)br_get_bits(br, 3) + 3;
            for (int j = 0; j < repeat && cl_idx < total; j++)
                code_lengths[cl_idx++] = 0;
        } else if (sym == 18) {
            /* Repeat zero 11..138 times */
            int repeat = (int)br_get_bits(br, 7) + 11;
            for (int j = 0; j < repeat && cl_idx < total; j++)
                code_lengths[cl_idx++] = 0;
        }
    }

    /* Build literal/length table */
    struct huff_sym_len lit_syms[288];
    int lit_num = 0;
    for (int i = 0; i < hlit && i < 286; i++) {
        if (code_lengths[i] > 0) {
            lit_syms[lit_num].sym = (uint16_t)i;
            lit_syms[lit_num].len = code_lengths[i];
            lit_num++;
        }
    }
    /* Ensure symbol 256 is present for end-of-block */
    if (lit_num == 0 || (lit_num > 0 && lit_syms[lit_num-1].sym != 256)) {
        /* Add end-of-block if missing */
        lit_syms[lit_num].sym = 256;
        lit_syms[lit_num].len = 7; /* reasonable default */
        lit_num++;
    }

    struct huff_table lit_table;
    if (huff_build(&lit_table, lit_syms, lit_num) < 0)
        return -1;

    /* Build distance table */
    struct huff_sym_len dist_syms[32];
    int dist_num = 0;
    for (int i = 0; i < hdist && i < 30; i++) {
        int idx = hlit + i;
        if (idx < total && code_lengths[idx] > 0) {
            dist_syms[dist_num].sym = (uint16_t)i;
            dist_syms[dist_num].len = code_lengths[idx];
            dist_num++;
        }
    }

    struct huff_table dist_table;
    if (dist_num > 0) {
        if (huff_build(&dist_table, dist_syms, dist_num) < 0)
            return -1;
    } else {
        /* No distance codes — all matches have distance 0? This shouldn't happen
         * in valid data, but handle gracefully by creating a dummy table. */
        dist_syms[0].sym = 0;
        dist_syms[0].len = 1;
        dist_num = 1;
        if (huff_build(&dist_table, dist_syms, dist_num) < 0)
            return -1;
    }

    /* Decompress the actual data */
    return inflate_huffman(br, &lit_table, &dist_table, out, out_size, out_pos);
}

/* ── Inflate one DEFLATE block ──────────────────────────────────────── */

static int inflate_block(struct bit_reader *br,
                          uint8_t *out, uint64_t out_size,
                          uint64_t *out_pos)
{
    int bfinal = (int)br_get_bits(br, 1);
    int btype = (int)br_get_bits(br, 2);

    switch (btype) {
    case 0: /* No compression */
        if (inflate_stored(br, out, out_size, out_pos) < 0)
            return -1;
        break;

    case 1: /* Fixed Huffman */
        if (!huff_fixed_built) {
            if (build_fixed_huff_tables() < 0)
                return -1;
        }
        if (inflate_huffman(br, &huff_fixed_lit, &huff_fixed_dist,
                             out, out_size, out_pos) < 0)
            return -1;
        break;

    case 2: /* Dynamic Huffman */
        if (inflate_dynamic(br, out, out_size, out_pos) < 0)
            return -1;
        break;

    default:
        return -1; /* Reserved / error */
    }

    return bfinal;
}

/* ── Full gzip decompression ────────────────────────────────────────── */

int gzip_inflate(const uint8_t *in, uint64_t in_size,
                 uint8_t *out, uint64_t out_size,
                 uint64_t *decompressed_size)
{
    if (!in || !out || !decompressed_size || in_size < 18)
        return -EINVAL;

    /* Parse gzip header */
    const struct gzip_header *hdr = (const struct gzip_header *)in;

    if (hdr->id1 != GZIP_MAGIC_0 || hdr->id2 != GZIP_MAGIC_1)
        return -EINVAL;
    if (hdr->cm != GZIP_CM_DEFLATE)
        return -EINVAL;

    uint64_t pos = sizeof(struct gzip_header); /* 10 bytes */

    /* Skip optional fields */
    if (hdr->flg & GZIP_FLG_FEXTRA) {
        if (pos + 2 > in_size) return -EINVAL;
        uint16_t xlen = (uint16_t)in[pos] | ((uint16_t)in[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (hdr->flg & GZIP_FLG_FNAME) {
        while (pos < in_size && in[pos] != 0) pos++;
        pos++; /* skip null terminator */
    }
    if (hdr->flg & GZIP_FLG_FCOMMENT) {
        while (pos < in_size && in[pos] != 0) pos++;
        pos++;
    }
    if (hdr->flg & GZIP_FLG_FHCRC) {
        pos += 2; /* skip CRC16 */
    }

    /* Decompress DEFLATE data */
    struct bit_reader br;
    br_init(&br, in + pos, in_size - pos);

    uint64_t out_pos = 0;
    int bfinal;

    do {
        bfinal = inflate_block(&br, out, out_size, &out_pos);
        if (bfinal < 0) {
            kprintf("[MOD_COMPRESS] gzip inflate error at output offset %llu\\n",
                    (unsigned long long)out_pos);
            return -EINVAL;
        }
    } while (!bfinal);

    /* Skip gzip trailer (CRC32 + ISIZE = 8 bytes) */
    *decompressed_size = out_pos;

    kprintf("[MOD_COMPRESS] gzip decompressed %llu -> %llu bytes\\n",
            (unsigned long long)in_size, (unsigned long long)out_pos);

    return 0;
}

/* ── Module ELF decompression wrapper ──────────────────────────────── */

int module_decompress(const uint8_t *input, uint64_t input_size,
                       uint8_t **output, uint64_t *output_size)
{
    if (!input || !output || !output_size)
        return -EINVAL;

    enum module_compress_type ctype;
    if (!module_is_compressed(input, input_size, &ctype)) {
        /* Not compressed — just reference the input directly */
        *output = (uint8_t *)input;
        *output_size = input_size;
        return 0;
    }

    switch (ctype) {
    case MODULE_COMPRESS_GZIP: {
        /* Estimate decompressed size: typically modules are 4x max compressed.
         * Read ISIZE from gzip trailer for exact size. */
        uint64_t estimate = input_size * 4;
        if (estimate < 4096) estimate = 4096;
        if (estimate > 8 * 1024 * 1024) estimate = 8 * 1024 * 1024;

        uint8_t *decomp_buf = (uint8_t *)kmalloc(estimate);
        if (!decomp_buf)
            return -ENOMEM;

        uint64_t decomp_size = 0;
        int ret = gzip_inflate(input, input_size, decomp_buf, estimate, &decomp_size);
        if (ret != 0) {
            kfree(decomp_buf);
            return ret;
        }

        *output = decomp_buf;
        *output_size = decomp_size;
        kprintf("[MOD_COMPRESS] Decompressed gzip module (%llu -> %llu bytes)\\n",
                (unsigned long long)input_size, (unsigned long long)decomp_size);
        return 1; /* 1 = was compressed, buffer is kmalloc'd */
    }

    case MODULE_COMPRESS_XZ:
        kprintf("[MOD_COMPRESS] XZ compression detected — not yet supported\n");
        return -EINVAL;

    default:
        return -EINVAL;
    }
}

void module_decompress_free(uint8_t *buf, int was_compressed)
{
    if (buf && was_compressed)
        kfree(buf);
}
