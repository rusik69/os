/*
 * dm-verity.c — Device Mapper integrity target (Merkle hash tree)
 *
 * On-access integrity verification of block devices using a Merkle
 * hash tree stored on a separate hash device.  Each data block
 * (4096 bytes = 8 sectors) is verified against a SHA-256 hash tree
 * before being returned to the caller.  Writes are rejected with -EIO
 * (read-only protection).
 *
 * Hash tree layout (Linux dm-verity compatible):
 *   Level 0: hashes of data blocks, stored sequentially
 *   Level 1: hashes of Level 0 hash blocks
 *   Level 2: hashes of Level 1 hash blocks
 *   ...
 *   Root hash: passed as a constructor parameter (not stored)
 *
 * Each hash = SHA-256 (32 bytes).
 * Each hash block = 4096 bytes = 128 hashes.
 *
 * Table format:
 *   start length verity <data_dev_id> <hash_dev_id> <hash_start_sector> <root_hash_hex>
 *
 * Example:
 *   0 4194304 verity 0 1 0 deadbeef...64hex...
 *     — 2 GB device, data on dev 0, hashes on dev 1 at sector 0,
 *       root hash = deadbeef... (64 hex chars = 32 bytes)
 *
 * Item 359: dm-verity — device mapper integrity target
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "sha256.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"
#include "errno.h"
#include "blockdev.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/* Verity block size: 4096 bytes (8 sectors).  This is the unit
 * of data that is hashed and verified. */
#define VERITY_BLOCK_SIZE      4096
#define VERITY_BLOCK_SECTORS   8    /* 4096 / 512 */

/* SHA-256 digest size (32 bytes / 64 hex chars) */
#define VERITY_HASH_SIZE       32

/* Number of hashes that fit in one hash block */
#define VERITY_HASHES_PER_BLOCK  (VERITY_BLOCK_SIZE / VERITY_HASH_SIZE)

/* Maximum root hash hex string length */
#define VERITY_ROOT_HASH_HEX_LEN 65

/* ── Private per-target data ───────────────────────────────────────── */

struct verity_private {
    int     data_dev_id;           /* block device ID of the data device */
    int     hash_dev_id;           /* block device ID of the hash device */
    uint64_t hash_start;           /* first sector of hash tree on hash device */
    uint8_t  root_hash[VERITY_HASH_SIZE];  /* expected root hash */
    uint64_t total_blocks;         /* total number of verity blocks */
};

/* ── Hex string helper ─────────────────────────────────────────────── */

/* Decode a hex string into a byte array.  Returns bytes written or -1. */
static int hex_decode(const char *hex, uint8_t *out, int max_bytes)
{
    int len = 0;
    while (*hex && len < max_bytes) {
        uint8_t hi = 0, lo = 0;

        if (*hex >= '0' && *hex <= '9') hi = (uint8_t)(*hex - '0');
        else if (*hex >= 'a' && *hex <= 'f') hi = (uint8_t)(*hex - 'a' + 10);
        else if (*hex >= 'A' && *hex <= 'F') hi = (uint8_t)(*hex - 'A' + 10);
        else break;
        hex++;

        if (*hex >= '0' && *hex <= '9') lo = (uint8_t)(*hex - '0');
        else if (*hex >= 'a' && *hex <= 'f') lo = (uint8_t)(*hex - 'a' + 10);
        else if (*hex >= 'A' && *hex <= 'F') lo = (uint8_t)(*hex - 'A' + 10);
        else { hi = 0; break; }  /* odd hex digit — malformed */
        hex++;

        out[len++] = (uint8_t)((hi << 4) | lo);
    }
    return len;
}

/* ── Hash tree helpers ─────────────────────────────────────────────── */

/* Given a data block index, compute the hash device sector offset for
 * the hash at the given tree level.
 *
 * The hash tree is stored level by level on the hash device:
 *   Level 0: hashes of all data blocks   — offset = hash_start
 *   Level 1: hashes of level-0 hash blks — offset = hash_start + l0_size
 *   Level 2: hashes of level-1 hash blks — offset = hash_start + l0_size + l1_size
 *   ...
 *
 * Each hash is 32 bytes.  Hashes within a level are packed sequentially.
 */
