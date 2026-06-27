// SPDX-License-Identifier: GPL-2.0-only
/*
 * Poly1305 MAC (Message Authentication Code)
 *
 * Implements the Poly1305 one-time MAC as defined in RFC 8439.
 * Uses 130-bit integer arithmetic with 5 26-bit limbs.
 */
#include "types.h"
#include "string.h"
#include "printf.h"

#define POLY1305_KEY_SIZE  32
#define POLY1305_MAC_SIZE  16
#define POLY1305_BLOCK_SIZE 16

struct poly1305_ctx {
    uint32_t r[5];   /* key part r (clamped) */
    uint32_t h[5];   /* accumulator */
    uint32_t s[4];   /* key part s (for finalization) */
    uint8_t buf[16]; /* partial block buffer */
    size_t buf_used;
};

/* Clamp r according to RFC 8439 */
static void poly1305_set_key(struct poly1305_ctx *ctx, const uint8_t key[32])
{
    /* r = key[0..15] clamped */
    ctx->r[0] = ((uint32_t)key[0]) |
                ((uint32_t)key[1] << 8) |
                ((uint32_t)key[2] << 16) |
                ((uint32_t)(key[3] & 0x0f) << 24);
    ctx->r[1] = ((uint32_t)(key[3] >> 4) & 0x0f) |
                ((uint32_t)key[4] << 4) |
                ((uint32_t)key[5] << 12) |
                ((uint32_t)(key[6] & 0xfc) << 20);
    ctx->r[2] = ((uint32_t)(key[6] >> 2) & 0x03) |
                ((uint32_t)key[7] << 6) |
                ((uint32_t)key[8] << 14) |
                ((uint32_t)(key[9] & 0xf0) << 22);
    ctx->r[3] = ((uint32_t)(key[9] >> 4) & 0x0f) |
                ((uint32_t)key[10] << 4) |
                ((uint32_t)key[11] << 12) |
                ((uint32_t)(key[12] & 0xfc) << 20);
    ctx->r[4] = ((uint32_t)(key[12] >> 2) & 0x03) |
                ((uint32_t)key[13] << 6) |
                ((uint32_t)key[14] << 14) |
                ((uint32_t)(key[15] & 0xf0) << 22);

    /* s = key[16..31] (used for finalization) */
    for (int i = 0; i < 4; i++) {
        ctx->s[i] = ((uint32_t)key[16 + 4*i]) |
                    ((uint32_t)key[16 + 4*i + 1] << 8) |
                    ((uint32_t)key[16 + 4*i + 2] << 16) |
                    ((uint32_t)key[16 + 4*i + 3] << 24);
    }
}

