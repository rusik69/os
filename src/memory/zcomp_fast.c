/*
 * zcomp_fast.c — Fast LZ77 compression for ZRAM page compression
 *
 * Optimized for 4096-byte pages with hash-chain match finding.
 * Decompression is extremely fast (simple memcpy + back-reference).
 *
 * Format (per control-byte group of 8 operations):
 *   Control byte: bit i = 0 → literal (1 data byte follows)
 *                 bit i = 1 → reference (2 bytes follow encoding offset+length)
 *   Reference encoding:
 *     [0]: [offset_hi(8..4) | length_lo(3..0)]
 *     [1]: [offset_lo(7..0)]
 *     offset = ((byte[0] & 0xF0) << 4) | byte[1]   (12 bits, 1..4095)
 *     length = (byte[0] & 0x0F) + 3                  (4 bits, 3..18)
 *
 * Worst-case expansion: 1 control byte per 8 literals = 12.5% overhead,
 * plus the control byte itself.  For a 4K page, max output ~ 4608 bytes.
 */

#include "zcomp.h"
#include "types.h"
#include "string.h"
#include "errno.h"
#include "heap.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define FAST_HASH_BITS    10
#define FAST_HASH_SIZE    (1U << FAST_HASH_BITS)  /* 1024 entries */
#define FAST_MIN_MATCH    3
#define FAST_MAX_MATCH    18
#define FAST_CHAIN_MAX    4096  /* Max positions to chain (1 page) */

/* Per-CPU workspace for the fast compressor */
struct fast_workspace {
    int32_t hash_head[FAST_HASH_SIZE];
    int32_t hash_next[FAST_CHAIN_MAX];
};

/* ── Hash function (3 bytes → hash) ────────────────────────────────── */

static inline uint32_t fast_hash(const uint8_t *data)
{
    return ((uint32_t)data[0] << 4) ^
           ((uint32_t)data[1] << 2) ^
            (uint32_t)data[2];
}

/* ── Workspace management ──────────────────────────────────────────── */

static void *fast_create_workspace(void)
{
    struct fast_workspace *ws;

    ws = (struct fast_workspace *)kmalloc(sizeof(struct fast_workspace));
    if (!ws)
        return NULL;

    /* Initialize hash table to "empty" */
    for (unsigned int i = 0; i < FAST_HASH_SIZE; i++)
        ws->hash_head[i] = -1;

    return ws;
}

static void fast_destroy_workspace(void *ws)
{
    kfree(ws);
}

/* ── Compression ───────────────────────────────────────────────────── */

static int fast_compress(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_len,
                         void *workspace)
{
    struct fast_workspace *ws = (struct fast_workspace *)workspace;
    int32_t *hash_head, *hash_next;
    size_t in_pos = 0, out_pos = 0;

    if (!src || !dst || src_len == 0)
        return -EINVAL;
    if (!ws)
        return -ENOMEM;

    hash_head = ws->hash_head;
    hash_next = ws->hash_next;

    /* Reset hash table for this compression run */
    for (unsigned int i = 0; i < FAST_HASH_SIZE; i++)
        hash_head[i] = -1;

    while (in_pos < src_len) {
        uint8_t control = 0;
        int control_pos = (int)out_pos;

        /* Reserve space for control byte */
        if (out_pos >= dst_len)
            return -ENOSPC;
        out_pos++;

        /* Process up to 8 operations per control byte */
        for (int op = 0; op < 8 && in_pos < src_len; op++) {
            int best_len = 0, best_off = 0;
            size_t max_match = src_len - in_pos;
            if (max_match > FAST_MAX_MATCH)
                max_match = FAST_MAX_MATCH;

            /* Try to find a match (need at least MIN_MATCH bytes) */
            if (max_match >= FAST_MIN_MATCH) {
                uint32_t h = fast_hash(&src[in_pos]);
                int32_t probe = hash_head[h];

                /* Walk hash chain looking for the longest match */
                while (probe >= 0 &&
                       (in_pos - (size_t)probe) <= FAST_CHAIN_MAX) {
                    size_t len = 0;
                    while (len < max_match &&
                           src[(size_t)probe + len] == src[in_pos + len])
                        len++;
                    if (len > (size_t)best_len) {
                        best_len = (int)len;
                        best_off = (int)(in_pos - (size_t)probe);
                        if (len == max_match)
                            break; /* Can't do better */
                    }
                    probe = hash_next[probe];
                }

                /* Update hash chain with current position */
                hash_next[in_pos] = hash_head[h];
                hash_head[h] = (int32_t)in_pos;
            }

            if (best_len >= FAST_MIN_MATCH) {
                /* Emit a reference */
                control |= (uint8_t)(1U << op);

                if (out_pos + 2 > dst_len)
                    return -ENOSPC;

                /* Encode: 12-bit offset, 4-bit (length - 3) */
                uint32_t off_code = (uint32_t)best_off;
                uint32_t len_code = (uint32_t)(best_len - FAST_MIN_MATCH);
                dst[out_pos] = (uint8_t)(((off_code >> 4) & 0xF0) | len_code);
                dst[out_pos + 1] = (uint8_t)(off_code & 0xFF);
                out_pos += 2;
                in_pos += (size_t)best_len;
            } else {
                /* Emit a literal — also update hash for this position */
                if (max_match >= FAST_MIN_MATCH) {
                    uint32_t h = fast_hash(&src[in_pos]);
                    hash_next[in_pos] = hash_head[h];
                    hash_head[h] = (int32_t)in_pos;
                }

                if (out_pos >= dst_len)
                    return -ENOSPC;

                dst[out_pos++] = src[in_pos++];
            }
        }

        /* Write the control byte for this group */
        dst[control_pos] = control;
    }

    return (int)out_pos;
}

