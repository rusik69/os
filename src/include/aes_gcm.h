/* aes_gcm.h — AES-GCM AEAD mode (NIST SP 800-38D)
 *
 * Authenticated encryption using AES in Galois/Counter Mode.
 * Supports AES-128-GCM and AES-256-GCM with 12-byte nonces.
 */

#ifndef AES_GCM_H
#define AES_GCM_H

#include "types.h"
#include "aes.h"

#define AES_GCM_BLOCK_SIZE   16
#define AES_GCM_MIN_IV_LEN   12
#define AES_GCM_MAX_TAG_LEN  16
#define AES_GCM_DEFAULT_TAG_LEN 16

/* ── AES-GCM context ─────────────────────────────────────────────── */
struct aes_gcm_ctx {
	struct aes_ctx  aes;           /* AES encryption context */
	uint8_t         H[AES_GCM_BLOCK_SIZE];  /* GHASH H = AES_K(0^128) */
	int             key_len;       /* 16 (AES-128) or 32 (AES-256) */
	int             tag_len;       /* auth tag length (default 16) */
};

/**
 * aes_gcm_init — initialise AES-GCM context with key
 * @ctx:     AES-GCM context (uninitialised)
 * @key:     encryption key bytes
 * @key_len: key length: 16 (AES-128) or 32 (AES-256)
 * @tag_len: authentication tag length in bytes (0 = use default 16)
 *
 * Returns 0 on success, or negative errno on failure.
 */
int aes_gcm_init(struct aes_gcm_ctx *ctx,
                 const uint8_t *key, int key_len,
                 int tag_len);

/**
 * aes_gcm_encrypt — AES-GCM authenticated encryption
 * @ctx:      AES-GCM context (initialised with aes_gcm_init)
 * @iv:       initialisation vector / nonce (12 bytes)
 * @iv_len:   length of IV (must be 12)
 * @aad:      additional authenticated data (may be NULL if aad_len == 0)
 * @aad_len:  length of AAD in bytes
 * @plain:    plaintext input
 * @plain_len: length of plaintext in bytes
 * @cipher:   output ciphertext (same length as plaintext)
 * @tag:      output authentication tag (ctx->tag_len bytes)
 *
 * Returns 0 on success, or negative errno on failure.
 */
int aes_gcm_encrypt(struct aes_gcm_ctx *ctx,
                    const uint8_t *iv, int iv_len,
                    const uint8_t *aad, int aad_len,
                    const uint8_t *plain, int plain_len,
                    uint8_t *cipher, uint8_t *tag);

/**
 * aes_gcm_decrypt — AES-GCM authenticated decryption
 * @ctx:       AES-GCM context (initialised with aes_gcm_init)
 * @iv:        initialisation vector / nonce (12 bytes)
 * @iv_len:    length of IV (must be 12)
 * @aad:       additional authenticated data (may be NULL if aad_len == 0)
 * @aad_len:   length of AAD in bytes
 * @cipher:    ciphertext input
 * @cipher_len: length of ciphertext in bytes
 * @tag:       expected authentication tag (ctx->tag_len bytes)
 * @plain:     output plaintext (same length as ciphertext)
 *
 * Returns 0 on success, negative errno on failure (including
 * -EBADMSG if the authentication tag does not match).
 */
int aes_gcm_decrypt(struct aes_gcm_ctx *ctx,
                    const uint8_t *iv, int iv_len,
                    const uint8_t *aad, int aad_len,
                    const uint8_t *cipher, int cipher_len,
                    const uint8_t *tag, uint8_t *plain);

#endif /* AES_GCM_H */
