#ifndef FSCRYPT_H
#define FSCRYPT_H

#include "types.h"

/*
 * fscrypt.h — Filesystem-level encryption policy definitions.
 *
 * Implements the Linux fscrypt API for per-directory encryption policies
 * compatible with ext4 encryption (EXT4_FEATURE_INCOMPAT_ENCRYPT).
 *
 * References:
 *   - Linux include/uapi/linux/fscrypt.h
 *   - Linux fs/crypto/policy.c
 */

/* ── Encryption modes ──────────────────────────────────────────────── */

#define FSCRYPT_MODE_AES_256_XTS       1
#define FSCRYPT_MODE_AES_256_CTS       4
#define FSCRYPT_MODE_AES_128_CBC       5
#define FSCRYPT_MODE_AES_128_CTS       6
#define FSCRYPT_MODE_ADIANTUM          9

/* ── Policy versions ───────────────────────────────────────────────── */

#define FSCRYPT_POLICY_V1              0
#define FSCRYPT_POLICY_V2              2

/* ── Policy flags ──────────────────────────────────────────────────── */

#define FSCRYPT_POLICY_FLAGS_PAD_4     0x00
#define FSCRYPT_POLICY_FLAGS_PAD_8     0x01
#define FSCRYPT_POLICY_FLAGS_PAD_16    0x02
#define FSCRYPT_POLICY_FLAGS_PAD_32    0x03
#define FSCRYPT_POLICY_FLAGS_PAD_MASK  0x03
#define FSCRYPT_POLICY_FLAG_DIRECT_KEY 0x04
#define FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64 0x08
#define FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32 0x10

/* ── Key identifier sizes ──────────────────────────────────────────── */

#define FSCRYPT_KEY_DESCRIPTOR_SIZE    8
#define FSCRYPT_KEY_IDENTIFIER_SIZE    16

/* ── Maximum policy size (on-disk context) ─────────────────────────── */

#define FSCRYPT_MAX_CONTEXT_SIZE       32

/* ── Encryption context (stored as inode xattr) ────────────────────── */

/*
 * On-disk encryption context stored as an extended attribute
 * ("encryption.ctx" or in the inode's i_crypt_info).
 * V1 context: master_key_descriptor (8 bytes)
 * V2 context: master_key_identifier (16 bytes)
 */

/* V1 encryption context (stored in xattr) */
struct fscrypt_context_v1 {
	uint8_t  version;          /* FSCRYPT_POLICY_V1 */
	uint8_t  contents_encryption_mode;
	uint8_t  filenames_encryption_mode;
	uint8_t  flags;
	uint8_t  master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
	uint8_t  nonce[16];        /* per-inode nonce */
} __attribute__((packed));

/* V2 encryption context (stored in xattr) */
struct fscrypt_context_v2 {
	uint8_t  version;          /* FSCRYPT_POLICY_V2 */
	uint8_t  contents_encryption_mode;
	uint8_t  filenames_encryption_mode;
	uint8_t  flags;
	uint8_t  log2_data_unit_size;
	uint8_t  master_key_identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
	uint8_t  nonce[16];        /* per-inode nonce */
} __attribute__((packed));

/* ── Policy structures (userspace ABI) ─────────────────────────────── */

/* V1 policy — set/get via FS_IOC_SET/GET_ENCRYPTION_POLICY */
struct fscrypt_policy_v1 {
	uint8_t  version;          /* FSCRYPT_POLICY_V1 */
	uint8_t  contents_encryption_mode;
	uint8_t  filenames_encryption_mode;
	uint8_t  flags;
	uint8_t  master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
} __attribute__((packed));

/* V2 policy — set/get via FS_IOC_SET/GET_ENCRYPTION_POLICY_EX */
struct fscrypt_policy_v2 {
	uint8_t  version;          /* FSCRYPT_POLICY_V2 */
	uint8_t  contents_encryption_mode;
	uint8_t  filenames_encryption_mode;
	uint8_t  flags;
	uint8_t  log2_data_unit_size;
	uint8_t  master_key_identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
} __attribute__((packed));