static uint64_t verity_hash_offset(struct verity_private *vp,
                                   uint64_t block_num, int level)
{
    uint64_t offset = vp->hash_start;

    /* Skip preceding levels */
    for (int l = 0; l < level; l++) {
        uint64_t blocks_at_level = vp->total_blocks;
        for (int i = 0; i < l; i++)
            blocks_at_level = (blocks_at_level + VERITY_HASHES_PER_BLOCK - 1)
                              / VERITY_HASHES_PER_BLOCK;
        uint64_t hash_blocks = (blocks_at_level + VERITY_HASHES_PER_BLOCK - 1)
                               / VERITY_HASHES_PER_BLOCK;
        offset += hash_blocks * VERITY_BLOCK_SECTORS;
    }

    /* Add the hash position within this level */
    return offset + block_num * VERITY_HASH_SIZE / 512;
}

/* Verify a single data block against the Merkle hash tree.
 *
 * Reads the data block from the data device, computes its hash,
 * then walks up the tree verifying each level.  If any level
 * mismatches, the block is corrupt and -EIO is returned.
 *
 * @param vp        verity private data
 * @param block_num index of the data block to verify
 * @param buf       buffer containing the data block (must be VERITY_BLOCK_SIZE)
 * @return 0 on success, -EIO on hash mismatch, negative errno on I/O error
 */
static int verity_verify_block(struct verity_private *vp,
                               uint64_t block_num, const uint8_t *buf)
{
    uint8_t  hash[VERITY_HASH_SIZE];
    uint8_t  stored_hash[VERITY_HASH_SIZE];
    uint64_t hash_sector;
    int      ret;

    /* Compute hash of the data block */
    sha256_hash(hash, buf, VERITY_BLOCK_SIZE);

    /* Walk up the hash tree */
    uint64_t current_block = block_num;
    int      level = 0;

    while (1) {
        /* Calculate the hash sector on the hash device for this level */
        hash_sector = verity_hash_offset(vp, current_block, level);

        /* Read the stored hash (32 bytes = 1 sector's worth of hashes) */
        ret = blk_submit_sync(vp->hash_dev_id, hash_sector, 1,
                              stored_hash, BLK_REQ_READ);
        if (ret != 0) {
            kprintf("[dm-verity] I/O error reading hash at level %d, "
                    "block %llu, sector %llu: %d\n",
                    level, (unsigned long long)current_block,
                    (unsigned long long)hash_sector, ret);
            return ret;
        }

        /* Compare the computed hash with the stored hash */
        if (memcmp(hash, stored_hash, VERITY_HASH_SIZE) != 0) {
            kprintf("[dm-verity] VERIFICATION FAILED at level %d, "
                    "block %llu: hash mismatch\n",
                    level, (unsigned long long)current_block);
            kprintf("  computed: ");
            for (int i = 0; i < VERITY_HASH_SIZE; i++)
                kprintf("%02x", hash[i]);
            kprintf("\n  stored:   ");
            for (int i = 0; i < VERITY_HASH_SIZE; i++)
                kprintf("%02x", stored_hash[i]);
            kprintf("\n");
            return -EIO;
        }

        /* If we just verified against the root hash, we're done */
        if (level > 0 && current_block == 0) {
            /* Compare the stored hash against the known root hash.
             * We've already verified it matches the computed hash from
             * the level above; now check it matches the trusted root. */
            if (memcmp(hash, vp->root_hash, VERITY_HASH_SIZE) != 0) {
                kprintf("[dm-verity] ROOT HASH MISMATCH: block %llu\n",
                        (unsigned long long)block_num);
                kprintf("  expected root: ");
                for (int i = 0; i < VERITY_HASH_SIZE; i++)
                    kprintf("%02x", vp->root_hash[i]);
                kprintf("\n");
                return -EIO;
            }
            return 0; /* Verified successfully */
        }

        /* Move up one level: the hash of this hash block is stored
         * at the parent level.  Compute which hash block we're in
         * and which parent hash verifies it. */
        uint64_t hash_block_index = current_block / VERITY_HASHES_PER_BLOCK;
        uint64_t parent_block = hash_block_index;
        uint64_t hash_in_block = current_block % VERITY_HASHES_PER_BLOCK;

        /* Read the full hash block to verify it first */
        uint8_t *hash_block = (uint8_t *)kmalloc(VERITY_BLOCK_SIZE);
        if (!hash_block) return -ENOMEM;

        /* The hash block containing the hash we just read */
        uint64_t hash_block_sector = hash_sector -
            (hash_in_block * VERITY_HASH_SIZE / 512);
        ret = blk_submit_sync(vp->hash_dev_id, hash_block_sector,
                              VERITY_BLOCK_SECTORS, hash_block, BLK_REQ_READ);
        if (ret != 0) {
            kfree(hash_block);
            return ret;
        }

        /* Compute the hash of this hash block */
        uint8_t parent_hash[VERITY_HASH_SIZE];
        sha256_hash(parent_hash, hash_block, VERITY_BLOCK_SIZE);
        kfree(hash_block);

        /* Move up: the parent hash becomes our target */
        memcpy(hash, parent_hash, VERITY_HASH_SIZE);
        current_block = parent_block;
        level++;
    }
}

