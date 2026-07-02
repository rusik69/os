/*
 * src/fs/btrfs_csum.c — Btrfs checksum verification (CRC32C)
 *
 * Provides CRC32C verification for Btrfs tree nodes and data blocks.
 * Btrfs uses CRC32C (Castagnoli polynomial 0x82F63B78) for both node
 * header checksums and data block checksums stored in the checksum tree.
 *
 * Node checksum verification:
 *   Each tree node/leaf has a 32-byte csum field at offset 0 of the header.
 *   For csum_type 0 (CRC32C), the first 4 bytes hold the CRC32C of the
 *   entire node (nodesize bytes) after zeroing the csum field.
 *
 * Data block checksum verification:
 *   The checksum tree (CSUM_TREE_OBJECTID = 7) maps logical block numbers
 *   to checksums. Items are keyed by (block_number, CSUM_ITEM_KEY, count)
 *   and contain an array of 4-byte CRC32C values.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "btrfs.h"
#include "crc.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Utility: little-endian 32-bit load ───────────────────────── */

static inline uint32_t csum_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── Checksum size lookup ─────────────────────────────────────── */

/**
 * btrfs_csum_size - Return the checksum size in bytes for a given type
 * @csum_type: Btrfs superblock csum_type (0 = CRC32C)
 *
 * Returns: checksum size in bytes, or negative errno for unknown types
 */
int btrfs_csum_size(uint16_t csum_type)
{
	switch (csum_type) {
	case 0:
		return 4;	/* CRC32C — 4 bytes per block */
	default:
		return -EINVAL;
	}
}

/* ── Node/leaf header checksum verification ───────────────────── */

/**
 * btrfs_csum_verify_node - Verify CRC32C checksum of a tree node/leaf
 * @node_buf: Buffer containing the raw node data (modified then restored)
 * @nodesize: Total size of the node in bytes
 * @csum_type: Superblock csum_type (0 = CRC32C)
 *
 * Extracts the stored CRC32C from the first 4 bytes of the header csum
 * field, zeros the 32-byte csum field, computes CRC32C over the entire
 * node, restores the csum field, and compares.
 *
 * Returns: 0 on success, -EBADMSG on checksum mismatch, -EINVAL on
 *          invalid parameters
 */
int btrfs_csum_verify_node(uint8_t *node_buf, uint32_t nodesize,
                            uint16_t csum_type)
{
	uint8_t saved_csum[32];
	uint32_t stored_csum;
	uint32_t computed_csum;

	if (csum_type != 0)
		return -EINVAL;
	if (nodesize < 32 || nodesize > 65536)
		return -EINVAL;

	/* Extract stored CRC32C (first 4 bytes of the 32-byte csum field) */
	stored_csum = csum_le32(node_buf);

	/* Save and zero the 32-byte csum field for CRC computation */
	memcpy(saved_csum, node_buf, 32);
	memset(node_buf, 0, 32);

	computed_csum = crc32c(0, node_buf, nodesize);

	/* Restore the original csum field */
	memcpy(node_buf, saved_csum, 32);

	if (computed_csum != stored_csum)
		return -EBADMSG;

	return 0;
}

/* ── Data block checksum verification ─────────────────────────── */

/**
 * btrfs_csum_verify_data - Verify a data block's CRC32C against its
 *                          checksum stored in the checksum tree leaf
 * @csum_leaf_data: Pointer to the stored checksum array
 * @num_csums: Number of checksums in the array
 * @block_index: Index of the block within this checksum item (0-based)
 * @block_data: Pointer to the actual block data
 * @block_size: Size of the data block in bytes
 * @csum_type: Superblock csum_type (0 = CRC32C)
 *
 * Returns: 0 if checksum matches, -EBADMSG on mismatch, -EINVAL on
 *          invalid parameters
 */
int btrfs_csum_verify_data(const uint8_t *csum_leaf_data,
                            uint32_t num_csums, uint32_t block_index,
                            const uint8_t *block_data,
                            uint32_t block_size, uint16_t csum_type)
{
	uint32_t stored_csum;
	uint32_t computed_csum;

	if (csum_type != 0)
		return -EINVAL;
	if (block_index >= num_csums)
		return -EINVAL;

	stored_csum = csum_le32(csum_leaf_data + block_index * 4);
	computed_csum = crc32c(0, block_data, block_size);

	if (computed_csum != stored_csum)
		return -EBADMSG;

	return 0;
}

/* ── Module boilerplate ───────────────────────────────────────── */

#ifdef MODULE
int init_module(void) { return 0; }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Btrfs CRC32C checksum verification for tree nodes and data blocks");
#endif