/* ── Decompression ─────────────────────────────────────────────────── */

static int fast_decompress(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_len,
                           void *workspace)
{
    size_t in_pos = 0, out_pos = 0;
    (void)workspace; /* Fast decompression doesn't need workspace */

    if (!src || !dst || src_len == 0)
        return -EINVAL;

    while (in_pos < src_len) {
        uint8_t control = src[in_pos++];

        for (int op = 0; op < 8; op++) {
            if (control & (1U << op)) {
                /* Reference: 2 bytes */
                if (in_pos + 2 > src_len)
                    return -EINVAL;

                uint32_t offset = (((uint32_t)src[in_pos] & 0xF0) << 4) |
                                   (uint32_t)src[in_pos + 1];
                int match_len = (src[in_pos] & 0x0F) + FAST_MIN_MATCH;
                in_pos += 2;

                if (offset == 0 || offset > (uint32_t)out_pos)
                    return -EINVAL;
                if (out_pos + (size_t)match_len > dst_len)
                    return -ENOSPC;

                /* Copy match (may overlap — use byte-by-byte copy) */
                uint8_t *match_src = &dst[out_pos - offset];
                for (int i = 0; i < match_len; i++)
                    dst[out_pos++] = match_src[i];
            } else {
                /* Literal: 1 byte */
                if (in_pos >= src_len)
                    return (int)out_pos; /* Normal end of stream */
                if (out_pos >= dst_len)
                    return -ENOSPC;

                dst[out_pos++] = src[in_pos++];
            }
        }
    }

    return (int)out_pos;
}

/* ── Ops descriptor ────────────────────────────────────────────────── */

static const struct zcomp_ops fast_ops = {
    .name              = "fast",
    .algo_id           = ZCOMP_ALGO_FAST,
    .compress          = fast_compress,
    .decompress        = fast_decompress,
    .create_workspace  = fast_create_workspace,
    .destroy_workspace = fast_destroy_workspace,
};

/* ── Registration ──────────────────────────────────────────────────── */

int zcomp_fast_init(void)
{
    return zcomp_register(&fast_ops);
}

/* ── zcomp_fast_compress ─────────────────────────────── */
int zcomp_fast_compress(const void *src, size_t slen, void *dst, size_t *dlen)
{
    if (!src || !dst || !dlen || slen == 0)
        return -EINVAL;

    /* Try LZ4-style fast compression first using the workspace.
     * If compression doesn't save space, store uncompressed with a marker. */

    /* First, try the fast compressor from this file */
    void *ws = fast_create_workspace();
    if (!ws) {
        /* Fall back to "no compression" marker */
        if (*dlen < slen + 8)
            return -ENOSPC;
        /* Store uncompressed page with a special header */
        *(uint32_t *)dst = 0;       /* algo=0 means uncompressed */
        *(uint32_t *)((uint8_t *)dst + 4) = (uint32_t)slen; /* orig size */
        memcpy((uint8_t *)dst + 8, src, slen);
        *dlen = slen + 8;
        return 0;
    }

    int ret = fast_compress((const uint8_t *)src, slen, (uint8_t *)dst, *dlen, ws);
    fast_destroy_workspace(ws);

    if (ret > 0 && (size_t)ret < slen) {
        *dlen = (size_t)ret;
        return 0;
    }

    /* Compression didn't save space — store uncompressed */
    if (*dlen < slen + 8)
        return -ENOSPC;
    *(uint32_t *)dst = 0;       /* uncompressed marker */
    *(uint32_t *)((uint8_t *)dst + 4) = (uint32_t)slen;
    memcpy((uint8_t *)dst + 8, src, slen);
    *dlen = slen + 8;
    return 0;
}

/* ── zcomp_fast_decompress ─────────────────────────────── */
int zcomp_fast_decompress(const void *src, size_t slen, void *dst, size_t *dlen)
{
    if (!src || !dst || !dlen || slen == 0)
        return -EINVAL;

    /* Check for uncompressed marker */
    if (*(const uint32_t *)src == 0) {
        /* Uncompressed: skip header, memcpy */
        uint32_t orig_size = *(const uint32_t *)((const uint8_t *)src + 4);
        if (*dlen < (size_t)orig_size)
            return -ENOSPC;
        memcpy(dst, (const uint8_t *)src + 8, (size_t)orig_size);
        *dlen = (size_t)orig_size;
        return 0;
    }

    /* Try LZ4-style decompression */
    void *ws = fast_create_workspace();
    if (!ws) {
        kprintf("[zcomp_fast] zcomp_fast_decompress: no workspace\n");
        return -ENOMEM;
    }

    int ret = fast_decompress((const uint8_t *)src, slen, (uint8_t *)dst, *dlen, ws);
    fast_destroy_workspace(ws);

    if (ret > 0) {
        *dlen = (size_t)ret;
        return 0;
    }

    return -EINVAL;
}
