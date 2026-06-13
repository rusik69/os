#ifndef VERITY_H
#define VERITY_H

#include "types.h"

/* ── fs-verity: Merkle tree per-file integrity verification ───────── */

/* SHA-256 digest size (32 bytes) */
#define FS_VERITY_HASH_SIZE 32

/* Initialize the fs-verity subsystem */
void fsverity_init(void);

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

#endif /* VERITY_H */