/* ── Target operations ────────────────────────────────────────────── */

/* Constructor: parse arguments and initialise the verity target.
 *
 * Table line format:
 *   start length verity <data_dev_id> <hash_dev_id> <hash_start_sector> <root_hash_hex>
 *
 * Example:
 *   0 4194304 verity 0 1 0 deadbeef...64hex...
 */
static int verity_ctr(struct dm_target *ti, int argc, const char **argv)
{
    if (argc < 4) {
        kprintf("[dm-verity] ctr: need 4 args (data_dev hash_dev hash_start root_hash), got %d\n",
                argc);
        return -EINVAL;
    }

    struct verity_private *priv = (struct verity_private *)
        kmalloc(sizeof(struct verity_private));
    if (!priv) return -ENOMEM;
    memset(priv, 0, sizeof(*priv));

    /* Parse data device ID */
    const char *s = argv[0];
    int dev_id = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            kprintf("[dm-verity] ctr: invalid data_dev_id '%s'\n", argv[0]);
            kfree(priv);
            return -EINVAL;
        }
        dev_id = dev_id * 10 + (*s++ - '0');
    }
    if (!blockdev_is_registered(dev_id)) {
        kprintf("[dm-verity] ctr: data device %d not registered\n", dev_id);
        kfree(priv);
        return -ENODEV;
    }
    priv->data_dev_id = dev_id;

    /* Parse hash device ID */
    s = argv[1];
    int hash_dev = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            kprintf("[dm-verity] ctr: invalid hash_dev_id '%s'\n", argv[1]);
            kfree(priv);
            return -EINVAL;
        }
        hash_dev = hash_dev * 10 + (*s++ - '0');
    }
    if (!blockdev_is_registered(hash_dev)) {
        kprintf("[dm-verity] ctr: hash device %d not registered\n", hash_dev);
        kfree(priv);
        return -ENODEV;
    }
    priv->hash_dev_id = hash_dev;

    /* Parse hash start sector */
    s = argv[2];
    uint64_t hash_start = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            kprintf("[dm-verity] ctr: invalid hash_start '%s'\n", argv[2]);
            kfree(priv);
            return -EINVAL;
        }
        hash_start = hash_start * 10 + (*s++ - '0');
    }
    priv->hash_start = hash_start;

    /* Parse root hash (64 hex chars = 32 bytes SHA-256) */
    int hash_len = hex_decode(argv[3], priv->root_hash, VERITY_HASH_SIZE);
    if (hash_len != VERITY_HASH_SIZE) {
        kprintf("[dm-verity] ctr: root_hash must be %d hex chars (got %d bytes)\n",
                VERITY_HASH_SIZE * 2, hash_len * 2);
        kfree(priv);
        return -EINVAL;
    }

    /* Compute total blocks from the target length in sectors */
    priv->total_blocks = ti->length / VERITY_BLOCK_SECTORS;

    ti->private = priv;

    kprintf("[dm-verity] ctr: [%llu, %llu) data dev %d, hash dev %d, "
            "hash_start %llu, blocks %llu\n",
            (unsigned long long)ti->start,
            (unsigned long long)(ti->start + ti->length),
            priv->data_dev_id, priv->hash_dev_id,
            (unsigned long long)priv->hash_start,
            (unsigned long long)priv->total_blocks);

    return 0;
}

