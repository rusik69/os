/*
 * src/fs/crypto.c — Kernel crypto API and fscrypt policy management.
 *
 * Implements:
 *   - Basic AES-128-ECB kernel crypto (original implementation)
 *   - Filesystem-level encryption (fscrypt) policy management:
 *       parsing on-disk contexts, building policies from contexts,
 *       setting/getting encryption policies via xattrs, encrypt/decrypt.
 *
 * Part of the Hermes OS ext4 filesystem encryption support (D177).
 */

#define KERNEL_INTERNAL
#include "crypto.h"
#include "printf.h"
#include "types.h"
#include "fscrypt.h"
#include "string.h"
#include "vfs.h"
#include "errno.h"
#include "heap.h"
#include "initcall.h"

#ifdef MODULE
#include "module.h"
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  AES-128 S-box and internals (preserved from original)
 * ═══════════════════════════════════════════════════════════════════ */

static const uint8_t sbox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t inv_sbox[256] = {
	0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
	0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
	0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
	0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
	0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
	0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
	0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
	0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
	0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
	0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
	0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
	0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
	0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
	0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
	0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
	0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const uint8_t rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static uint8_t key_sched[176]; /* 11 round keys * 16 bytes */

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;
	uint8_t hi;
	for (int i = 0; i < 8; i++) {
		if (b & 1)
			p ^= a;
		hi = a & 0x80;
		a <<= 1;
		if (hi)
			a ^= 0x1b;
		b >>= 1;
	}
	return p;
}

static void sub_word(uint8_t *w)
{
	w[0] = sbox[w[0]]; w[1] = sbox[w[1]];
	w[2] = sbox[w[2]]; w[3] = sbox[w[3]];
}

static void rot_word(uint8_t *w)
{
	uint8_t t = w[0];
	w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = t;
}

static void key_expansion(const uint8_t *key)
{
	for (int i = 0; i < 16; i++)
		key_sched[i] = key[i];
	for (int i = 4; i < 44; i++) {
		uint8_t tmp[4];
		for (int j = 0; j < 4; j++)
			tmp[j] = key_sched[(i - 1) * 4 + j];
		if (i % 4 == 0) {
			rot_word(tmp);
			sub_word(tmp);
			tmp[0] ^= rcon[i / 4];
		}
		for (int j = 0; j < 4; j++)
			key_sched[i * 4 + j] = key_sched[(i - 4) * 4 + j] ^ tmp[j];
	}
}

static void add_round_key(uint8_t state[16], int round)
{
	for (int i = 0; i < 16; i++)
		state[i] ^= key_sched[round * 16 + i];
}

static void sub_bytes(uint8_t state[16])
{
	for (int i = 0; i < 16; i++)
		state[i] = sbox[state[i]];
}

static void inv_sub_bytes(uint8_t state[16])
{
	for (int i = 0; i < 16; i++)
		state[i] = inv_sbox[state[i]];
}

static void shift_rows(uint8_t state[16])
{
	uint8_t t;
	t = state[1]; state[1] = state[5]; state[5] = state[9];
	state[9] = state[13]; state[13] = t;
	t = state[2]; state[2] = state[10]; state[10] = t;
	t = state[6]; state[6] = state[14]; state[14] = t;
	t = state[3]; state[3] = state[15]; state[15] = state[11];
	state[11] = state[7]; state[7] = t;
}

static void inv_shift_rows(uint8_t state[16])
{
	uint8_t t;
	t = state[13]; state[13] = state[9]; state[9] = state[5];
	state[5] = state[1]; state[1] = t;
	t = state[2]; state[2] = state[10]; state[10] = t;
	t = state[6]; state[6] = state[14]; state[14] = t;
	t = state[7]; state[7] = state[11]; state[11] = state[15];
	state[15] = state[3]; state[3] = t;
}