/* Process a full 16-byte block */
static void poly1305_process_block(struct poly1305_ctx *ctx, const uint8_t block[16])
{
    uint32_t n[5];
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;

    /* Decode block into 5 26-bit limbs with high bit set */
    n[0] = ((uint32_t)block[0]) |
           ((uint32_t)block[1] << 8) |
           ((uint32_t)block[2] << 16) |
           ((uint32_t)(block[3] & 0x03) << 24);
    n[1] = ((uint32_t)(block[3] >> 2) & 0x3f) |
           ((uint32_t)block[4] << 6) |
           ((uint32_t)block[5] << 14) |
           ((uint32_t)(block[6] & 0x0f) << 22);
    n[2] = ((uint32_t)(block[6] >> 4) & 0x03) |
           ((uint32_t)block[7] << 4) |
           ((uint32_t)block[8] << 12) |
           ((uint32_t)(block[9] & 0x3f) << 20);
    n[3] = ((uint32_t)(block[9] >> 6) & 0x0f) |
           ((uint32_t)block[10] << 2) |
           ((uint32_t)block[11] << 10) |
           ((uint32_t)(block[12] & 0x0f) << 18);
    n[4] = ((uint32_t)(block[12] >> 4) & 0x0f) |
           ((uint32_t)block[13] << 4) |
           ((uint32_t)block[14] << 12) |
           ((uint32_t)(block[15] & 0x3f) << 20);

    /* Add the block to h */
    ctx->h[0] += n[0];
    ctx->h[1] += n[1];
    ctx->h[2] += n[2];
    ctx->h[3] += n[3];
    ctx->h[4] += n[4] + (1U << 24); /* add 2^130 - 5 = 1 (bit 130) */

    /* h = h * r mod (2^130 - 5) */
    d0 = (uint64_t)ctx->h[0] * ctx->r[0] +
         (uint64_t)ctx->h[1] * (5 * ctx->r[4]) +
         (uint64_t)ctx->h[2] * (5 * ctx->r[3]) +
         (uint64_t)ctx->h[3] * (5 * ctx->r[2]) +
         (uint64_t)ctx->h[4] * (5 * ctx->r[1]);
    d1 = (uint64_t)ctx->h[0] * ctx->r[1] +
         (uint64_t)ctx->h[1] * ctx->r[0] +
         (uint64_t)ctx->h[2] * (5 * ctx->r[4]) +
         (uint64_t)ctx->h[3] * (5 * ctx->r[3]) +
         (uint64_t)ctx->h[4] * (5 * ctx->r[2]);
    d2 = (uint64_t)ctx->h[0] * ctx->r[2] +
         (uint64_t)ctx->h[1] * ctx->r[1] +
         (uint64_t)ctx->h[2] * ctx->r[0] +
         (uint64_t)ctx->h[3] * (5 * ctx->r[4]) +
         (uint64_t)ctx->h[4] * (5 * ctx->r[3]);
    d3 = (uint64_t)ctx->h[0] * ctx->r[3] +
         (uint64_t)ctx->h[1] * ctx->r[2] +
         (uint64_t)ctx->h[2] * ctx->r[1] +
         (uint64_t)ctx->h[3] * ctx->r[0] +
         (uint64_t)ctx->h[4] * (5 * ctx->r[4]);
    d4 = (uint64_t)ctx->h[0] * ctx->r[4] +
         (uint64_t)ctx->h[1] * ctx->r[3] +
         (uint64_t)ctx->h[2] * ctx->r[2] +
         (uint64_t)ctx->h[3] * ctx->r[1] +
         (uint64_t)ctx->h[4] * ctx->r[0];

    /* Reduce (carry) */
    c = (uint32_t)(d0 >> 26); ctx->h[0] = (uint32_t)d0 & 0x3ffffff;
    d1 += c;
    c = (uint32_t)(d1 >> 26); ctx->h[1] = (uint32_t)d1 & 0x3ffffff;
    d2 += c;
    c = (uint32_t)(d2 >> 26); ctx->h[2] = (uint32_t)d2 & 0x3ffffff;
    d3 += c;
    c = (uint32_t)(d3 >> 26); ctx->h[3] = (uint32_t)d3 & 0x3ffffff;
    d4 += c;
    c = (uint32_t)(d4 >> 26); ctx->h[4] = (uint32_t)d4 & 0x3ffffff;
    ctx->h[0] += c * 5;
    c = ctx->h[0] >> 26; ctx->h[0] &= 0x3ffffff; ctx->h[1] += c;
}

void poly1305_init(struct poly1305_ctx *ctx, const uint8_t key[32])
{
    memset(ctx, 0, sizeof(*ctx));
    poly1305_set_key(ctx, key);
}

void poly1305_update(struct poly1305_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t i;

    /* Process any leftover partial block */
    if (ctx->buf_used > 0) {
        size_t fill = 16 - ctx->buf_used;
        if (fill > len) fill = len;
        memcpy(ctx->buf + ctx->buf_used, data, fill);
        ctx->buf_used += fill;
        data += fill;
        len -= fill;
        if (ctx->buf_used == 16) {
            poly1305_process_block(ctx, ctx->buf);
            ctx->buf_used = 0;
        }
    }

    /* Process full blocks */
    for (i = 0; i + 16 <= len; i += 16)
        poly1305_process_block(ctx, data + i);

    /* Save remaining bytes */
    size_t rem = len - i;
    if (rem > 0) {
        memcpy(ctx->buf, data + i, rem);
        ctx->buf_used = rem;
    }
}

