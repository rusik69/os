/* aes_gcm.c — AES-GCM AEAD mode (NIST SP 800-38D)
 *
 * Implements AES-GCM authenticated encryption:
 *   - AES-128-GCM and AES-256-GCM
 *   - 12-byte nonces (recommended IV length)
 *   - 16-byte authentication tags
 *
 * References:
 *   NIST SP 800-38D — Recommendation for Block Cipher Modes of Operation:
 *                     Galois/Counter Mode (GCM) and GMAC
 */

#include "aes_gcm.h"
#include "string.h"
#include "errno.h"
#include "aes.h"
#include "types.h"

/* ── GHASH: Universal Hash over GF(2^128) ──────────────────────────── */

/* The Galois field polynomial: x^128 + x^7 + x^2 + x + 1
 * For reduction: R = 0xe1 << 120 (big-endian byte representation) */

/* Multiply X by H in GF(2^128) — in-place on X */
static void gcm_mul(uint8_t x[16], const uint8_t h[16])
{
	uint8_t z[16] = {0};
	uint8_t v[16];
	int i;

	memcpy(v, h, 16);

	for (i = 0; i < 128; i++) {
		int byte_idx = i >> 3;       /* i / 8 */
		int bit     = 7 - (i & 7);  /* MSB first within byte */
		int j;

		if (x[byte_idx] & (1 << bit)) {
			for (j = 0; j < 16; j++)
				z[j] ^= v[j];
		}

		/* Shift v right by 1 bit (divide by α) */
		{
			uint8_t carry = 0;
			for (j = 15; j >= 0; j--) {
				uint8_t nc = v[j] & 1;
				v[j] = (v[j] >> 1) | (carry << 7);
				carry = nc;
			}
			/* If carry, XOR with reduction polynomial R */
			if (carry)
				v[0] ^= 0xe1;
		}
	}

	memcpy(x, z, 16);
}

/* Compute GHASH over input blocks.
 * 'in' contains the byte data, 'in_len' is its length in bytes.
 * 'h' is the hash key (AES_K(0^128)).
 * 'out' receives the 16-byte GHASH result. */
static void ghash(const uint8_t *in, int in_len,
                  const uint8_t h[16], uint8_t out[16])
{
	int i;

	memset(out, 0, 16);

	for (i = 0; i + 16 <= in_len; i += 16) {
		int j;
		for (j = 0; j < 16; j++)
			out[j] ^= in[i + j];
		gcm_mul(out, h);
	}

	/* Handle partial last block (zero-padded) */
	if (in_len % 16 != 0) {
		int rem = in_len % 16;
		int j;
		for (j = 0; j < rem; j++)
			out[j] ^= in[in_len - rem + j];
		gcm_mul(out, h);
	}
}

/* ── AES-CTR Mode (GCM variant — uses J0) ──────────────────────────── */

/* Encrypt plaintext using AES-CTR with initial counter block J0.
 * J0 is modified in place (counter increments). */
static void gcm_ctr(struct aes_gcm_ctx *ctx,
                    uint8_t J0[16],
                    const uint8_t *in, int in_len,
                    uint8_t *out)
{
	uint8_t ctr[16];
	uint8_t ks[16];
	int offset = 0;

	memcpy(ctr, J0, 16);

	while (offset < in_len) {
		int todo, j;

		/* Increment the counter: J0[12..15] big-endian */
		ctr[15]++;
		if (ctr[15] == 0)
			ctr[14]++;
		if (ctr[14] == 0)
			ctr[13]++;
		if (ctr[13] == 0)
			ctr[12]++;

		aes_encrypt_block(&ctx->aes, ctr, ks);

		todo = (in_len - offset < 16) ? (in_len - offset) : 16;
		for (j = 0; j < todo; j++)
			out[offset + j] = in[offset + j] ^ ks[j];
		offset += todo;
	}
}

/* ── Public API ────────────────────────────────────────────────────── */

int aes_gcm_init(struct aes_gcm_ctx *ctx,
                 const uint8_t *key, int key_len,
                 int tag_len)
{
	uint8_t zero_block[16] = {0};
	int ret;

	if (!ctx || !key)
		return -EINVAL;
	if (key_len != 16 && key_len != 32)
		return -EINVAL;
	if (tag_len < 0)
		return -EINVAL;

	ctx->key_len = key_len;
	ctx->tag_len = (tag_len == 0) ? AES_GCM_DEFAULT_TAG_LEN : tag_len;

	if (ctx->tag_len > AES_GCM_MAX_TAG_LEN)
		return -EINVAL;

	ret = aes_init(&ctx->aes, key, key_len);
	if (ret < 0)
		return ret;

	/* Compute H = AES_K(0^128) */
	aes_encrypt_block(&ctx->aes, zero_block, ctx->H);

	return 0;
}

