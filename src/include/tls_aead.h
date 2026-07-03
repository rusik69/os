/* tls_aead.h — TLS AEAD encryption API
 *
 * Unified interface over AES-GCM and ChaCha20-Poly1305 for TLS record
 * protection (RFC 8446 §5.2, RFC 5246 §6.2.3.3).
 *
 * The nonce is constructed from the TLS cipher state:
 *   TLS 1.2:  nonce = fixed_iv (4B) || explicit_iv (8B)  — 12 bytes total
 *   TLS 1.3:  nonce = (fixed_iv[0..3] || explicit_iv[0..7]) XOR (0^4 || seq_num)
 *              — 12 bytes total
 *
 * The additional data (AAD) for TLS 1.2:
 *   seq_num (8B BE) || content_type (1B) || version (2B) || length (2B)
 *
 * The additional data (AAD) for TLS 1.3:
 *   content_type (1B) || version (2B) || length (2B)
 *   (The content type is also appended to the plaintext per RFC 8446 §5.2)
 */

#ifndef TLS_AEAD_H
#define TLS_AEAD_H

#include "types.h"
#include "tls.h"

#define TLS_AEAD_NONCE_LEN    12
#define TLS_AEAD_MAX_TAG_LEN  16

/* ── TLS AEAD context ──────────────────────────────────────────────── */
struct tls_aead_ctx {
	uint16_t cipher_suite;     /* TLS cipher suite code point */
	int      enc_key_len;      /* symmetric key length in bytes */
	int      tag_len;          /* authentication tag length */
	int      is_aead;          /* 1 = AEAD cipher */
	uint8_t  enc_key[32];      /* encryption key */
};

/**
 * tls_aead_init — initialise a TLS AEAD context for a cipher suite
 * @ctx:          TLS AEAD context (uninitialised)
 * @key:          encryption key bytes
 * @key_len:      key length in bytes
 * @cipher_suite: TLS cipher suite identifier (e.g. TLS_AES_128_GCM_SHA256)
 *
 * Returns 0 on success, or negative errno on failure.
 */
int tls_aead_init(struct tls_aead_ctx *ctx,
                  const uint8_t *key, int key_len,
                  uint16_t cipher_suite);

/**
 * tls_aead_build_nonce — construct per-record AEAD nonce from cipher state
 * @cs:      TLS cipher state (provides fixed_iv, explicit_iv, seq_num)
 * @version: TLS protocol version (for version-specific construction rules)
 * @nonce:   output buffer (TLS_AEAD_NONCE_LEN bytes)
 *
 * For TLS 1.2: nonce = fixed_iv (4B) || explicit_iv (8B)
 * For TLS 1.3: nonce = (fixed_iv || explicit_iv) XOR (0^4 || seq_num_BE8)
 */
void tls_aead_build_nonce(const struct tls_cipher_state *cs,
                          uint16_t version,
                          uint8_t nonce[TLS_AEAD_NONCE_LEN]);

/**
 * tls_aead_encrypt — AEAD encrypt a TLS record fragment
 * @ctx:      TLS AEAD context (initialised)
 * @nonce:    12-byte nonce (constructed per TLS version rules)
 * @aad:      additional authenticated data
 * @aad_len:  length of AAD in bytes
 * @plain:    plaintext input
 * @plain_len: length of plaintext in bytes
 * @cipher:   output ciphertext (same length as plaintext)
 * @tag:      output authentication tag (ctx->tag_len bytes)
 *
 * Returns 0 on success, or negative errno on failure.
 */
int tls_aead_encrypt(struct tls_aead_ctx *ctx,
                     const uint8_t *nonce,
                     const uint8_t *aad, int aad_len,
                     const uint8_t *plain, int plain_len,
                     uint8_t *cipher, uint8_t *tag);

/**
 * tls_aead_decrypt — AEAD decrypt a TLS record fragment
 * @ctx:       TLS AEAD context (initialised)
 * @nonce:     12-byte nonce (constructed per TLS version rules)
 * @aad:       additional authenticated data
 * @aad_len:   length of AAD in bytes
 * @cipher:    ciphertext input
 * @cipher_len: length of ciphertext in bytes
 * @tag:       expected authentication tag (ctx->tag_len bytes)
 * @plain:     output plaintext (same length as ciphertext)
 *
 * Returns 0 on success, negative errno on failure (including
 * -EBADMSG on authentication failure).
 */
int tls_aead_decrypt(struct tls_aead_ctx *ctx,
                     const uint8_t *nonce,
                     const uint8_t *aad, int aad_len,
                     const uint8_t *cipher, int cipher_len,
                     const uint8_t *tag, uint8_t *plain);

#endif /* TLS_AEAD_H */