void poly1305_final(struct poly1305_ctx *ctx, uint8_t mac[16])
{
    uint64_t f0, f1, f2, f3;
    uint32_t g0, g1, g2, g3, g4;
    uint32_t mask;
    uint32_t c;

    /* Process final block (with padding) */
    if (ctx->buf_used > 0) {
        memset(ctx->buf + ctx->buf_used, 0, 16 - ctx->buf_used);
        ctx->buf[ctx->buf_used] = 1; /* set high bit */
        poly1305_process_block(ctx, ctx->buf);
    }

    /* Fully carry h */
    c = ctx->h[1] >> 26; ctx->h[1] &= 0x3ffffff; ctx->h[2] += c;
    c = ctx->h[2] >> 26; ctx->h[2] &= 0x3ffffff; ctx->h[3] += c;
    c = ctx->h[3] >> 26; ctx->h[3] &= 0x3ffffff; ctx->h[4] += c;
    c = ctx->h[4] >> 26; ctx->h[4] &= 0x3ffffff; ctx->h[0] += c * 5;
    c = ctx->h[0] >> 26; ctx->h[0] &= 0x3ffffff; ctx->h[1] += c;

    /* Compute h - (2^130-5) */
    g0 = ctx->h[0] + 5;
    c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = ctx->h[1] + c;
    c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = ctx->h[2] + c;
    c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = ctx->h[3] + c;
    c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = ctx->h[4] + c - (1U << 26);

    /* Select h or h-(2^130-5) based on borrow */
    mask = (g4 >> 31) - 1;
    ctx->h[0] = (ctx->h[0] & ~mask) | (g0 & mask);
    ctx->h[1] = (ctx->h[1] & ~mask) | (g1 & mask);
    ctx->h[2] = (ctx->h[2] & ~mask) | (g2 & mask);
    ctx->h[3] = (ctx->h[3] & ~mask) | (g3 & mask);
    ctx->h[4] = (ctx->h[4] & ~mask) | (g4 & mask);

    /* Add s */
    f0 = (uint64_t)ctx->h[0] + ctx->s[0];
    f1 = (uint64_t)ctx->h[1] + ctx->s[1];
    f2 = (uint64_t)ctx->h[2] + ctx->s[2];
    f3 = (uint64_t)ctx->h[3] + ctx->s[3];

    /* Convert to bytes */
    mac[0]  = (uint8_t)(f0 & 0xFF); mac[1]  = (uint8_t)((f0 >> 8) & 0xFF);
    mac[2]  = (uint8_t)((f0 >> 16) & 0xFF); mac[3]  = (uint8_t)((f0 >> 24) & 0xFF);
    mac[4]  = (uint8_t)(f1 & 0xFF); mac[5]  = (uint8_t)((f1 >> 8) & 0xFF);
    mac[6]  = (uint8_t)((f1 >> 16) & 0xFF); mac[7]  = (uint8_t)((f1 >> 24) & 0xFF);
    mac[8]  = (uint8_t)(f2 & 0xFF); mac[9]  = (uint8_t)((f2 >> 8) & 0xFF);
    mac[10] = (uint8_t)((f2 >> 16) & 0xFF); mac[11] = (uint8_t)((f2 >> 24) & 0xFF);
    mac[12] = (uint8_t)(f3 & 0xFF); mac[13] = (uint8_t)((f3 >> 8) & 0xFF);
    mac[14] = (uint8_t)((f3 >> 16) & 0xFF); mac[15] = (uint8_t)((f3 >> 24) & 0xFF);
}

void poly1305_mac(uint8_t mac[16], const uint8_t *data, size_t len,
                  const uint8_t key[32])
{
    struct poly1305_ctx ctx;
    poly1305_init(&ctx, key);
    poly1305_update(&ctx, data, len);
    poly1305_final(&ctx, mac);
}

/* ── poly1305_auth ─────────────────────────────── */
int poly1305_auth(void *mac, const void *msg, size_t mlen, const void *key)
{
    if (!mac || !key)
        return -1;
    poly1305_mac((uint8_t *)mac, (const uint8_t *)msg, mlen, (const uint8_t *)key);
    return 0;
}