int aes_gcm_encrypt(struct aes_gcm_ctx *ctx,
                    const uint8_t *iv, int iv_len,
                    const uint8_t *aad, int aad_len,
                    const uint8_t *plain, int plain_len,
                    uint8_t *cipher, uint8_t *tag)
{
	uint8_t J0[16];
	uint8_t mac_input[4096];
	int mac_len = 0;
	int aad_pad, ct_pad;
	int i;

	if (!ctx || !iv || !cipher || !tag)
		return -EINVAL;
	if (iv_len != AES_GCM_MIN_IV_LEN)
		return -EINVAL;
	if (plain_len < 0)
		return -EINVAL;
	if (plain_len > 0 && !plain)
		return -EINVAL;
	if (aad_len < 0)
		return -EINVAL;
	if (aad_len > 0 && !aad)
		return -EINVAL;

	/* Construct J0: IV || 0x00000001 (for 12-byte IV) */
	memset(J0, 0, 16);
	memcpy(J0, iv, iv_len);
	J0[15] = 1;

	/* Encrypt with AES-CTR starting from counter J0 + 1 */
	gcm_ctr(ctx, J0, plain, plain_len, cipher);

	/* Pad lengths */
	aad_pad = (16 - (aad_len % 16)) % 16;
	ct_pad  = (16 - (plain_len % 16)) % 16;

	/* Build GHASH input: AAD || pad(AAD) || C || pad(C) || len(AAD) || len(C) */
	if (aad_len > 0) {
		memcpy(mac_input + mac_len, aad, (size_t)aad_len);
		mac_len += aad_len;
	}
	/* AAD padding */
	for (i = 0; i < aad_pad; i++)
		mac_input[mac_len++] = 0;

	if (plain_len > 0) {
		memcpy(mac_input + mac_len, cipher, (size_t)plain_len);
		mac_len += plain_len;
	}
	/* Ciphertext padding */
	for (i = 0; i < ct_pad; i++)
		mac_input[mac_len++] = 0;

	/* Length block (64-bit big-endian each) */
	{
		uint64_t aad_bits = (uint64_t)aad_len * 8;
		uint64_t ct_bits  = (uint64_t)plain_len * 8;
		int j;
		for (j = 7; j >= 0; j--) {
			mac_input[mac_len + j]     = (uint8_t)(aad_bits & 0xFF);
			mac_input[mac_len + 8 + j] = (uint8_t)(ct_bits & 0xFF);
			aad_bits >>= 8;
			ct_bits  >>= 8;
		}
		mac_len += 16;
	}

	/* Compute GHASH */
	{
		uint8_t S[16];
		ghash(mac_input, mac_len, ctx->H, S);

		/* Reset J0 to original for tag computation */
		memset(J0, 0, 16);
		memcpy(J0, iv, iv_len);
		J0[15] = 1;

		/* T = GCTR(J0, S) */
		gcm_ctr(ctx, J0, S, 16, tag);
	}

	return 0;
}

int aes_gcm_decrypt(struct aes_gcm_ctx *ctx,
                    const uint8_t *iv, int iv_len,
                    const uint8_t *aad, int aad_len,
                    const uint8_t *cipher, int cipher_len,
                    const uint8_t *tag, uint8_t *plain)
{
	uint8_t J0[16];
	uint8_t mac_input[4096];
	int mac_len = 0;
	int aad_pad, ct_pad;
	uint8_t computed_tag[16];
	int i;

	if (!ctx || !iv || !cipher || !tag || !plain)
		return -EINVAL;
	if (iv_len != AES_GCM_MIN_IV_LEN)
		return -EINVAL;
	if (cipher_len < 0)
		return -EINVAL;
	if (aad_len < 0)
		return -EINVAL;
	if (aad_len > 0 && !aad)
		return -EINVAL;

	/* Construct J0 */
	memset(J0, 0, 16);
	memcpy(J0, iv, iv_len);
	J0[15] = 1;

	/* Pad lengths */
	aad_pad = (16 - (aad_len % 16)) % 16;
	ct_pad  = (16 - (cipher_len % 16)) % 16;

	/* Build GHASH input */
	if (aad_len > 0) {
		memcpy(mac_input + mac_len, aad, (size_t)aad_len);
		mac_len += aad_len;
	}
	for (i = 0; i < aad_pad; i++)
		mac_input[mac_len++] = 0;

	if (cipher_len > 0) {
		memcpy(mac_input + mac_len, cipher, (size_t)cipher_len);
		mac_len += cipher_len;
	}
	for (i = 0; i < ct_pad; i++)
		mac_input[mac_len++] = 0;

	/* Length block */
	{
		uint64_t aad_bits = (uint64_t)aad_len * 8;
		uint64_t ct_bits  = (uint64_t)cipher_len * 8;
		int j;
		for (j = 7; j >= 0; j--) {
			mac_input[mac_len + j]     = (uint8_t)(aad_bits & 0xFF);
			mac_input[mac_len + 8 + j] = (uint8_t)(ct_bits & 0xFF);
			aad_bits >>= 8;
			ct_bits  >>= 8;
		}
		mac_len += 16;
	}

	/* Verify GHASH first (before decryption — constant-time compare) */
	{
		uint8_t S[16];
		uint8_t diff = 0;

		ghash(mac_input, mac_len, ctx->H, S);

		memset(J0, 0, 16);
		memcpy(J0, iv, iv_len);
		J0[15] = 1;

		gcm_ctr(ctx, J0, S, 16, computed_tag);

		/* Constant-time comparison */
		for (i = 0; i < ctx->tag_len; i++)
			diff |= tag[i] ^ computed_tag[i];

		if (diff != 0)
			return -EBADMSG;
	}

	/* Decrypt */
	memset(J0, 0, 16);
	memcpy(J0, iv, iv_len);
	J0[15] = 1;

	gcm_ctr(ctx, J0, cipher, cipher_len, plain);

	return 0;
}