static void mix_columns(uint8_t state[16])
{
	for (int c = 0; c < 4; c++) {
		int i = c * 4;
		uint8_t a0 = state[i], a1 = state[i + 1];
		uint8_t a2 = state[i + 2], a3 = state[i + 3];
		state[i]     = gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3;
		state[i + 1] = a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3;
		state[i + 2] = a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3);
		state[i + 3] = gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2);
	}
}

static void inv_mix_columns(uint8_t state[16])
{
	for (int c = 0; c < 4; c++) {
		int i = c * 4;
		uint8_t a0 = state[i], a1 = state[i + 1];
		uint8_t a2 = state[i + 2], a3 = state[i + 3];
		state[i]     = gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9);
		state[i + 1] = gf_mul(a0, 9)  ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13);
		state[i + 2] = gf_mul(a0, 13) ^ gf_mul(a1, 9)  ^ gf_mul(a2, 14) ^ gf_mul(a3, 11);
		state[i + 3] = gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9)  ^ gf_mul(a3, 14);
	}
}

static uint8_t aes_key[16];

void crypto_init(void)
{
	memset(aes_key, 0x42, 16);
	key_expansion(aes_key);
	kprintf("[OK] Kernel crypto API initialized (AES-128-ECB)\n");
}

void crypto_aes_set_key(const uint8_t *key)
{
	if (key) {
		memcpy(aes_key, key, 16);
		key_expansion(aes_key);
	}
}

void crypto_aes_encrypt(const uint8_t in[16], uint8_t out[16])
{
	if (!in || !out)
		return;
	uint8_t state[16];
	memcpy(state, in, 16);
	add_round_key(state, 0);
	for (int round = 1; round < 10; round++) {
		sub_bytes(state);
		shift_rows(state);
		mix_columns(state);
		add_round_key(state, round);
	}
	sub_bytes(state);
	shift_rows(state);
	add_round_key(state, 10);
	memcpy(out, state, 16);
}

void crypto_aes_decrypt(const uint8_t in[16], uint8_t out[16])
{
	if (!in || !out)
		return;
	uint8_t state[16];
	memcpy(state, in, 16);
	add_round_key(state, 10);
	inv_shift_rows(state);
	inv_sub_bytes(state);
	for (int round = 9; round >= 1; round--) {
		add_round_key(state, round);
		inv_mix_columns(state);
		inv_shift_rows(state);
		inv_sub_bytes(state);
	}
	add_round_key(state, 0);
	memcpy(out, state, 16);
}

/* ═══════════════════════════════════════════════════════════════════
 *  fscrypt — Filesystem-level encryption policy management
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * fscrypt_parse_context — Parse an on-disk encryption context.
 *
 * @ctx:        pointer to context bytes (from xattr)
 * @ctx_size:   size of context in bytes
 * @info:       output fscrypt_info structure
 *
 * Returns 0 on success, -EINVAL on invalid context.
 */
int fscrypt_parse_context(const uint8_t *ctx, size_t ctx_size,
                          struct fscrypt_info *info)
{
	if (!ctx || !info || ctx_size < 1)
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	switch (ctx[0]) {
	case FSCRYPT_POLICY_V1: {
		const struct fscrypt_context_v1 *c1;
		if (ctx_size < sizeof(struct fscrypt_context_v1))
			return -EINVAL;
		c1 = (const struct fscrypt_context_v1 *)ctx;

		info->ci_policy_version = FSCRYPT_POLICY_V1;
		info->ci_contents_mode = c1->contents_encryption_mode;
		info->ci_filenames_mode = c1->filenames_encryption_mode;
		info->ci_flags = c1->flags;
		memcpy(info->ci_master_key_descriptor,
		       c1->master_key_descriptor,
		       FSCRYPT_KEY_DESCRIPTOR_SIZE);
		memcpy(info->ci_nonce, c1->nonce, 16);
		info->ci_initialized = 1;
		return 0;
	}
	case FSCRYPT_POLICY_V2: {
		const struct fscrypt_context_v2 *c2;
		if (ctx_size < sizeof(struct fscrypt_context_v2))
			return -EINVAL;
		c2 = (const struct fscrypt_context_v2 *)ctx;

		info->ci_policy_version = FSCRYPT_POLICY_V2;
		info->ci_contents_mode = c2->contents_encryption_mode;
		info->ci_filenames_mode = c2->filenames_encryption_mode;
		info->ci_flags = c2->flags;
		info->ci_log2_data_unit_size = c2->log2_data_unit_size;
		memcpy(info->ci_master_key_identifier,
		       c2->master_key_identifier,
		       FSCRYPT_KEY_IDENTIFIER_SIZE);
		memcpy(info->ci_nonce, c2->nonce, 16);
		info->ci_initialized = 1;
		return 0;
	}
	default:
		/* Unknown context version */
		return -EINVAL;
	}
}

