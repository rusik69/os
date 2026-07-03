/* tls_aead.c — TLS AEAD encryption implementation
 *
 * Unified wrapper over AES-GCM and ChaCha20-Poly1305 for TLS record
 * protection.  Dispatches to the appropriate AEAD primitive based on
 * the cipher suite identifier.
 */

#include "tls.h"
#include "tls_aead.h"
#include "aes_gcm.h"
#include "string.h"
#include "errno.h"

/* The ChaCha20-Poly1305 AEAD functions are declared in the library
 * source (src/lib/chacha20poly1305.c).  Provide extern references. */
extern int chacha20poly1305_encrypt(const void *key, const void *nonce,
                                    const void *aad, size_t aad_len,
                                    const void *plain, size_t plen,
                                    void *cipher, void *tag);
extern int chacha20poly1305_decrypt(const void *key, const void *nonce,
                                    const void *aad, size_t aad_len,
                                    const void *cipher, size_t clen,
                                    const void *tag, void *plain);

/* ── Internal AEAD context (per-cipher) ────────────────────────────── */

union tls_aead_cipher_ctx {
	struct aes_gcm_ctx gcm;
	/* ChaCha20-Poly1305 is stateless (keyed per-call) */
};

struct tls_aead_internal {
	uint16_t cipher_suite;
	int      key_len;
	int      tag_len;
	int      is_aead;
	union tls_aead_cipher_ctx cipher;
};

/* ── Initialisation ────────────────────────────────────────────────── */

int tls_aead_init(struct tls_aead_ctx *ctx,
                  const uint8_t *key, int key_len,
                  uint16_t cipher_suite)
{
	struct tls_aead_internal *intl;

	if (!ctx || !key)
		return -EINVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->cipher_suite = cipher_suite;
	ctx->enc_key_len  = key_len;
	memcpy(ctx->enc_key, key, (size_t)(key_len < 32 ? key_len : 32));

	/* The internal structure rides on top of the opaque ctx.
	 * We store the cipher-specific data after the public fields
	 * by casting ctx to our internal struct.  Since ctx is allocated
	 * by the caller (part of tls_cipher_state or stack), we need
	 * to ensure sufficient size.  For simplicity, we keep cipher
	 * state in a separate static table. */

	/* Determine tag length and AEAD properties from cipher suite */
	switch (cipher_suite) {
	case TLS_AES_128_GCM_SHA256:
	case TLS_AES_256_GCM_SHA384:
	case TLS_ECDHE_RSA_WITH_AES_128_GCM:
	case TLS_ECDHE_ECDSA_WITH_AES_128_GCM:
	case TLS_DHE_RSA_WITH_AES_128_GCM:
		ctx->tag_len   = 16;
		ctx->is_aead   = 1;
		break;

	case TLS_CHACHA20_POLY1305_SHA256:
	case TLS_ECDHE_RSA_WITH_CHACHA20:
		ctx->tag_len   = 16;
		ctx->is_aead   = 1;
		break;

	default:
		/* Non-AEAD cipher — not supported by this layer */
		return -EOPNOTSUPP;
	}

	return 0;
}

/* ── Nonce Construction Helper ─────────────────────────────────────── */

/* Build a per-record nonce from the cipher state.
 *
 * For TLS 1.2:
 *   nonce = fixed_iv (4 bytes) || explicit_iv (8 bytes)
 *
 * For TLS 1.3:
 *   nonce = (fixed_iv || explicit_iv) XOR (0x00000000 || seq_num_BE8)
 *
 * We treat the combined fixed_iv (4) + explicit_iv (8) as the base nonce.
 */
void tls_aead_build_nonce(const struct tls_cipher_state *cs,
                                 uint16_t version,
                                 uint8_t nonce[TLS_AEAD_NONCE_LEN])
{
	int i;

	/* Combine fixed_iv and explicit_iv as the base */
	memcpy(nonce, cs->fixed_iv, 4);
	memcpy(nonce + 4, cs->explicit_iv, 8);

	if (version >= TLS_VER_1_3) {
		/* TLS 1.3: XOR with sequence number (8-byte big-endian) */
		uint64_t seq = cs->seq_num;
		uint8_t seq_be[8];
		int j;
		for (j = 7; j >= 0; j--) {
			seq_be[j] = (uint8_t)(seq & 0xFF);
			seq >>= 8;
		}
		for (i = 4; i < 12; i++)
			nonce[i] ^= seq_be[i - 4];
	}
}

