/*
 * verity.c — fs-verity: Merkle tree per-file integrity verification
 *
 * Implements fs-verity, a mechanism for on-read Merkle tree integrity
 * verification of file contents.  When fs-verity is enabled on a file,
 * every read is verified against a pre-computed Merkle hash tree before
 * being returned to the caller.
 *
 * Key concepts:
 *   - Each file has an associated Merkle tree stored in a dedicated
 *     verity descriptor (separate from file data).
 *   - The Merkle tree is built with PAGE_SIZE (4096) data blocks and
 *     SHA-256 (32-byte) hashes.
 *   - The root hash is stored in the verity descriptor and serves as
 *     the trust anchor.
 *   - Once enabled, the file becomes read-only (writes are rejected).
 *   - On each read, the data blocks are verified against the tree.
 *
 * Item 456: fs-verity — Merkle tree per-file verification
 */

#define KERNEL_INTERNAL
#include "verity.h"
#include "sha256.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "export.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define VERITY_MAX_FILES     32    /* max files with fs-verity enabled */
#define VERITY_BLOCK_SIZE    4096  /* data block size (must match PAGE_SIZE) */
#define VERITY_HASH_SIZE     32    /* SHA-256 digest size */

/* ── Verity descriptor (per-file) ──────────────────────────────────── */

struct verity_descriptor {
    int      in_use;
    uint64_t ino;               /* inode number of the verified file */
    uint8_t  root_hash[VERITY_HASH_SIZE]; /* trusted root hash */
    uint32_t data_blocks;       /* total number of data blocks */
    uint32_t tree_levels;       /* number of levels in the Merkle tree */
    int      verified;          /* 1 = verification enabled */
    uint8_t *tree_data;         /* allocated: whole Merkle tree */
    uint64_t tree_size;         /* total size of the tree in bytes */
};

/* ── Global state ─────────────────────────────────────────────────── */

static struct verity_descriptor g_verity_files[VERITY_MAX_FILES];
static int g_verity_initialized = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════ */

/* Compute the number of levels in a Merkle tree given data blocks. */
static uint32_t verity_calc_levels(uint64_t num_data_blocks)
{
    uint32_t levels = 0;
    uint64_t blocks_at_level = num_data_blocks;
    uint32_t hashes_per_block = VERITY_BLOCK_SIZE / VERITY_HASH_SIZE;

    while (blocks_at_level > 1) {
        blocks_at_level = (blocks_at_level + hashes_per_block - 1) / hashes_per_block;
        levels++;
    }
    return levels;
}

/* Compute the total size of the Merkle tree (all levels). */
static uint64_t verity_calc_tree_size(uint64_t num_data_blocks, uint32_t *out_levels)
{
    uint64_t total = 0;
    uint64_t blocks_at_level = num_data_blocks;
    uint32_t hashes_per_block = VERITY_BLOCK_SIZE / VERITY_HASH_SIZE;
    uint32_t levels = 0;

    while (blocks_at_level > 1) {
        blocks_at_level = (blocks_at_level + hashes_per_block - 1) / hashes_per_block;
        total += blocks_at_level * VERITY_BLOCK_SIZE;
        levels++;
    }

    if (out_levels) *out_levels = levels;
    return total;
}

