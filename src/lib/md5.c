#include "types.h"
#include "string.h"
#include "printf.h"
#include "md5.h"

/* MD5 initial state */
static const uint32_t H0[4] = {
    0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
};

/* MD5 round constants */
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

/* MD5 per-round shift amounts */
static const uint32_t S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define FF(a, b, c, d, x, s, ac) \
    do { \
        (a) += F((b), (c), (d)) + (x) + (ac); \
        (a) = ROTL32((a), (s)); \
        (a) += (b); \
    } while (0)

#define GG(a, b, c, d, x, s, ac) \
    do { \
        (a) += G((b), (c), (d)) + (x) + (ac); \
        (a) = ROTL32((a), (s)); \
        (a) += (b); \
    } while (0)

#define HH(a, b, c, d, x, s, ac) \
    do { \
        (a) += H((b), (c), (d)) + (x) + (ac); \
        (a) = ROTL32((a), (s)); \
        (a) += (b); \
    } while (0)

#define II(a, b, c, d, x, s, ac) \
    do { \
        (a) += I((b), (c), (d)) + (x) + (ac); \
        (a) = ROTL32((a), (s)); \
        (a) += (b); \
    } while (0)

static void md5_transform(struct md5_ctx *ctx,
                          const uint8_t block[MD5_BLOCK_SIZE])
{
    uint32_t a, b, c, d, X[16] = {0};

    for (int i = 0; i < 16; i++) {
        X[i] = (uint32_t)block[4*i] |
               ((uint32_t)block[4*i+1] << 8) |
               ((uint32_t)block[4*i+2] << 16) |
               ((uint32_t)block[4*i+3] << 24);
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];

    /* Round 1 */
    FF(a, b, c, d, X[ 0], S[ 0], K[ 0]); FF(d, a, b, c, X[ 1], S[ 1], K[ 1]);
    FF(c, d, a, b, X[ 2], S[ 2], K[ 2]); FF(b, c, d, a, X[ 3], S[ 3], K[ 3]);
    FF(a, b, c, d, X[ 4], S[ 4], K[ 4]); FF(d, a, b, c, X[ 5], S[ 5], K[ 5]);
    FF(c, d, a, b, X[ 6], S[ 6], K[ 6]); FF(b, c, d, a, X[ 7], S[ 7], K[ 7]);
    FF(a, b, c, d, X[ 8], S[ 8], K[ 8]); FF(d, a, b, c, X[ 9], S[ 9], K[ 9]);
    FF(c, d, a, b, X[10], S[10], K[10]); FF(b, c, d, a, X[11], S[11], K[11]);
    FF(a, b, c, d, X[12], S[12], K[12]); FF(d, a, b, c, X[13], S[13], K[13]);
    FF(c, d, a, b, X[14], S[14], K[14]); FF(b, c, d, a, X[15], S[15], K[15]);

    /* Round 2 */
    GG(a, b, c, d, X[ 1], S[16], K[16]); GG(d, a, b, c, X[ 6], S[17], K[17]);
    GG(c, d, a, b, X[11], S[18], K[18]); GG(b, c, d, a, X[ 0], S[19], K[19]);
    GG(a, b, c, d, X[ 5], S[20], K[20]); GG(d, a, b, c, X[10], S[21], K[21]);
    GG(c, d, a, b, X[15], S[22], K[22]); GG(b, c, d, a, X[ 4], S[23], K[23]);
    GG(a, b, c, d, X[ 9], S[24], K[24]); GG(d, a, b, c, X[14], S[25], K[25]);
    GG(c, d, a, b, X[ 3], S[26], K[26]); GG(b, c, d, a, X[ 8], S[27], K[27]);
    GG(a, b, c, d, X[13], S[28], K[28]); GG(d, a, b, c, X[ 2], S[29], K[29]);
    GG(c, d, a, b, X[ 7], S[30], K[30]); GG(b, c, d, a, X[12], S[31], K[31]);

    /* Round 3 */
    HH(a, b, c, d, X[ 5], S[32], K[32]); HH(d, a, b, c, X[ 8], S[33], K[33]);
    HH(c, d, a, b, X[11], S[34], K[34]); HH(b, c, d, a, X[14], S[35], K[35]);
    HH(a, b, c, d, X[ 1], S[36], K[36]); HH(d, a, b, c, X[ 4], S[37], K[37]);
    HH(c, d, a, b, X[ 7], S[38], K[38]); HH(b, c, d, a, X[10], S[39], K[39]);
    HH(a, b, c, d, X[13], S[40], K[40]); HH(d, a, b, c, X[ 0], S[41], K[41]);
    HH(c, d, a, b, X[ 3], S[42], K[42]); HH(b, c, d, a, X[ 6], S[43], K[43]);
    HH(a, b, c, d, X[ 9], S[44], K[44]); HH(d, a, b, c, X[12], S[45], K[45]);
    HH(c, d, a, b, X[15], S[46], K[46]); HH(b, c, d, a, X[ 2], S[47], K[47]);

    /* Round 4 */
    II(a, b, c, d, X[ 0], S[48], K[48]); II(d, a, b, c, X[ 7], S[49], K[49]);
    II(c, d, a, b, X[14], S[50], K[50]); II(b, c, d, a, X[ 5], S[51], K[51]);
    II(a, b, c, d, X[12], S[52], K[52]); II(d, a, b, c, X[ 3], S[53], K[53]);
    II(c, d, a, b, X[10], S[54], K[54]); II(b, c, d, a, X[ 1], S[55], K[55]);
    II(a, b, c, d, X[ 8], S[56], K[56]); II(d, a, b, c, X[15], S[57], K[57]);
    II(c, d, a, b, X[ 6], S[58], K[58]); II(b, c, d, a, X[13], S[59], K[59]);
    II(a, b, c, d, X[ 4], S[60], K[60]); II(d, a, b, c, X[11], S[61], K[61]);
    II(c, d, a, b, X[ 2], S[62], K[62]); II(b, c, d, a, X[ 9], S[63], K[63]);

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

void md5_init(struct md5_ctx *ctx)
{
    ctx->count = 0;
    for (int i = 0; i < 4; i++)
        ctx->state[i] = H0[i];
}

void md5_update(struct md5_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)(ctx->count & 0x3F);
    ctx->count += len;

    if (idx) {
        size_t fill = MD5_BLOCK_SIZE - idx;
        if (len < fill) {
            memcpy(&ctx->buffer[idx], p, len);
            return;
        }
        memcpy(&ctx->buffer[idx], p, fill);
        md5_transform(ctx, ctx->buffer);
        p += fill;
        len -= fill;
    }

    while (len >= MD5_BLOCK_SIZE) {
        md5_transform(ctx, p);
        p += MD5_BLOCK_SIZE;
        len -= MD5_BLOCK_SIZE;
    }

    if (len)
        memcpy(ctx->buffer, p, len);
}