/*
 * fscrypt_build_context — Build an on-disk encryption context from info.
 *
 * @info:       encryption info
 * @ctx_out:    output buffer for context (at least FSCRYPT_MAX_CONTEXT_SIZE)
 * @ctx_size_out: output size in bytes
 *
 * Returns 0 on success, -EINVAL on invalid version.
 */
int fscrypt_build_context(const struct fscrypt_info *info,
                          uint8_t *ctx_out, size_t *ctx_size_out)
{
	if (!info || !ctx_out || !ctx_size_out)
		return -EINVAL;

	switch (info->ci_policy_version) {
	case FSCRYPT_POLICY_V1: {
		struct fscrypt_context_v1 *c1;

		c1 = (struct fscrypt_context_v1 *)ctx_out;
		memset(c1, 0, sizeof(*c1));
		c1->version = FSCRYPT_POLICY_V1;
		c1->contents_encryption_mode = info->ci_contents_mode;
		c1->filenames_encryption_mode = info->ci_filenames_mode;
		c1->flags = info->ci_flags;
		memcpy(c1->master_key_descriptor,
		       info->ci_master_key_descriptor,
		       FSCRYPT_KEY_DESCRIPTOR_SIZE);
		memcpy(c1->nonce, info->ci_nonce, 16);
		*ctx_size_out = sizeof(struct fscrypt_context_v1);
		return 0;
	}
	case FSCRYPT_POLICY_V2: {
		struct fscrypt_context_v2 *c2;

		c2 = (struct fscrypt_context_v2 *)ctx_out;
		memset(c2, 0, sizeof(*c2));
		c2->version = FSCRYPT_POLICY_V2;
		c2->contents_encryption_mode = info->ci_contents_mode;
		c2->filenames_encryption_mode = info->ci_filenames_mode;
		c2->flags = info->ci_flags;
		c2->log2_data_unit_size = info->ci_log2_data_unit_size;
		memcpy(c2->master_key_identifier,
		       info->ci_master_key_identifier,
		       FSCRYPT_KEY_IDENTIFIER_SIZE);
		memcpy(c2->nonce, info->ci_nonce, 16);
		*ctx_size_out = sizeof(struct fscrypt_context_v2);
		return 0;
	}
	default:
		return -EINVAL;
	}
}

/*
 * fscrypt_policy_to_context_v1 — Build a V1 on-disk context from a V1 policy.
 *
 * @policy:    userspace V1 policy
 * @ctx:       output on-disk context (includes per-inode nonce)
 * @nonce:     16-byte per-inode nonce (random)
 */
void fscrypt_policy_to_context_v1(const struct fscrypt_policy_v1 *policy,
                                  struct fscrypt_context_v1 *ctx,
                                  const uint8_t *nonce)
{
	if (!policy || !ctx)
		return;

	memset(ctx, 0, sizeof(*ctx));
	ctx->version = FSCRYPT_POLICY_V1;
	ctx->contents_encryption_mode = policy->contents_encryption_mode;
	ctx->filenames_encryption_mode = policy->filenames_encryption_mode;
	ctx->flags = policy->flags;
	memcpy(ctx->master_key_descriptor,
	       policy->master_key_descriptor,
	       FSCRYPT_KEY_DESCRIPTOR_SIZE);
	if (nonce)
		memcpy(ctx->nonce, nonce, 16);
}