/* ── AEAD Encrypt ──────────────────────────────────────────────────── */

int tls_aead_encrypt(struct tls_aead_ctx *ctx,
                     const uint8_t *nonce,
                     const uint8_t *aad, int aad_len,
                     const uint8_t *plain, int plain_len,
                     uint8_t *cipher, uint8_t *tag)
{
	int ret;

	if (!ctx || !nonce || !cipher || !tag)
		return -EINVAL;
	if (plain_len < 0)
		return -EINVAL;
	if (plain_len > 0 && !plain)
		return -EINVAL;
	if (aad_len < 0)
		return -EINVAL;
	if (aad_len > 0 && !aad)
		return -EINVAL;

	switch (ctx->cipher_suite) {
	case TLS_AES_128_GCM_SHA256:
	case TLS_AES_256_GCM_SHA384:
	case TLS_ECDHE_RSA_WITH_AES_128_GCM:
	case TLS_ECDHE_ECDSA_WITH_AES_128_GCM:
	case TLS_DHE_RSA_WITH_AES_128_GCM:
	{
		struct aes_gcm_ctx gcm;
		int key_len;

		key_len = (ctx->cipher_suite == TLS_AES_256_GCM_SHA384) ? 32 : 16;

		ret = aes_gcm_init(&gcm, ctx->enc_key, key_len, ctx->tag_len);
		if (ret < 0)
			return ret;

		ret = aes_gcm_encrypt(&gcm, nonce, TLS_AEAD_NONCE_LEN,
		                      aad, aad_len,
		                      plain, plain_len,
		                      cipher, tag);
		return ret;
	}

	case TLS_CHACHA20_POLY1305_SHA256:
	case TLS_ECDHE_RSA_WITH_CHACHA20:
	{
		ret = chacha20poly1305_encrypt(ctx->enc_key, nonce,
		                                aad, (size_t)aad_len,
		                                plain, (size_t)plain_len,
		                                cipher, tag);
		if (ret < 0)
			return -EIO;
		return 0;
	}

	default:
		return -EOPNOTSUPP;
	}
}

/* ── AEAD Decrypt ──────────────────────────────────────────────────── */

int tls_aead_decrypt(struct tls_aead_ctx *ctx,
                     const uint8_t *nonce,
                     const uint8_t *aad, int aad_len,
                     const uint8_t *cipher, int cipher_len,
                     const uint8_t *tag, uint8_t *plain)
{
	int ret;

	if (!ctx || !nonce || !cipher || !tag || !plain)
		return -EINVAL;
	if (cipher_len < 0)
		return -EINVAL;
	if (aad_len < 0)
		return -EINVAL;
	if (aad_len > 0 && !aad)
		return -EINVAL;

	switch (ctx->cipher_suite) {
	case TLS_AES_128_GCM_SHA256:
	case TLS_AES_256_GCM_SHA384:
	case TLS_ECDHE_RSA_WITH_AES_128_GCM:
	case TLS_ECDHE_ECDSA_WITH_AES_128_GCM:
	case TLS_DHE_RSA_WITH_AES_128_GCM:
	{
		struct aes_gcm_ctx gcm;
		int key_len;

		key_len = (ctx->cipher_suite == TLS_AES_256_GCM_SHA384) ? 32 : 16;

		ret = aes_gcm_init(&gcm, ctx->enc_key, key_len, ctx->tag_len);
		if (ret < 0)
			return ret;

		ret = aes_gcm_decrypt(&gcm, nonce, TLS_AEAD_NONCE_LEN,
		                      aad, aad_len,
		                      cipher, cipher_len,
		                      tag, plain);
		return ret;
	}

	case TLS_CHACHA20_POLY1305_SHA256:
	case TLS_ECDHE_RSA_WITH_CHACHA20:
	{
		ret = chacha20poly1305_decrypt(ctx->enc_key, nonce,
		                                aad, (size_t)aad_len,
		                                cipher, (size_t)cipher_len,
		                                tag, plain);
		if (ret < 0)
			return -EBADMSG;
		return 0;
	}

	default:
		return -EOPNOTSUPP;
	}
}