void md5_final(uint8_t digest[MD5_DIGEST_SIZE], struct md5_ctx *ctx)
{
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 0x3F);
    size_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);

    uint8_t padding[128];
    memset(padding, 0, pad_len);
    padding[0] = 0x80;
    md5_update(ctx, padding, pad_len);

    uint8_t len_buf[8];
    for (int i = 0; i < 8; i++)
        len_buf[i] = (uint8_t)(bits >> (8 * i));

    md5_update(ctx, len_buf, 8);

    for (int i = 0; i < 4; i++) {
        digest[4*i]   = (uint8_t)(ctx->state[i]);
        digest[4*i+1] = (uint8_t)(ctx->state[i] >> 8);
        digest[4*i+2] = (uint8_t)(ctx->state[i] >> 16);
        digest[4*i+3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

/**
 * md5_hash - Compute MD5 hash of a data buffer
 * @digest: Output buffer of MD5_DIGEST_SIZE (16) bytes to receive the hash
 * @data: Pointer to the input data buffer
 * @len: Length of the input data in bytes
 *
 * Convenience wrapper that initializes an MD5 context, feeds the data,
 * and finalizes the digest in a single call.
 *
 * Context: Any context.
 * Return: void (digest written to @digest).
 */
void md5_hash(uint8_t digest[MD5_DIGEST_SIZE],
              const void *data, size_t len)
{
    struct md5_ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(digest, &ctx);
}

void md5_init_crypto(void)
{
    kprintf("[OK] MD5 initialized\n");
}

