#define KERNEL_INTERNAL
#include "types.h"
#include "compress.h"
#include "string.h"
#include "errno.h"

/*
 * ── LZSS Compression ────────────────────────────────────────────────
 *
 * Hash-chain based match finder. For each byte position, we compute a
 * hash of the next 3 bytes, look up in the hash table to find the most
 * recent occurrence, then walk the chain to find the longest match.
 */

/* ── Internal helpers ─────────────────────────────────────────────── */

/*
 * Hash 3 bytes into a 12-bit value for hash-table lookup.
 */
static inline uint32_t lzss_hash(const uint8_t *data)
{
    return ((uint32_t)data[0] << 4) ^
           ((uint32_t)data[1] << 2) ^
            (uint32_t)data[2];
}

/*
 * Write a 2-byte reference (offset, length).
 * offset: 1..4095  (12 bits, 0 means "no match")
 * match_len: 3..18 (4 bits, stored as match_len - 3)
 * Reference format: [offset_hi | length_lo][offset_lo]
 *   byte0: bits 15-8 = (offset & 0xFF00) >> 8 | (match_len - 3)
 *   byte1: bits 7-0  = offset & 0xFF
 * So the 12-bit offset straddles the boundary: high 8 bits of offset go
 * into byte0's high bits, low 8 bits go into byte1.  The length nibble
 * goes into byte0's low 4 bits.
 */
static inline void put_ref(uint8_t *dst, uint32_t offset, int match_len)
{
    uint32_t len_code = (uint32_t)(match_len - LZSS_MIN_MATCH);
    dst[0] = (uint8_t)(((offset >> 4) & 0xF0) | len_code);
    dst[1] = (uint8_t)(offset & 0xFF);
}

/*
 * Decode a reference, returning offset and match_len via pointers.
 */
static inline void get_ref(const uint8_t *src, uint32_t *offset, int *match_len)
{
    *offset = (((uint32_t)src[0] & 0xF0) << 4) | (uint32_t)src[1];
    *match_len = (src[0] & 0x0F) + LZSS_MIN_MATCH;
}

/* ── Compression ──────────────────────────────────────────────────── */

int lzss_compress(const uint8_t *input, int input_len,
                  uint8_t *output, int output_len)
{
    if (input_len <= 0 || input_len > LZSS_MAX_INPUT)
        return -EINVAL;
    if (!input || !output)
        return -EINVAL;

    /* Hash table: maps hash -> position in input (-1 = empty) */
    int hash_head[LZSS_HASH_SIZE];
    int hash_next[LZSS_MAX_INPUT]; /* collision chain */
    for (int i = 0; i < LZSS_HASH_SIZE; i++)
        hash_head[i] = -1;

    int in_pos  = 0;  /* current position in input */
    int out_pos = 0;  /* current position in output */

    while (in_pos < input_len) {
        uint8_t control = 0;
        int control_pos = out_pos;

        /* Reserve space for control byte */
        if (out_pos >= output_len)
            return -ENOMEM;
        out_pos++;
        if (out_pos > output_len)
            return -ENOMEM;

        /* Process up to 8 operations per control byte */
        for (int op = 0; op < 8 && in_pos < input_len; op++) {
            int best_len  = 0;
            int best_off  = 0;

            /*
             * Try to find a match (only if we have at least LZSS_MIN_MATCH
             * bytes remaining and we can build a hash).
             */
            if (input_len - in_pos >= LZSS_MIN_MATCH) {
                uint32_t h = lzss_hash(&input[in_pos]);
                int max_match = input_len - in_pos;
                if (max_match > LZSS_MAX_MATCH)
                    max_match = LZSS_MAX_MATCH;

                /* Walk the hash chain looking for the longest match */
                int probe = hash_head[h];
                while (probe >= 0 && (in_pos - probe) <= LZSS_WINDOW_SIZE) {
                    /* Check match length */
                    int len = 0;
                    while (len < max_match &&
                           input[probe + len] == input[in_pos + len]) {
                        len++;
                    }
                    if (len > best_len) {
                        best_len = len;
                        best_off = in_pos - probe;
                        if (len == max_match)
                            break; /* maximum possible match */
                    }
                    probe = hash_next[probe];
                }

                /* Update hash chain with current position */
                hash_next[in_pos] = hash_head[h];
                hash_head[h] = in_pos;
            }

            if (best_len >= LZSS_MIN_MATCH) {
                /* Emit a reference */
                control |= (uint8_t)(1U << op);

                /* Ensure 2 bytes for the reference */
                if (out_pos + 2 > output_len)
                    return -ENOMEM;

                put_ref(&output[out_pos], (uint32_t)best_off, best_len);
                out_pos += 2;
                in_pos  += best_len;
            } else {
                /* Emit a literal */
                /* Update hash chain for this position too */
                if (input_len - in_pos >= LZSS_MIN_MATCH) {
                    uint32_t h = lzss_hash(&input[in_pos]);
                    hash_next[in_pos] = hash_head[h];
                    hash_head[h] = in_pos;
                }

                if (out_pos >= output_len)
                    return -ENOMEM;

                output[out_pos++] = input[in_pos++];
            }
        }

        /* Go back and write the control byte */
        output[control_pos] = control;
    }

    return out_pos;
}

