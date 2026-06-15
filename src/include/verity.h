#ifndef VERITY_H
#define VERITY_H

#include "types.h"

/* ── fs-verity: Merkle tree per-file integrity verification ───────── */

/* SHA-256 digest size (32 bytes) */
#define FS_VERITY_HASH_SIZE 32

/* Maximum supported file size for fs-verity */
#define FS_VERITY_MAX_FILE_SIZE (256 * 1024 * 1024)  /* 256 MB */

/* Xattr name for storing fs-verity root hash */
#define FS_VERITY_XATTR_NAME "security.verity"

/* FS_IOC_ENABLE_VERITY ioctl number (Linux-compatible) */
/* _IOW('f', 133, struct fsverity_enable_arg) */
/* For our simpler struct: _IOW('f', 133, struct fsverity_enable_arg) */
#define FS_IOC_ENABLE_VERITY 0x40086685

/* fsverity enable argument structure (simplified) */
struct fsverity_enable_arg {
    uint64_t version;       /* must be 1 */
    uint64_t hash_algorithm; /* 0 = SHA-256 */
    uint64_t block_size;    /* must be 4096 */
    uint64_t salt_ptr;      /* pointer to salt (or 0) */
    uint64_t salt_size;     /* size of salt */
    uint64_t flags;         /* reserved, must be 0 */
};

/* Initialize the fs-verity subsystem */
void fsverity_init(void);

/* Enable fs-verity on a file given its path.
 * Builds the Merkle tree, stores root hash in security.verity xattr,
 * and registers the file for verification on future reads.
 * Returns 0 on success, negative errno on failure. */
int fsverity_enable_path(const char *path);

/* Enable fs-verity on a file (build Merkle tree, store root hash).
 * Once enabled, the file becomes read-only. */
int fsverity_enable(uint64_t ino, const uint8_t *data, uint64_t size,
                    uint8_t root_hash[FS_VERITY_HASH_SIZE]);

/* Verify a single data block against the Merkle tree.
 * Returns 0 if valid, -EIO on mismatch. */
int fsverity_verify_block(uint64_t ino, uint32_t block,
                          const uint8_t *data);

/* Verify a complete file against its Merkle tree.
 * Returns 0 if all blocks verify. */
int fsverity_verify_file(uint64_t ino, const uint8_t *data, uint64_t size);

/* Get the root hash for a file with fs-verity enabled. */
int fsverity_get_root_hash(uint64_t ino,
                           uint8_t root_hash[FS_VERITY_HASH_SIZE]);

/* Disable fs-verity on a file (free tree data). */
int fsverity_disable(uint64_t ino);

/* Check if a file has fs-verity enabled (by checking xattr) */
int fsverity_is_enabled(const char *path);

/* Handle FS_IOC_ENABLE_VERITY ioctl for a file.
 * path: full path to the file
 * arg: pointer to struct fsverity_enable_arg from userspace
 * Returns 0 on success, negative errno on failure. */
int fsverity_ioctl_enable(const char *path, void *arg);

#endif /* VERITY_H */