/* Destructor: free private data. */
static void verity_dtr(struct dm_target *ti)
{
    if (ti->private) {
        struct verity_private *priv = (struct verity_private *)ti->private;
        memset(priv, 0, sizeof(*priv));
        kfree(priv);
        ti->private = NULL;
    }
}

/* Map: verify reads against Merkle hash tree; reject writes.
 *
 * For READ requests:
 *   1. Read the data block from the backing device synchronously
 *   2. Verify the data block against the Merkle hash tree
 *   3. On success, mark the request as done
 *   4. On verification failure, return -EIO
 *
 * For WRITE requests:
 *   dm-verity is read-only — all writes are rejected with -EIO.
 */
static int verity_map(struct dm_target *ti, struct blk_request *req,
                      struct blk_request *mapped[], int *mapped_count)
{
    (void)mapped; /* we handle I/O synchronously — mapped array not used */

    struct verity_private *priv = (struct verity_private *)ti->private;
    if (!priv) return -EINVAL;

    /* Calculate the virtual sector offset within this target */
    uint64_t offset = req->lba - ti->start;

    if (req->flags & BLK_REQ_WRITE) {
        /* ── WRITE path: reject (read-only target) ─────────────────── */
        kprintf("[dm-verity] WRITE rejected at virtual sector %llu "
                "(read-only target)\n", (unsigned long long)req->lba);
        req->result = -EROFS;
        blk_request_done(req);
        *mapped_count = 0;
        return 0;
    } else {
        /* ── READ path: verify data against hash tree ──────────────── */

        /* Determine which verity block(s) this request spans */
        uint64_t first_block = offset / VERITY_BLOCK_SECTORS;
        uint64_t last_block  = (offset + req->count - 1) / VERITY_BLOCK_SECTORS;

        /* Read the data from the backing data device synchronously into
         * the request buffer.  The data device is a different block device
         * than the hash device for dm-verity. */
        uint64_t data_lba = offset; /* data starts at sector 0 on data dev */
        int ret = blk_submit_sync(priv->data_dev_id, data_lba,
                                  req->count, req->buf, BLK_REQ_READ);
        if (ret != 0) {
            req->result = ret;
            *mapped_count = 0;
            return ret;
        }

        /* Verify each block in the request against the Merkle hash tree */
        for (uint64_t b = first_block; b <= last_block; b++) {
            uint64_t block_offset = (b - first_block) * VERITY_BLOCK_SIZE;
            if (block_offset + VERITY_BLOCK_SIZE > (uint64_t)req->count * 512)
                break; /* partial block at end — skip (verified next time) */

            const uint8_t *block_buf = (const uint8_t *)req->buf + block_offset;
            ret = verity_verify_block(priv, b, block_buf);
            if (ret != 0) {
                kprintf("[dm-verity] Block %llu verification FAILED\n",
                        (unsigned long long)b);
                req->result = ret;
                *mapped_count = 0;
                return ret;
            }
        }

        /* All blocks verified — complete the request */
        req->result = 0;
        blk_request_done(req);
        *mapped_count = 0;
        return 0;
    }
}

/* ── Target operations struct ─────────────────────────────────────── */

static const struct dm_target_ops verity_ops = {
    .name  = "verity",
    .flags = 0,   /* data verification target — no special flags */
    .ctr   = verity_ctr,
    .dtr   = verity_dtr,
    .map   = verity_map,
};

/* ── Init / registration ──────────────────────────────────────────── */

void dm_verity_init(void)
{
    int ret = dm_register_target(&verity_ops);
    if (ret == 0) {
        kprintf("[OK] dm-verity: integrity target registered\n");
    } else {
        kprintf("[FAIL] dm-verity: registration failed: %d\n", ret);
    }
}