/* ── In-memory encryption info (per-inode) ─────────────────────────── */

/* Per-inode fscrypt information (in-memory cache of encryption state) */
struct fscrypt_info {
	int        ci_policy_version;
	uint8_t    ci_contents_mode;
	uint8_t    ci_filenames_mode;
	uint8_t    ci_flags;
	uint8_t    ci_log2_data_unit_size;
	uint8_t    ci_master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
	uint8_t    ci_master_key_identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
	uint8_t    ci_nonce[16];
	int        ci_initialized;  /* 1 = info is populated */
};

/* ── IOCTL commands (placed here for ext4 dispatch) ────────────────── */

/*
 * fscrypt ioctl numbers.
 * These match the Linux UAPI values so ext4 can dispatch FS_IOC_*
 * through the ioctl VFS path.
 */
#define FS_IOC_SET_ENCRYPTION_POLICY        _IOR('f', 19, struct fscrypt_policy_v1)
#define FS_IOC_GET_ENCRYPTION_POLICY        _IOW('f', 20, struct fscrypt_policy_v1)
#define FS_IOC_GET_ENCRYPTION_POLICY_EX     _IOW('f', 22, uint8_t[256])

/* ── Inode flag for encryption ─────────────────────────────────────── */

/* This flag is set on an inode when it uses fscrypt */
#define EXT4_ENCRYPT_FL             0x00000800  /* ext4 inode flag for encryption */

/* ── xattr name for encryption context ─────────────────────────────── */

#define FSCRYPT_XATTR_NAME          "encryption.ctx"

/* ── Public API ────────────────────────────────────────────────────── */

/* Encrypt/decrypt file contents using the inode's encryption policy */
int fscrypt_encrypt(struct fscrypt_info *info,
                    const uint8_t *plaintext, uint8_t *ciphertext,
                    size_t len, uint64_t file_offset);
int fscrypt_decrypt(struct fscrypt_info *info,
                    const uint8_t *ciphertext, uint8_t *plaintext,
                    size_t len, uint64_t file_offset);

/* Parse an on-disk encryption context into in-memory fscrypt_info */
int fscrypt_parse_context(const uint8_t *ctx, size_t ctx_size,
                          struct fscrypt_info *info);

/* Set/get encryption policy (ioctl-backed operations) */
int fscrypt_policy_set(struct fscrypt_policy_v1 *policy);
int fscrypt_policy_get(const char *path, struct fscrypt_policy_v1 *policy);
int fscrypt_policy_get_ex(const char *path, uint8_t *buf, size_t buf_size);

/* Setup / teardown per-inode encryption state */
int fscrypt_setup_encryption(const char *path,
                             const uint8_t *key, size_t key_len);
int fscrypt_get_encryption(const char *path,
                           struct fscrypt_info *info);
int fscrypt_release_encryption(const char *path);

/* Check if a path has an encryption policy set */
int fscrypt_has_policy(const char *path);

/* Format the encryption context from an info structure */
int fscrypt_build_context(const struct fscrypt_info *info,
                          uint8_t *ctx_out, size_t *ctx_size_out);

/* Convert policy v1 to encryption context v1 */
void fscrypt_policy_to_context_v1(const struct fscrypt_policy_v1 *policy,
                                  struct fscrypt_context_v1 *ctx,
                                  const uint8_t *nonce);

/* Convert policy v2 to encryption context v2 */
void fscrypt_policy_to_context_v2(const struct fscrypt_policy_v2 *policy,
                                  struct fscrypt_context_v2 *ctx,
                                  const uint8_t *nonce);

/* Initialize the fscrypt subsystem */
int fscrypt_init(void);

#endif /* FSCRYPT_H */