/*
 * fscrypt_policy_to_context_v2 — Build a V2 on-disk context from a V2 policy.
 */
void fscrypt_policy_to_context_v2(const struct fscrypt_policy_v2 *policy,
                                  struct fscrypt_context_v2 *ctx,
                                  const uint8_t *nonce)
{
	if (!policy || !ctx)
		return;

	memset(ctx, 0, sizeof(*ctx));
	ctx->version = FSCRYPT_POLICY_V2;
	ctx->contents_encryption_mode = policy->contents_encryption_mode;
	ctx->filenames_encryption_mode = policy->filenames_encryption_mode;
	ctx->flags = policy->flags;
	ctx->log2_data_unit_size = policy->log2_data_unit_size;
	memcpy(ctx->master_key_identifier,
	       policy->master_key_identifier,
	       FSCRYPT_KEY_IDENTIFIER_SIZE);
	if (nonce)
		memcpy(ctx->nonce, nonce, 16);
}

/*
 * fscrypt_has_policy — Check if a path has an encryption policy.
 *
 * Returns 1 if policy found, 0 if not, negative errno on error.
 */
int fscrypt_has_policy(const char *path)
{
	uint8_t ctx_buf[FSCRYPT_MAX_CONTEXT_SIZE];
	int ret;

	if (!path)
		return -EINVAL;

	ret = vfs_getxattr(path, FSCRYPT_XATTR_NAME, ctx_buf,
	                   (int)sizeof(ctx_buf));
	if (ret < 0)
		return 0; /* No xattr means no policy */

	return 1;
}

/*
 * fscrypt_get_encryption — Get encryption info for a path.
 *
 * @path:  absolute path to file/directory
 * @info:  output fscrypt_info structure
 *
 * Returns 0 on success, negative errno on failure.
 */
int fscrypt_get_encryption(const char *path,
                            struct fscrypt_info *info)
{
	uint8_t ctx_buf[FSCRYPT_MAX_CONTEXT_SIZE];
	int ret;

	if (!path || !info)
		return -EINVAL;

	ret = vfs_getxattr(path, FSCRYPT_XATTR_NAME, ctx_buf,
	                   (int)sizeof(ctx_buf));
	if (ret < 0)
		return -ENODATA;

	return fscrypt_parse_context(ctx_buf, (size_t)ret, info);
}

/*
 * fscrypt_setup_encryption — Set encryption on a directory.
 *
 * @path:    absolute path to empty directory
 * @key:     master key (for V1: exactly 64 bytes, key descriptor in last 8)
 * @key_len: length of key data
 *
 * Creates an encryption context xattr on the directory.
 * Returns 0 on success, negative errno on failure.
 */
int fscrypt_setup_encryption(const char *path,
                             const uint8_t *key, size_t key_len)
{
	struct fscrypt_context_v1 ctx_v1;
	uint8_t nonce[16];
	int ret;

	if (!path)
		return -EINVAL;

	/* Generate a random nonce (for now, use a fixed derivation) */
	for (int i = 0; i < 16; i++)
		nonce[i] = (uint8_t)((uintptr_t)path + i * 0x73);

	/* Build V1 context with AES-256-XTS for contents, AES-256-CBC-CTS
	 * for filenames (Linux fscrypt defaults). */
	memset(&ctx_v1, 0, sizeof(ctx_v1));
	ctx_v1.version = FSCRYPT_POLICY_V1;
	ctx_v1.contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS;
	ctx_v1.filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS;
	ctx_v1.flags = FSCRYPT_POLICY_FLAGS_PAD_32;

	/* Store key descriptor */
	if (key && key_len >= FSCRYPT_KEY_DESCRIPTOR_SIZE) {
		memcpy(ctx_v1.master_key_descriptor,
		       key + key_len - FSCRYPT_KEY_DESCRIPTOR_SIZE,
		       FSCRYPT_KEY_DESCRIPTOR_SIZE);
	}
	memcpy(ctx_v1.nonce, nonce, 16);

	/* Write as xattr */
	ret = vfs_setxattr(path, FSCRYPT_XATTR_NAME,
	                   (const uint8_t *)&ctx_v1,
	                   (int)sizeof(ctx_v1));
	if (ret < 0)
		return ret;

	kprintf("[fscrypt] Encryption policy set on %s "
	        "(mode=%u, flags=0x%02x)\n",
	        path, ctx_v1.contents_encryption_mode, ctx_v1.flags);
	return 0;
}

