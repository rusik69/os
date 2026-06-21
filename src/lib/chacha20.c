// SPDX-License-Identifier: GPL-2.0-only
/*
 * ChaCha20 stream cipher (20 rounds, 64-byte block)
 *
 * Implements the ChaCha20 stream cipher as defined in RFC 8439.
 * Block size: 64 bytes (16 x 32-bit words)
 * Rounds: 20 (10 double rounds)
 */
#include "types.h"
#include "string.h"
#include "printf.h"

/* ChaCha20 quarter round */
#define QUARTERROUND(a, b, c, d) \
    a += b; d ^= a; d = (d << 16) | (d >> 16); \
    c += d; b ^= c; b = (b << 12) | (b >> 20); \
    a += b; d ^= a; d = (d << 8)  | (d >> 24); \
    c += d; b ^= c; b = (b << 7)  | (b >> 25);

struct chacha20_ctx {
    uint32_t state[16];
};

/* Initial state: "expand 32-byte k" */
static const uint32_t chacha20_sigma[4] = {
    0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
};

static void chacha20_init(struct chacha20_ctx *ctx, const uint8_t key[32], const uint8_t nonce[12])
{
    int i;
    /* Constant */
    ctx->state[0] = chacha20_sigma[0];
    ctx->state[1] = chacha20_sigma[1];
    ctx->state[2] = chacha20_sigma[2];
    ctx->state[3] = chacha20_sigma[3];

    /* Key (8 words, little-endian) */
    for (i = 0; i < 8; i++)
        ctx->state[4 + i] = ((uint32_t)key[4*i]) |
                            ((uint32_t)key[4*i+1] << 8) |
                            ((uint32_t)key[4*i+2] << 16) |
                            ((uint32_t)key[4*i+3] << 24);

    /* Counter starts at 0 */
    ctx->state[12] = 0;

    /* Nonce (3 words: state[13..15]) */
    for (i = 0; i < 3; i++)
        ctx->state[13 + i] = ((uint32_t)nonce[4*i]) |
                             ((uint32_t)nonce[4*i+1] << 8) |
                             ((uint32_t)nonce[4*i+2] << 16) |
                             ((uint32_t)nonce[4*i+3] << 24);
}

static void chacha20_block(struct chacha20_ctx *ctx, uint32_t output[16])
{
    int i;
    uint32_t x[16];

    for (i = 0; i < 16; i++)
        x[i] = ctx->state[i];

    /* 20 rounds = 10 double rounds */
    for (i = 0; i < 10; i++) {
        /* Column round */
        QUARTERROUND(x[0], x[4], x[8],  x[12]);
        QUARTERROUND(x[1], x[5], x[9],  x[13]);
        QUARTERROUND(x[2], x[6], x[10], x[14]);
        QUARTERROUND(x[3], x[7], x[11], x[15]);
        /* Diagonal round */
        QUARTERROUND(x[0], x[5], x[10], x[15]);
        QUARTERROUND(x[1], x[6], x[11], x[12]);
        QUARTERROUND(x[2], x[7], x[8],  x[13]);
        QUARTERROUND(x[3], x[4], x[9],  x[14]);
    }

    for (i = 0; i < 16; i++)
        output[i] = x[i] + ctx->state[i];

    /* Increment counter */
    ctx->state[12]++;
    if (ctx->state[12] == 0)
        ctx->state[13]++;
}

void lib_chacha20_encrypt(uint8_t *out, const uint8_t *in, size_t len,
                      const uint8_t key[32], const uint8_t nonce[12],
                      uint64_t counter)
{
    struct chacha20_ctx ctx;
    uint32_t block[16];
    uint8_t keystream[64];
    size_t i;

    chacha20_init(&ctx, key, nonce);

    /* Set initial counter */
    ctx.state[12] = (uint32_t)(counter & 0xFFFFFFFFULL);
    ctx.state[13] = (uint32_t)(counter >> 32);

    while (len > 0) {
        chacha20_block(&ctx, block);

        /* Convert block to byte array (little-endian) */
        for (i = 0; i < 16; i++) {
            keystream[4*i]     = (uint8_t)(block[i] & 0xFF);
            keystream[4*i + 1] = (uint8_t)((block[i] >> 8) & 0xFF);
            keystream[4*i + 2] = (uint8_t)((block[i] >> 16) & 0xFF);
            keystream[4*i + 3] = (uint8_t)((block[i] >> 24) & 0xFF);
        }

        size_t todo = (len > 64) ? 64 : len;
        for (i = 0; i < todo; i++)
            out[i] = in[i] ^ keystream[i];

        out += todo;
        in += todo;
        len -= todo;
    }
}

/* ── chacha20_encrypt ─────────────────────────────── */
int chacha20_encrypt(void *ctx, const void *src, void *dst, size_t len)
{
    /* If ctx is provided, interpret it as raw state; otherwise we lack key.
     * Since lib_chacha20_encrypt takes key+nonce, we need them.
     * For now, the caller must have set up a chacha20_ctx (as in the struct above).
     * We'll use a simplified approach: the ctx is actually a uint32_t[16] state. */
    if (!ctx || !src || !dst)
        return -1;

    uint32_t *state = (uint32_t *)ctx;
    uint8_t keystream[64];
    size_t out_idx = 0;
    size_t in_idx = 0;

    while (len > 0) {
        uint32_t block[16];
        for (int i = 0; i < 16; i++)
            block[i] = state[i];

        for (int i = 0; i < 10; i++) {
            QUARTERROUND(block[0], block[4], block[8],  block[12]);
            QUARTERROUND(block[1], block[5], block[9],  block[13]);
            QUARTERROUND(block[2], block[6], block[10], block[14]);
            QUARTERROUND(block[3], block[7], block[11], block[15]);
            QUARTERROUND(block[0], block[5], block[10], block[15]);
            QUARTERROUND(block[1], block[6], block[11], block[12]);
            QUARTERROUND(block[2], block[7], block[8],  block[13]);
            QUARTERROUND(block[3], block[4], block[9],  block[14]);
        }

        for (int i = 0; i < 16; i++)
            block[i] += state[i];

        for (int i = 0; i < 16; i++) {
            keystream[4*i]     = (uint8_t)(block[i] & 0xFF);
            keystream[4*i + 1] = (uint8_t)((block[i] >> 8) & 0xFF);
            keystream[4*i + 2] = (uint8_t)((block[i] >> 16) & 0xFF);
            keystream[4*i + 3] = (uint8_t)((block[i] >> 24) & 0xFF);
        }

        size_t todo = (len > 64) ? 64 : len;
        const uint8_t *in = (const uint8_t *)src + in_idx;
        uint8_t *out = (uint8_t *)dst + out_idx;
        for (size_t i = 0; i < todo; i++)
            out[i] = in[i] ^ keystream[i];

        out_idx += todo;
        in_idx += todo;
        len -= todo;

        /* Increment counter */
        state[12]++;
        if (state[12] == 0)
            state[13]++;
    }
    return 0;
}
