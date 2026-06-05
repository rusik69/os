#ifndef AES_XTS_H
#define AES_XTS_H

#include "types.h"
#include "aes.h"

/*
 * AES-XTS: IEEE 1619 disk encryption mode
 *
 * XTS encrypts each 512-byte sector independently using two AES keys:
 *   - key1 (data key): encrypts plaintext blocks
 *   - key2 (tweak key): generates per-block tweaks from the sector number
 *
 * For each 16-byte block j in sector i:
 *   Tweak_j = AES_encrypt(key2, i) ⊗ α^j    (GF(2^128) multiplication)
 *   C_j     = AES_encrypt(key1, P_j ⊕ Tweak_j) ⊕ Tweak_j
 *
 * The last block may be partial (ciphertext stealing), but for 512-byte
 * sectors aligned to 16 bytes, ciphertext stealing is not needed.
 *
 * Reference: IEEE Std 1619-2007
 */

#define XTS_SECTOR_SIZE  512   /* bytes per sector */
#define XTS_BLOCK_SIZE   AES_BLOCK_SIZE  /* 16 bytes */

/* AES-XTS context (two AES keys for 256-bit / 512-bit total) */
struct xts_ctx {
    struct aes_ctx data_ctx;   /* key1: data encryption key */
    struct aes_ctx tweak_ctx;  /* key2: tweak encryption key */
    int   key_len;             /* per-key length (16 or 32) */
};

/**
 * xts_init - initialise AES-XTS context
 * @ctx:      XTS context
 * @key1:     data encryption key (key_len bytes)
 * @key2:     tweak encryption key (key_len bytes)
 * @key_len:  length of each key (16 for AES-128, 32 for AES-256)
 * Returns 0 on success, -EINVAL on invalid key length.
 */
int xts_init(struct xts_ctx *ctx, const uint8_t *key1, const uint8_t *key2,
             int key_len);

/**
 * xts_encrypt_sector - encrypt one sector
 * @ctx:    XTS context
 * @sector: sector number (64-bit, used as tweak)
 * @in:     input plaintext (XTS_SECTOR_SIZE bytes)
 * @out:    output ciphertext (XTS_SECTOR_SIZE bytes)
 */
void xts_encrypt_sector(const struct xts_ctx *ctx, uint64_t sector,
                        const uint8_t *in, uint8_t *out);

/**
 * xts_decrypt_sector - decrypt one sector
 * @ctx:    XTS context
 * @sector: sector number (64-bit, used as tweak)
 * @in:     input ciphertext (XTS_SECTOR_SIZE bytes)
 * @out:    output plaintext (XTS_SECTOR_SIZE bytes)
 */
void xts_decrypt_sector(const struct xts_ctx *ctx, uint64_t sector,
                        const uint8_t *in, uint8_t *out);

/* Encrypt/decrypt multiple consecutive sectors */
void xts_encrypt(const struct xts_ctx *ctx, uint64_t start_sector,
                 const uint8_t *in, uint8_t *out, int num_sectors);
void xts_decrypt(const struct xts_ctx *ctx, uint64_t start_sector,
                 const uint8_t *in, uint8_t *out, int num_sectors);

#endif /* AES_XTS_H */