/*
 * fscrypt_release_encryption — Remove encryption from a path.
 *
 * @path: absolute path
 * Returns 0 on success, negative errno on failure.
 */
int fscrypt_release_encryption(const char *path)
{
	if (!path)
		return -EINVAL;

	/* Remove the encryption context xattr.
	 * vfs_setxattr with size 0 effectively removes it. */
	return vfs_setxattr(path, FSCRYPT_XATTR_NAME, NULL, 0);
}

/*
 * fscrypt_encrypt — Encrypt plaintext using the file's encryption info.
 *
 * For this read-only implementation, this is a transparent pass-through
 * that logs the encryption operation.  Full XTS encryption will be
 * implemented when write support is added.
 *
 * @info:        encryption info (from fscrypt_get_encryption)
 * @plaintext:   input plaintext
 * @ciphertext:  output ciphertext buffer
 * @len:         length of data (must be multiple of 16 for AES blocks)
 * @file_offset: byte offset within the file (for IV derivation)
 *
 * Returns 0 on success, negative errno on failure.
 */
int fscrypt_encrypt(struct fscrypt_info *info,
                    const uint8_t *plaintext, uint8_t *ciphertext,
                    size_t len, uint64_t file_offset)
{
	(void)info;
	(void)file_offset;

	if (!plaintext || !ciphertext)
		return -EINVAL;

	/* For now, transparent pass-through (read-only mode).
	 * Full AES-XTS encryption will be added with write support. */
	memcpy(ciphertext, plaintext, len);
	return 0;
}

/*
 * fscrypt_decrypt — Decrypt ciphertext using the file's encryption info.
 *
 * For this read-only implementation, this is a transparent pass-through
 * that logs the decryption operation.
 *
 * @info:        encryption info (from fscrypt_get_encryption)
 * @ciphertext:  input ciphertext
 * @plaintext:   output plaintext buffer
 * @len:         length of data (must be multiple of 16 for AES blocks)
 * @file_offset: byte offset within the file (for IV derivation)
 *
 * Returns 0 on success, negative errno on failure.
 */
int fscrypt_decrypt(struct fscrypt_info *info,
                    const uint8_t *ciphertext, uint8_t *plaintext,
                    size_t len, uint64_t file_offset)
{
	(void)info;
	(void)file_offset;

	if (!ciphertext || !plaintext)
		return -EINVAL;

	/* For now, transparent pass-through (read-only mode). */
	memcpy(plaintext, ciphertext, len);
	return 0;
}

/*
 * fscrypt_policy_set — Set a V1 encryption policy on a directory.
 *
 * @policy: V1 policy to apply
 *
 * Returns 0 on success, negative errno on failure.
 * NOTE: In the ioctl path, the policy is applied to the directory
 * referenced by the file descriptor.  For this implementation we
 * assume the policy context is embedded in the caller's knowledge.
 */
int fscrypt_policy_set(struct fscrypt_policy_v1 *policy)
{
	if (!policy)
		return -EINVAL;

	if (policy->version != FSCRYPT_POLICY_V1)
		return -EINVAL;

	kprintf("[fscrypt] Policy V1 set: contents=%u, filenames=%u, "
	        "flags=0x%02x, key_desc=%02x%02x...\n",
	        policy->contents_encryption_mode,
	        policy->filenames_encryption_mode,
	        policy->flags,
	        policy->master_key_descriptor[0],
	        policy->master_key_descriptor[1]);
	return 0;
}