/* Find the verity descriptor for an inode. */
static struct verity_descriptor *verity_find(uint64_t ino)
{
    for (int i = 0; i < VERITY_MAX_FILES; i++) {
        if (g_verity_files[i].in_use && g_verity_files[i].ino == ino)
            return &g_verity_files[i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize the fs-verity subsystem. */
void fsverity_init(void)
{
    if (g_verity_initialized) return;

    memset(g_verity_files, 0, sizeof(g_verity_files));
    g_verity_initialized = 1;
    kprintf("[OK] fs-verity initialized (Merkle tree per-file verification)\n");
}
EXPORT_SYMBOL(fsverity_init);

/* Enable fs-verity on a file.
 *
 * Builds the Merkle hash tree from the file's data, computes the root
 * hash, and stores the tree for future verification.  Once enabled, the
 * file becomes read-only.
 *
 * @ino:   inode number of the file to protect
 * @data:  pointer to the file's data (all blocks)
 * @size:  total file size in bytes
 * @root_hash: output: 32-byte root hash
 *
 * Returns 0 on success, negative errno on failure.
 */
int fsverity_enable(uint64_t ino, const uint8_t *data, uint64_t size,
                    uint8_t root_hash[VERITY_HASH_SIZE])
{
    if (!g_verity_initialized || !data || !root_hash)
        return -EINVAL;

    if (verity_find(ino))
        return -EEXIST; /* already enabled */

    /* Find a free descriptor slot */
    int slot = -1;
    for (int i = 0; i < VERITY_MAX_FILES; i++) {
        if (!g_verity_files[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOSPC;

    /* Compute number of data blocks */
    uint32_t num_data_blocks = (uint32_t)((size + VERITY_BLOCK_SIZE - 1) / VERITY_BLOCK_SIZE);
    if (num_data_blocks == 0) return -EINVAL;

    /* Calculate tree size and allocate */
    uint32_t levels;
    uint64_t tree_size = verity_calc_tree_size(num_data_blocks, &levels);
    uint8_t *tree = (uint8_t *)kmalloc(tree_size);
    if (!tree) return -ENOMEM;
    memset(tree, 0, tree_size);

    /* Build the Merkle tree bottom-up */
    uint32_t hashes_per_block = VERITY_BLOCK_SIZE / VERITY_HASH_SIZE;
    uint32_t blocks_at_current = num_data_blocks;
    uint64_t tree_offset = 0;

    /* Level 0: hash each data block */
    for (uint32_t b = 0; b < blocks_at_current; b++) {
        uint64_t data_offset = (uint64_t)b * VERITY_BLOCK_SIZE;
        uint32_t data_len = VERITY_BLOCK_SIZE;
        if (data_offset + data_len > size)
            data_len = (uint32_t)(size - data_offset);

        uint8_t block_buf[VERITY_BLOCK_SIZE];
        memset(block_buf, 0, VERITY_BLOCK_SIZE);
        if (data_len > 0)
            memcpy(block_buf, data + data_offset, data_len);

        uint32_t hash_idx = b % hashes_per_block;
        uint8_t *hash_slot = tree + tree_offset + (uint64_t)hash_idx * VERITY_HASH_SIZE;
        sha256_hash(hash_slot, block_buf, (size_t)data_len > 0 ? (size_t)data_len : 1);
    }

    tree_offset += blocks_at_current * VERITY_HASH_SIZE;
    /* Align to block boundary */
    tree_offset = (tree_offset + VERITY_BLOCK_SIZE - 1) & ~(uint64_t)(VERITY_BLOCK_SIZE - 1);

    /* Upper levels: hash the hash blocks */
    uint32_t prev_level_blocks = blocks_at_current;
    while (prev_level_blocks > 1) {
        uint32_t hash_blocks = (prev_level_blocks + hashes_per_block - 1) / hashes_per_block;

        for (uint32_t hb = 0; hb < hash_blocks; hb++) {
            uint8_t hash_block[VERITY_BLOCK_SIZE];
            memset(hash_block, 0, VERITY_BLOCK_SIZE);

            for (uint32_t h = 0; h < hashes_per_block; h++) {
                uint32_t src_idx = hb * hashes_per_block + h;
                if (src_idx >= prev_level_blocks) break;

                /* Get the hash from the previous level */
                uint64_t src_offset;
                if (hb == 0 && tree_offset > 0) {
                    /* Previous level starts before tree_offset */
                    src_offset = tree_offset - (uint64_t)prev_level_blocks * VERITY_HASH_SIZE;
                } else {
                    src_offset = tree_offset - (uint64_t)prev_level_blocks * VERITY_HASH_SIZE
                                + (uint64_t)src_idx * VERITY_HASH_SIZE;
                }

                /* Actually, let's simplify: we read from the previous level position */
                uint8_t hash_buf[VERITY_HASH_SIZE];
                memcpy(hash_buf, tree + src_offset, VERITY_HASH_SIZE);
                memcpy(hash_block + (uint64_t)h * VERITY_HASH_SIZE, hash_buf, VERITY_HASH_SIZE);
            }

            sha256_hash(tree + tree_offset + (uint64_t)hb * VERITY_HASH_SIZE,
                        hash_block, VERITY_BLOCK_SIZE);
        }

        prev_level_blocks = hash_blocks;
    }

    /* The last hash is the root hash */
    if (tree_size >= VERITY_HASH_SIZE)
        memcpy(root_hash, tree + tree_size - VERITY_HASH_SIZE, VERITY_HASH_SIZE);

    /* Fill in descriptor */
    struct verity_descriptor *vd = &g_verity_files[slot];
    vd->in_use = 1;
    vd->ino = ino;
    memcpy(vd->root_hash, root_hash, VERITY_HASH_SIZE);
    vd->data_blocks = num_data_blocks;
    vd->tree_levels = levels;
    vd->verified = 1;
    vd->tree_data = tree;
    vd->tree_size = tree_size;

    kprintf("[fs-verity] enabled on ino=%llu (%u blocks, %u levels, root=",
            (unsigned long long)ino, num_data_blocks, levels);
    for (int i = 0; i < VERITY_HASH_SIZE; i++)
        kprintf("%02x", root_hash[i]);
    kprintf(")\n");

    return 0;
}
EXPORT_SYMBOL(fsverity_enable);

/* Verify a single data block against the Merkle tree.
 *
 * @ino:   inode number
 * @block: data block index (0-based)
 * @data:  pointer to the data block (must be VERITY_BLOCK_SIZE bytes)
 *
 * Returns 0 if the block is valid, -EIO if verification fails.
 */
int fsverity_verify_block(uint64_t ino, uint32_t block, const uint8_t *data)
{
    struct verity_descriptor *vd = verity_find(ino);
    if (!vd || !vd->verified)
        return -ENOENT; /* verity not enabled on this file */

    if (block >= vd->data_blocks)
        return -EINVAL;

    uint32_t hashes_per_block = VERITY_BLOCK_SIZE / VERITY_HASH_SIZE;
    uint32_t current_block = block;
    uint8_t current_hash[VERITY_HASH_SIZE];
    uint8_t block_buf[VERITY_BLOCK_SIZE];
    uint32_t levels_verified = 0;

    /* Hash the data block */
    memset(block_buf, 0, VERITY_BLOCK_SIZE);
    memcpy(block_buf, data, VERITY_BLOCK_SIZE);
    sha256_hash(current_hash, block_buf, VERITY_BLOCK_SIZE);

    /* Walk up the tree */
    while (1) {
        /* Find the hash block containing the current hash at this level */
        uint32_t hash_block_idx = current_block / hashes_per_block;
        uint32_t hash_in_block = current_block % hashes_per_block;

        /* Offset to the hash block in the tree */
        uint64_t hash_block_offset = 0;
        uint32_t blocks_before = 0;
        uint32_t blocks_at_level = vd->data_blocks;

        for (uint32_t l = 0; l <= levels_verified; l++) {
            if (l == levels_verified)
                break;
            uint32_t next = (blocks_at_level + hashes_per_block - 1) / hashes_per_block;
            uint32_t hash_blocks_at_level = (blocks_at_level + hashes_per_block - 1) / hashes_per_block;
            uint64_t level_size = (uint64_t)blocks_at_level * VERITY_HASH_SIZE;
            /* Align to block */
            level_size = (level_size + VERITY_BLOCK_SIZE - 1) & ~(uint64_t)(VERITY_BLOCK_SIZE - 1);
            hash_block_offset += level_size;
            blocks_before = blocks_at_level;
            blocks_at_level = hash_blocks_at_level;
        }

        /* Now hash_block_offset points to the current level's hash data */
        /* Within the level, each hash block is VERITY_BLOCK_SIZE */
        uint64_t hash_offset = hash_block_offset + (uint64_t)hash_block_idx * VERITY_BLOCK_SIZE
                               + (uint64_t)hash_in_block * VERITY_HASH_SIZE;

        /* This would be the stored hash; in our tree layout it's at hash_offset */
        uint8_t stored_hash[VERITY_HASH_SIZE];
        if (hash_offset + VERITY_HASH_SIZE <= vd->tree_size) {
            memcpy(stored_hash, vd->tree_data + hash_offset, VERITY_HASH_SIZE);
        } else {
            /* At the root level */
            if (vd->tree_size >= VERITY_HASH_SIZE)
                memcpy(stored_hash, vd->root_hash, VERITY_HASH_SIZE);
            else
                return -EIO;
        }

        /* Compare */
        if (memcmp(current_hash, stored_hash, VERITY_HASH_SIZE) != 0) {
            kprintf("[fs-verity] VERIFICATION FAILED: ino=%llu block=%u "
                    "at level %u\n",
                    (unsigned long long)ino, block, levels_verified);
            return -EIO;
        }

        /* Check if we've reached the root */
        if (levels_verified >= vd->tree_levels - 1)
            return 0; /* verified successfully */

        /* Move up: the hash of the entire hash block becomes our target */
        uint8_t hash_block_data[VERITY_BLOCK_SIZE];
        memset(hash_block_data, 0, VERITY_BLOCK_SIZE);
        memcpy(hash_block_data + (uint64_t)hash_in_block * VERITY_HASH_SIZE,
               current_hash, VERITY_HASH_SIZE);

        sha256_hash(current_hash, hash_block_data, VERITY_BLOCK_SIZE);
        current_block = hash_block_idx;
        levels_verified++;
    }
}
EXPORT_SYMBOL(fsverity_verify_block);

/* Verify a complete file against its Merkle tree.
 *
 * @ino:  inode number
 * @data: pointer to the file's data
 * @size: file size in bytes
 *
 * Returns 0 if all blocks verify, -EIO on first verification failure.
 */
int fsverity_verify_file(uint64_t ino, const uint8_t *data, uint64_t size)
{
    struct verity_descriptor *vd = verity_find(ino);
    if (!vd || !vd->verified)
        return -ENOENT;

    uint32_t num_blocks = (uint32_t)((size + VERITY_BLOCK_SIZE - 1) / VERITY_BLOCK_SIZE);
    uint32_t to_verify = num_blocks < vd->data_blocks ? num_blocks : vd->data_blocks;

    for (uint32_t b = 0; b < to_verify; b++) {
        const uint8_t *block_data = data + (uint64_t)b * VERITY_BLOCK_SIZE;
        int ret = fsverity_verify_block(ino, b, block_data);
        if (ret != 0)
            return ret;
    }

    return 0;
}
EXPORT_SYMBOL(fsverity_verify_file);

/* Get the root hash for a file with fs-verity enabled.
 * Returns 0 on success (root hash copied to output), -ENOENT if not enabled. */
int fsverity_get_root_hash(uint64_t ino, uint8_t root_hash[VERITY_HASH_SIZE])
{
    struct verity_descriptor *vd = verity_find(ino);
    if (!vd || !vd->verified)
        return -ENOENT;

    memcpy(root_hash, vd->root_hash, VERITY_HASH_SIZE);
    return 0;
}
EXPORT_SYMBOL(fsverity_get_root_hash);

/* Disable fs-verity on a file (free tree data).
 * Returns 0 on success. */
int fsverity_disable(uint64_t ino)
{
    struct verity_descriptor *vd = verity_find(ino);
    if (!vd)
        return -ENOENT;

    if (vd->tree_data) {
        kfree(vd->tree_data);
        vd->tree_data = NULL;
    }
    memset(vd, 0, sizeof(*vd));
    return 0;
}
EXPORT_SYMBOL(fsverity_disable);