/* ── Decompression ────────────────────────────────────────────────── */

int lzss_decompress(const uint8_t *input, int input_len,
                    uint8_t *output, int output_len)
{
    if (!input || !output)
        return -EINVAL;
    if (input_len <= 0)
        return -EINVAL;

    int in_pos  = 0;
    int out_pos = 0;

    while (in_pos < input_len) {
        uint8_t control = input[in_pos++];

        for (int op = 0; op < 8; op++) {
            if (control & (1U << op)) {
                /* Reference: 2 bytes */
                if (in_pos + 2 > input_len)
                    return -EINVAL;

                uint32_t offset;
                int match_len = 0;
                get_ref(&input[in_pos], &offset, &match_len);
                in_pos += 2;

                if (offset == 0 || offset > (uint32_t)out_pos)
                    return -EINVAL;
                if (out_pos + match_len > output_len)
                    return -ENOSPC;

                /* Copy the match (may overlap — use memmove) */
                uint8_t *src = &output[out_pos - offset];
                for (int i = 0; i < match_len; i++)
                    output[out_pos++] = src[i];
            } else {
                /* Literal: 1 byte */
                if (in_pos >= input_len) {
                    /* End of input is OK if we've processed all ops */
                    return out_pos;
                }
                if (out_pos >= output_len)
                    return -ENOSPC;

                output[out_pos++] = input[in_pos++];
            }
        }
    }

    return out_pos;
}

/* ── Stub: compress_deflate ────────────────────────────────────────── */
static int compress_deflate(const uint8_t *input, int input_len,
                     uint8_t *output, int output_len)
{
    (void)input; (void)input_len; (void)output; (void)output_len;
    kprintf("[COMPRESS] compress_deflate: not yet implemented\n");
    return -EIO;
}

/* ── Stub: compress_inflate ────────────────────────────────────────── */
static int compress_inflate(const uint8_t *input, int input_len,
                     uint8_t *output, int output_len)
{
    (void)input; (void)input_len; (void)output; (void)output_len;
    kprintf("[COMPRESS] compress_inflate: not yet implemented\n");
    return -EIO;
}

/* ── Stub: compress_init ───────────────────────────────────────────── */
static int compress_init(void)
{
    kprintf("[COMPRESS] compress_init: not yet implemented\n");
    return 0;
}

/* ── Stub: compress_exit ───────────────────────────────────────────── */
static void compress_exit(void)
{
    kprintf("[COMPRESS] compress_exit: not yet implemented\n");
}

/* ── Stub: compress_lz4 ─────────────────────────────── */
static int compress_lz4(const void *src, size_t src_len, void *dst, size_t *dst_len)
{
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_len;
    kprintf("[compress] compress_lz4: not yet implemented\n");
    return -EIO;
}
/* ── Stub: compress_zlib ─────────────────────────────── */
static int compress_zlib(const void *src, size_t src_len, void *dst, size_t *dst_len)
{
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_len;
    kprintf("[compress] compress_zlib: not yet implemented\n");
    return -EIO;
}