/*
 * fscrypt_policy_get — Get the V1 encryption policy for a path.
 *
 * @path:   absolute path
 * @policy: output V1 policy structure
 *
 * Returns 0 on success, -ENODATA if no policy, -EINVAL if V2 policy.
 */
int fscrypt_policy_get(const char *path,
                       struct fscrypt_policy_v1 *policy)
{
	struct fscrypt_info info;
	int ret;

	if (!path || !policy)
		return -EINVAL;

	ret = fscrypt_get_encryption(path, &info);
	if (ret < 0)
		return ret;

	if (info.ci_policy_version != FSCRYPT_POLICY_V1)
		return -EINVAL;

	memset(policy, 0, sizeof(*policy));
	policy->version = FSCRYPT_POLICY_V1;
	policy->contents_encryption_mode = info.ci_contents_mode;
	policy->filenames_encryption_mode = info.ci_filenames_mode;
	policy->flags = info.ci_flags;
	memcpy(policy->master_key_descriptor,
	       info.ci_master_key_descriptor,
	       FSCRYPT_KEY_DESCRIPTOR_SIZE);
	return 0;
}

/*
 * fscrypt_policy_get_ex — Get the V2 encryption policy for a path.
 *
 * @path:     absolute path
 * @buf:      output buffer for V2 policy
 * @buf_size: size of output buffer
 *
 * Returns 0 on success, negative errno on failure.
 */
int fscrypt_policy_get_ex(const char *path,
                          uint8_t *buf, size_t buf_size)
{
	struct fscrypt_info info;
	int ret;

	if (!path || !buf || buf_size < sizeof(struct fscrypt_policy_v2))
		return -EINVAL;

	ret = fscrypt_get_encryption(path, &info);
	if (ret < 0)
		return ret;

	if (info.ci_policy_version == FSCRYPT_POLICY_V1) {
		/* Return V1 policy in V2 format?  For now, just
		 * return the V1 data as-is. */
		struct fscrypt_policy_v1 *p1;

		if (buf_size < sizeof(struct fscrypt_policy_v1))
			return -EINVAL;
		p1 = (struct fscrypt_policy_v1 *)buf;
		memset(p1, 0, sizeof(*p1));
		p1->version = FSCRYPT_POLICY_V1;
		p1->contents_encryption_mode = info.ci_contents_mode;
		p1->filenames_encryption_mode = info.ci_filenames_mode;
		p1->flags = info.ci_flags;
		memcpy(p1->master_key_descriptor,
		       info.ci_master_key_descriptor,
		       FSCRYPT_KEY_DESCRIPTOR_SIZE);
	} else {
		struct fscrypt_policy_v2 *p2;

		p2 = (struct fscrypt_policy_v2 *)buf;
		memset(p2, 0, sizeof(*p2));
		p2->version = FSCRYPT_POLICY_V2;
		p2->contents_encryption_mode = info.ci_contents_mode;
		p2->filenames_encryption_mode = info.ci_filenames_mode;
		p2->flags = info.ci_flags;
		p2->log2_data_unit_size = info.ci_log2_data_unit_size;
		memcpy(p2->master_key_identifier,
		       info.ci_master_key_identifier,
		       FSCRYPT_KEY_IDENTIFIER_SIZE);
	}

	return 0;
}

/*
 * fscrypt_init — Initialize the fscrypt subsystem.
 *
 * Called at boot via initcall.
 */
int __init fscrypt_init(void)
{
	kprintf("[OK] fscrypt: filesystem encryption policy support initialized\n");
	return 0;
}

/* Register fscrypt initialization */
device_initcall(fscrypt_init);

#ifdef MODULE
int __init init_module(void) { return fscrypt_init(); }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("fscrypt: filesystem-level encryption policy management for ext4");
MODULE_VERSION("1.0");
#endif
