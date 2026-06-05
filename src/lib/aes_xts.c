/*
 * aes_xts.c — AES-XTS (IEEE 1619) disk encryption mode
 *
 * AES-XTS encrypts sectors using two AES keys.  Each 512-byte sector
 * is encrypted independently with a tweak derived from the sector
 * number, making this suitable for block-device encryption where
 * sectors may be read/written in any order.
 *
 * Reference: IEEE Std 1619-2007
 *
 * Item 323: dm-crypt — transparent disk encryption
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "aes_xts.h"
#include "aes.h"
#include "string.h"
#include "printf.h"

/* ── GF(2^128) multiplication helpers ─────────────────────────────── */

/*
 * Multiply a 128-bit value by α (the primitive element) in GF(2^128).
 * The reduction polynomial is x^128 + x^7 + x^2 + x + 1 (0x87).
 *
 * Input/output are 16-byte arrays in little-endian byte order
 * (as used by XTS: byte 0 is the LSB of the polynomial).
 */
static void gf128_mul_x(uint8_t *x)
{
    uint64_t carry = 0;
    uint64_t lo, hi;

    /* Read as two 64-bit little-endian halves */
    lo = *(const uint64_t *)&x[0];
    hi = *(const uint64_t *)&x[8];

    /* Save MSB of the value for reduction */
    carry = (hi >> 63) & 1;

    /* Left shift by 1 */
    hi = (hi << 1) | (lo >> 63);
    lo <<= 1;

    /* Conditional reduction by the polynomial */
    if (carry)
        lo ^= 0x87ULL;

    /* Write back */
    *(uint64_t *)&x[0] = lo;
    *(uint64_t *)&x[8] = hi;
}

/*
 * Multiply a 128-bit value by α^j in GF(2^128).
 * Returns the tweak for the j-th block within a sector.
 */
static void gf128_mul_xn(uint8_t *result, const uint8_t *tweak, int n)
{
    memcpy(result, tweak, 16);
    for (int i = 0; i < n; i++)
        gf128_mul_x(result);
}

/* ── XTS tweak computation ────────────────────────────────────────── */

/*
 * Compute the tweak for a given sector number.
 * Tweak = AES_encrypt(key2, sector_number_encoded_as_128bit)
 *
 * The sector number is encoded as a 16-byte value in little-endian
 * byte order (bytes 0-7 = sector number, bytes 8-15 = 0).
 */
static void xts_compute_tweak(const struct aes_ctx *tweak_ctx,
                               uint64_t sector, uint8_t tweak[16])
{
    uint8_t sector_enc[16];

    memset(sector_enc, 0, 16);
    /* Encode sector number in little-endian (bytes 0-7) */
    sector_enc[0] = (uint8_t)(sector >> 0);
    sector_enc[1] = (uint8_t)(sector >> 8);
    sector_enc[2] = (uint8_t)(sector >> 16);
    sector_enc[3] = (uint8_t)(sector >> 24);
    sector_enc[4] = (uint8_t)(sector >> 32);
    sector_enc[5] = (uint8_t)(sector >> 40);
    sector_enc[6] = (uint8_t)(sector >> 48);
    sector_enc[7] = (uint8_t)(sector >> 56);

    aes_encrypt_block(tweak_ctx, sector_enc, tweak);
}

/* ── Public API ───────────────────────────────────────────────────── */

int xts_init(struct xts_ctx *ctx, const uint8_t *key1, const uint8_t *key2,
             int key_len)
{
    if (key_len != 16 && key_len != 32)
        return -22; /* -EINVAL */

    int ret;

    ret = aes_init(&ctx->data_ctx, key1, key_len);
    if (ret != 0)
        return ret;

    ret = aes_init(&ctx->tweak_ctx, key2, key_len);
    if (ret != 0)
        return ret;

    ctx->key_len = key_len;
    return 0;
}

void xts_encrypt_sector(const struct xts_ctx *ctx, uint64_t sector,
                        const uint8_t *in, uint8_t *out)
{
    uint8_t tweak[16];
    uint8_t block[16];
    uint8_t encrypted[16];
    int num_blocks = XTS_SECTOR_SIZE / XTS_BLOCK_SIZE; /* 32 */

    /* Compute the base tweak for this sector */
    xts_compute_tweak(&ctx->tweak_ctx, sector, tweak);

    for (int j = 0; j < num_blocks; j++) {
        const uint8_t *p = &in[j * 16];
        uint8_t *c = &out[j * 16];

        /* Copy base tweak and multiply by α^j */
        uint8_t block_tweak[16];
        gf128_mul_xn(block_tweak, tweak, j);

        /* P' = P ⊕ Tweak */
        for (int k = 0; k < 16; k++)
            block[k] = p[k] ^ block_tweak[k];

        /* C' = AES_encrypt(Key1, P') */
        aes_encrypt_block(&ctx->data_ctx, block, encrypted);

        /* C = C' ⊕ Tweak */
        for (int k = 0; k < 16; k++)
            c[k] = encrypted[k] ^ block_tweak[k];
    }
}

void xts_decrypt_sector(const struct xts_ctx *ctx, uint64_t sector,
                        const uint8_t *in, uint8_t *out)
{
    uint8_t tweak[16];
    uint8_t block[16];
    uint8_t decrypted[16];
    int num_blocks = XTS_SECTOR_SIZE / XTS_BLOCK_SIZE; /* 32 */

    /* Compute the base tweak for this sector */
    xts_compute_tweak(&ctx->tweak_ctx, sector, tweak);

    for (int j = 0; j < num_blocks; j++) {
        const uint8_t *c = &in[j * 16];
        uint8_t *p = &out[j * 16];

        /* Copy base tweak and multiply by α^j */
        uint8_t block_tweak[16];
        gf128_mul_xn(block_tweak, tweak, j);

        /* C' = C ⊕ Tweak */
        for (int k = 0; k < 16; k++)
            block[k] = c[k] ^ block_tweak[k];

        /* P' = AES_decrypt(Key1, C') */
        aes_decrypt_block(&ctx->data_ctx, block, decrypted);

        /* P = P' ⊕ Tweak */
        for (int k = 0; k < 16; k++)
            p[k] = decrypted[k] ^ block_tweak[k];
    }
}

void xts_encrypt(const struct xts_ctx *ctx, uint64_t start_sector,
                 const uint8_t *in, uint8_t *out, int num_sectors)
{
    for (int i = 0; i < num_sectors; i++) {
        xts_encrypt_sector(ctx, start_sector + i,
                           &in[i * XTS_SECTOR_SIZE],
                           &out[i * XTS_SECTOR_SIZE]);
    }
}

void xts_decrypt(const struct xts_ctx *ctx, uint64_t start_sector,
                 const uint8_t *in, uint8_t *out, int num_sectors)
{
    for (int i = 0; i < num_sectors; i++) {
        xts_decrypt_sector(ctx, start_sector + i,
                           &in[i * XTS_SECTOR_SIZE],
                           &out[i * XTS_SECTOR_SIZE]);
    }
}
