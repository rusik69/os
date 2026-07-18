/*
 * src/fs/exfat.c — exFAT filesystem (read/write)
 *
 * Implements an exFAT filesystem supporting:
 *   - Boot sector parsing (MainBoot + BackupBoot)
 *   - FAT chain traversal via allocation bitmap
 *   - Root directory and subdirectory parsing via stream extension entries
 *   - Up-case table for case-insensitive lookup
 *   - Directory entry set creation, modification, and removal
 *   - Basic cluster allocation via bitmap management
 */

#define KERNEL_INTERNAL
#include "exfat.h"

#include "blockdev.h"
#include "crc.h"
#include "errno.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "vfs.h"

#ifdef MODULE
#include "module.h"
#endif
#include "initcall.h"

/* ── Write helpers ────────────────────────────────────────────────── */

/* Forward declaration */
static uint64_t exfat_cluster_to_sector(struct exfat_priv *ep, uint32_t cluster);
static int exfat_read_fat_entry(struct exfat_priv *ep, uint32_t cluster, uint32_t *entry);
static int exfat_write_fat_entry(struct exfat_priv *ep, uint32_t cluster, uint32_t value);

static int exfat_write_cluster(struct exfat_priv *ep, uint32_t cluster, const uint8_t *buf) {
    uint64_t start_sector = exfat_cluster_to_sector(ep, cluster);
    uint32_t sectors = 1U << ep->sectors_per_cluster_shift;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_write_sectors(ep->dev_id, start_sector + i, 1, buf + i * ep->sector_size) != 0)
            return -1;
    }
    return 0;
}

/* ── Helpers ────────────────────────────────────────────────────── */

static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t r64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* ── Cluster operations ─────────────────────────────────────────── */

static uint64_t exfat_cluster_to_sector(struct exfat_priv *ep, uint32_t cluster) {
    if (cluster == 0) {
        /* Cluster 0 is the root directory for exFAT */
        return (uint64_t)ep->cluster_heap_offset;
    }
    return (uint64_t)ep->cluster_heap_offset +
           ((uint64_t)(cluster - 2) << ep->sectors_per_cluster_shift);
}

static int exfat_read_cluster(struct exfat_priv *ep, uint32_t cluster, uint8_t *buf) {
    uint64_t start_sector = exfat_cluster_to_sector(ep, cluster);
    uint32_t sectors = 1U << ep->sectors_per_cluster_shift;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ep->dev_id, start_sector + i, 1, buf + i * ep->sector_size) != 0)
            return -1;
    }
    return 0;
}

/* ── FAT chain reading ──────────────────────────────────────────── */

/* exFAT doesn't have a traditional FAT. Instead, clusters are allocated
 * using the bitmap. Allocation is sequential in exFAT's design.
 * For reading file data, we use the first_cluster and data_length from
 * the stream extension entry. exFAT files are typically contiguous.
 * For simplicity, we assume contiguous allocation and iterate clusters
 * sequentially from first_cluster through (data_length / cluster_size) clusters.
 *
 * A more complete implementation would check the bitmap for allocation info,
 * but exFAT's FAT chain is implicit — clusters are contiguous unless the
 * volume is fragmented. We'll handle the contiguous case. */

static uint32_t exfat_next_cluster(struct exfat_priv *ep, uint32_t cluster) {
    if (cluster >= EXFAT_CLUSTER_END)
        return EXFAT_CLUSTER_END;

    /* Clusters 0 and 1 are reserved in exFAT and are never valid data
     * clusters in chain traversal.  Returning them would cause callers
     * to attempt I/O on invalid sector ranges. */
    if (cluster < 2)
        return EXFAT_CLUSTER_END;

    /* If no FAT table, assume contiguous allocation */
    if (ep->fat_length == 0) {
        /* Return next contiguous cluster, but don't go beyond the
         * valid cluster range (2 .. cluster_count+1).  Returning a
         * cluster past the heap would cause callers to attempt I/O
         * on sectors that do not belong to the filesystem data area. */
        if (cluster >= ep->cluster_count + 1)
            return EXFAT_CLUSTER_END;
        return cluster + 1;
    }

    /* Read the FAT entry for this cluster to find the next one */
    uint32_t next;
    if (exfat_read_fat_entry(ep, cluster, &next) != 0)
        return EXFAT_CLUSTER_END;

    /* Check for end-of-chain markers (0xFFFFFFF8 through 0xFFFFFFFF) */
    if (next >= 0xFFFFFFF8)
        return EXFAT_CLUSTER_END;

    /* Check for bad cluster marker (0xFFFFFFF7) */
    if (next == 0xFFFFFFF7)
        return EXFAT_CLUSTER_END;

    /* Check for reserved markers (0xFFFFFFF0 through 0xFFFFFFF6) */
    if (next >= 0xFFFFFFF0)
        return EXFAT_CLUSTER_END;

    /* Check for free marker (0x00000000) — should not happen for in-use cluster */
    if (next == 0)
        return EXFAT_CLUSTER_END;

    /* Cluster 1 is reserved in exFAT — should never appear in a chain */
    if (next == 1)
        return EXFAT_CLUSTER_END;

    /* Ensure the returned cluster is within the valid range.
     * The maximum valid data cluster is cluster_count+1.  Values
     * above this (but below 0xFFFFFFF0) are not valid data clusters
     * and would cause callers to compute sector addresses outside
     * the cluster heap. */
    if (next > ep->cluster_count + 1)
        return EXFAT_CLUSTER_END;

    return next;
}

/* ── Bitmap operations ──────────────────────────────────────────── */
/* exFAT uses an allocation bitmap to track cluster allocation.
 * Each bit = one cluster (1 = allocated, 0 = free).
 * Clusters 0 and 1 are reserved; valid clusters start at 2.
 *
 * A single-sector cache (512 bytes) is used to batch adjacent
 * bit lookups without re-reading the same disk sector.
 * Bitmap sectors start at ep->bitmap_start_sector. */

/* Load a bitmap sector into the cache.  Flushes any dirty cached
 * sector first.  Returns 0 on success, negative errno on error. */
static int exfat_bitmap_load_sector(struct exfat_priv *ep, uint32_t sector_index) {
    /* If already cached, nothing to do */
    if (ep->cached_bitmap_sector == sector_index)
        return 0;

    /* Flush any dirty cached sector before replacing it */
    if (ep->cached_bitmap_dirty) {
        if (blockdev_write_sectors(ep->dev_id, ep->bitmap_start_sector + ep->cached_bitmap_sector,
                                   1, ep->cached_bitmap_data) != 0)
            return -EIO;
        ep->cached_bitmap_dirty = 0;
    }

    /* Read the new sector into cache */
    if (blockdev_read_sectors(ep->dev_id, ep->bitmap_start_sector + sector_index, 1,
                              ep->cached_bitmap_data) != 0) {
        ep->cached_bitmap_sector = ~0U;
        return -EIO;
    }

    ep->cached_bitmap_sector = sector_index;
    ep->cached_bitmap_dirty = 0;
    return 0;
}

/* Read a single bit from the allocation bitmap.
 * Returns 0 on success, negative errno on error. */
static int exfat_bitmap_get(struct exfat_priv *ep, uint32_t cluster, uint8_t *bit_val) {
    uint32_t byte_offset = cluster / 8;
    uint32_t bit_offset = cluster % 8;
    uint32_t sector_idx = byte_offset / ep->sector_size;
    uint32_t byte_in_sec = byte_offset % ep->sector_size;

    if (cluster < 2 || cluster > ep->cluster_count + 1)
        return -EINVAL;

    if (exfat_bitmap_load_sector(ep, sector_idx) != 0)
        return -EIO;

    *bit_val = (ep->cached_bitmap_data[byte_in_sec] >> bit_offset) & 1;
    return 0;
}

/* Set or clear a single bit in the allocation bitmap.
 * Returns 0 on success, negative errno on error. */
static int exfat_bitmap_set(struct exfat_priv *ep, uint32_t cluster, int allocated) {
    uint32_t byte_offset = cluster / 8;
    uint32_t bit_offset = cluster % 8;
    uint32_t sector_idx = byte_offset / ep->sector_size;
    uint32_t byte_in_sec = byte_offset % ep->sector_size;

    if (cluster < 2 || cluster > ep->cluster_count + 1)
        return -EINVAL;

    if (exfat_bitmap_load_sector(ep, sector_idx) != 0)
        return -EIO;

    if (allocated)
        ep->cached_bitmap_data[byte_in_sec] |= (uint8_t)(1U << bit_offset);
    else
        ep->cached_bitmap_data[byte_in_sec] &= (uint8_t)(~(1U << bit_offset));

    ep->cached_bitmap_dirty = 1;
    return 0;
}

/* Flush any dirty bitmap cache to disk.  Returns 0 on success,
 * negative errno on error.  Safe to call when cache is clean. */
static int exfat_bitmap_flush(struct exfat_priv *ep) {
    if (!ep->cached_bitmap_dirty)
        return 0;

    if (blockdev_write_sectors(ep->dev_id, ep->bitmap_start_sector + ep->cached_bitmap_sector, 1,
                               ep->cached_bitmap_data) != 0)
        return -EIO;

    ep->cached_bitmap_dirty = 0;
    return 0;
}

/* ── FAT table management ────────────────────────────────────────── */
/* exFAT has an optional FAT table used for cluster chaining.
 * It is present when fat_length > 0 in the boot sector.
 * When absent, clusters are assumed contiguous.
 * Each FAT entry is a 32-bit little-endian value:
 *   0x00000000 = free cluster
 *   0xFFFFFFF7 = bad cluster
 *   0xFFFFFFF8..0xFFFFFFFF = end-of-chain */

/* Load a FAT sector into the cache.  Flushes any dirty cached
 * sector first.  Returns 0 on success, negative errno on error. */
static int exfat_fat_load_sector(struct exfat_priv *ep, uint32_t sector_index) {
    if (sector_index >= ep->fat_length)
        return -EINVAL;

    /* If already cached, nothing to do */
    if (ep->cached_fat_sector == sector_index)
        return 0;

    /* Flush any dirty cached sector before replacing it */
    if (ep->cached_fat_dirty) {
        if (blockdev_write_sectors(ep->dev_id, ep->fat_offset + ep->cached_fat_sector, 1,
                                   ep->cached_fat_data) != 0)
            return -EIO;
        ep->cached_fat_dirty = 0;
    }

    /* Read the new sector into cache */
    if (blockdev_read_sectors(ep->dev_id, ep->fat_offset + sector_index, 1, ep->cached_fat_data) !=
        0) {
        ep->cached_fat_sector = ~0U;
        return -EIO;
    }

    ep->cached_fat_sector = sector_index;
    ep->cached_fat_dirty = 0;
    return 0;
}

/* Flush any dirty FAT cache to disk.  Returns 0 on success,
 * negative errno on error.  Safe to call when cache is clean. */
static int exfat_fat_flush(struct exfat_priv *ep) {
    if (!ep->cached_fat_dirty)
        return 0;

    if (blockdev_write_sectors(ep->dev_id, ep->fat_offset + ep->cached_fat_sector, 1,
                               ep->cached_fat_data) != 0)
        return -EIO;

    ep->cached_fat_dirty = 0;
    return 0;
}

/* Read a FAT entry for the given cluster.
 * Returns 0 on success with *entry set, negative errno on error. */
static int exfat_read_fat_entry(struct exfat_priv *ep, uint32_t cluster, uint32_t *entry) {
    if (cluster < 2 || cluster > ep->cluster_count + 1)
        return -EINVAL;

    uint32_t byte_offset = cluster * 4;
    uint32_t sector_idx = byte_offset / ep->sector_size;
    uint32_t byte_in_sec = byte_offset % ep->sector_size;

    if (exfat_fat_load_sector(ep, sector_idx) != 0)
        return -EIO;

    *entry = r32(ep->cached_fat_data + byte_in_sec);
    return 0;
}

/* Write a FAT entry for the given cluster.
 * Returns 0 on success, negative errno on error. */
static int exfat_write_fat_entry(struct exfat_priv *ep, uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster > ep->cluster_count + 1)
        return -EINVAL;

    uint32_t byte_offset = cluster * 4;
    uint32_t sector_idx = byte_offset / ep->sector_size;
    uint32_t byte_in_sec = byte_offset % ep->sector_size;

    if (exfat_fat_load_sector(ep, sector_idx) != 0)
        return -EIO;

    /* Write little-endian 32-bit value */
    ep->cached_fat_data[byte_in_sec] = (uint8_t)(value & 0xFF);
    ep->cached_fat_data[byte_in_sec + 1] = (uint8_t)((value >> 8) & 0xFF);
    ep->cached_fat_data[byte_in_sec + 2] = (uint8_t)((value >> 16) & 0xFF);
    ep->cached_fat_data[byte_in_sec + 3] = (uint8_t)((value >> 24) & 0xFF);
    ep->cached_fat_dirty = 1;
    return 0;
}

/* ── Cluster allocator ──────────────────────────────────────────── */

static uint32_t exfat_alloc_cluster(struct exfat_priv *ep) {
    uint8_t bit_val;
    uint32_t start = ep->next_free_hint;
    uint32_t max_c = ep->cluster_count + 2;

    /* Clamp hint to valid range */
    if (start < 2 || start >= max_c)
        start = 2;

    /* Scan from hint upward */
    for (uint32_t c = start; c < max_c; c++) {
        if (exfat_bitmap_get(ep, c, &bit_val) != 0)
            return EXFAT_CLUSTER_END;
        if (bit_val == 0) {
            if (exfat_bitmap_set(ep, c, 1) != 0)
                return EXFAT_CLUSTER_END;
            /* Zero-initialize the cluster */
            uint32_t cluster_sectors = 1U << ep->sectors_per_cluster_shift;
            uint32_t buf_size = cluster_sectors * ep->sector_size;
            uint8_t *zbuf = (uint8_t *)kmalloc(buf_size);
            if (!zbuf) {
                exfat_bitmap_set(ep, c, 0);
                return EXFAT_CLUSTER_END;
            }
            memset(zbuf, 0, buf_size);
            int ret = exfat_write_cluster(ep, c, zbuf);
            kfree(zbuf);
            if (ret != 0) {
                exfat_bitmap_set(ep, c, 0);
                return EXFAT_CLUSTER_END;
            }
            /* Update hint and free cluster count */
            ep->next_free_hint = c + 1;
            if (ep->free_clusters > 0)
                ep->free_clusters--;
            return c;
        }
    }

    /* Wrap around and scan from 2 up to hint */
    if (start > 2) {
        for (uint32_t c = 2; c < start; c++) {
            if (exfat_bitmap_get(ep, c, &bit_val) != 0)
                return EXFAT_CLUSTER_END;
            if (bit_val == 0) {
                if (exfat_bitmap_set(ep, c, 1) != 0)
                    return EXFAT_CLUSTER_END;
                uint32_t cluster_sectors = 1U << ep->sectors_per_cluster_shift;
                uint32_t buf_size = cluster_sectors * ep->sector_size;
                uint8_t *zbuf = (uint8_t *)kmalloc(buf_size);
                if (!zbuf) {
                    exfat_bitmap_set(ep, c, 0);
                    return EXFAT_CLUSTER_END;
                }
                memset(zbuf, 0, buf_size);
                int ret = exfat_write_cluster(ep, c, zbuf);
                kfree(zbuf);
                if (ret != 0) {
                    exfat_bitmap_set(ep, c, 0);
                    return EXFAT_CLUSTER_END;
                }
                ep->next_free_hint = c + 1;
                if (ep->free_clusters > 0)
                    ep->free_clusters--;
                return c;
            }
        }
    }

    return EXFAT_CLUSTER_END; /* no free clusters */
}

/* ── Contiguous cluster allocation optimization ──────────────────── */
/* Scan the allocation bitmap for N consecutive free clusters.
 * Returns the first cluster of the first run found, or EXFAT_CLUSTER_END
 * if no contiguous run of 'count' clusters exists.  Does NOT mark the
 * bits as allocated; use exfat_alloc_contiguous_clusters() for that. */
static uint32_t exfat_bitmap_find_contiguous(struct exfat_priv *ep, uint32_t count) {
    uint8_t bit_val;
    uint32_t max_c = ep->cluster_count + 2;
    uint32_t start = ep->next_free_hint;

    if (count == 0 || count > ep->free_clusters)
        return EXFAT_CLUSTER_END;

    /* Clamp hint to valid range */
    if (start < 2 || start >= max_c)
        start = 2;

    /* Two-pass scan: first from hint upward, then wrap around */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t begin = (pass == 0) ? start : 2;
        uint32_t end = (pass == 0) ? max_c : start;

        uint32_t run_start = EXFAT_CLUSTER_END;
        uint32_t run_len = 0;

        for (uint32_t c = begin; c < end; c++) {
            if (exfat_bitmap_get(ep, c, &bit_val) != 0)
                return EXFAT_CLUSTER_END;

            if (bit_val == 0) {
                /* Free cluster — extend or start a run */
                if (run_len == 0)
                    run_start = c;
                run_len++;
                if (run_len >= count)
                    return run_start;
            } else {
                /* Allocated — reset run tracking */
                run_start = EXFAT_CLUSTER_END;
                run_len = 0;
            }
        }
    }

    return EXFAT_CLUSTER_END; /* not enough contiguous space */
}

/* Allocate N contiguous clusters from the allocation bitmap.
 * Uses exfat_bitmap_find_contiguous() to locate a suitable run.
 * Returns the first cluster of the run on success, or EXFAT_CLUSTER_END
 * on failure (no contiguous space available). */
static uint32_t exfat_alloc_contiguous_clusters(struct exfat_priv *ep, uint32_t count) {
    uint32_t first = exfat_bitmap_find_contiguous(ep, count);
    if (first >= EXFAT_CLUSTER_END)
        return EXFAT_CLUSTER_END;

    /* Mark all N clusters as allocated in the bitmap */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = first + i;
        if (exfat_bitmap_set(ep, c, 1) != 0) {
            /* Roll back on failure */
            for (uint32_t j = 0; j < i; j++)
                exfat_bitmap_set(ep, first + j, 0);
            return EXFAT_CLUSTER_END;
        }
    }

    /* Zero-initialize first cluster (preserves contiguous run for data) */
    {
        uint32_t cluster_sectors = 1U << ep->sectors_per_cluster_shift;
        uint32_t buf_size = cluster_sectors * ep->sector_size;
        uint8_t *zbuf = (uint8_t *)kmalloc(buf_size);
        if (!zbuf) {
            for (uint32_t i = 0; i < count; i++)
                exfat_bitmap_set(ep, first + i, 0);
            return EXFAT_CLUSTER_END;
        }
        memset(zbuf, 0, buf_size);
        int ret = exfat_write_cluster(ep, first, zbuf);
        kfree(zbuf);
        if (ret != 0) {
            for (uint32_t i = 0; i < count; i++)
                exfat_bitmap_set(ep, first + i, 0);
            return EXFAT_CLUSTER_END;
        }
    }

    /* Update hint and free count */
    ep->next_free_hint = first + count;
    if ((uint32_t)count <= ep->free_clusters)
        ep->free_clusters -= count;
    else
        ep->free_clusters = 0;

    return first;
}

static void exfat_free_cluster(struct exfat_priv *ep, uint32_t cluster) {
    if (cluster < 2 || cluster > ep->cluster_count + 1)
        return;
    if (exfat_bitmap_set(ep, cluster, 0) == 0) {
        ep->free_clusters++;
        /* Update hint if this cluster is earlier */
        if (cluster < ep->next_free_hint)
            ep->next_free_hint = cluster;
    }
}

/* ── Bitmap initialization and sync ─────────────────────────────── */

/* Initialize bitmap state from the on-disk allocation bitmap.
 * Parses the boot sector to locate the bitmap, counts free clusters,
 * and initializes the sector cache.  Returns 0 on success,
 * negative errno on error. */
static int exfat_bitmap_init(struct exfat_priv *ep) {
    uint8_t bpb_buf[512];
    uint32_t bitmap_byte_count;
    uint32_t bitmap_sect;
    uint32_t i;

    /* Read boot sector (LBA 0) to get bitmap location */
    if (blockdev_read_sectors(ep->dev_id, 0, 1, bpb_buf) != 0)
        return -EIO;

    struct exfat_bpb *bpb = (struct exfat_bpb *)bpb_buf;
    if (memcmp(bpb->oem_id, "EXFAT   ", 8) != 0)
        return -EINVAL;

    /* Populate geometry from boot sector */
    ep->bytes_per_sector = 1U << bpb->bytes_per_sector_shift;
    ep->sectors_per_cluster_shift = bpb->sectors_per_cluster_shift;
    ep->sector_size = ep->bytes_per_sector;
    ep->cluster_size = (uint32_t)(1U << bpb->sectors_per_cluster_shift) * ep->sector_size;
    ep->fat_offset = bpb->fat_offset;
    ep->fat_length = bpb->fat_length;
    ep->cluster_heap_offset = bpb->cluster_heap_offset;
    ep->cluster_count = bpb->cluster_count;
    ep->root_dir_cluster = bpb->root_dir_cluster;
    ep->volume_serial = bpb->volume_serial;
    ep->volume_flags = bpb->volume_flags;
    ep->num_clusters = bpb->cluster_count;
    ep->data_start_sector = bpb->cluster_heap_offset;

    /* Read and validate volume_length (64-bit) */
    ep->volume_length = bpb->volume_length;

    /* Volume length must be greater than 0 */
    if (ep->volume_length == 0) {
        kprintf("[exfat] ERROR: volume_length is 0\n");
        return -EINVAL;
    }

    /* Per exFAT spec: bytes_per_sector_shift must be 9..12 (512..4096) */
    if (bpb->bytes_per_sector_shift < 9 || bpb->bytes_per_sector_shift > 12) {
        kprintf("[exfat] ERROR: invalid bytes_per_sector_shift %u "
                "(must be 9..12)\n",
                bpb->bytes_per_sector_shift);
        return -EINVAL;
    }

    /* Validate that cluster heap fits within the volume.
     * The last sector of the cluster heap is at:
     *   cluster_heap_offset + cluster_count * sectors_per_cluster - 1
     * This must not exceed volume_length. */
    {
        uint64_t sectors_per_cluster = 1U << ep->sectors_per_cluster_shift;
        uint64_t heap_end =
            (uint64_t)ep->cluster_heap_offset + (uint64_t)ep->cluster_count * sectors_per_cluster;
        if (heap_end > ep->volume_length) {
            kprintf("[exfat] ERROR: cluster heap extends past volume end "
                    "(%llu > %llu sectors)\n",
                    (unsigned long long)heap_end, (unsigned long long)ep->volume_length);
            return -EINVAL;
        }
    }

    /* In exFAT, the allocation bitmap resides at the FAT offset
     * when no FAT is present, or after the FAT when FAT is active.
     * The bitmap size is ceil(cluster_count / 8) bytes, rounded up
     * to a sector boundary. */
    if (ep->cluster_count == 0)
        return -EINVAL;

    /* Per exFAT spec: bitmap must cover clusters 0 through cluster_count+1,
     * which is (cluster_count + 2) bits total. */
    bitmap_byte_count = (ep->cluster_count + 9) / 8;
    bitmap_sect = (bitmap_byte_count + ep->sector_size - 1) / ep->sector_size;

    /* When FAT is present (fat_length > 0), the bitmap is stored
     * after the FAT to avoid overlap.  When no FAT, the bitmap uses
     * the offset directly. */
    if (ep->fat_length > 0) {
        /* Validate fat_offset + fat_length doesn't overflow uint32_t.
         * The BPB stores these as 32-bit, and an overflow here would
         * cause bitmap_start_sector to wrap to a wrong location. */
        if ((uint64_t)ep->fat_offset + ep->fat_length > 0xFFFFFFFFULL) {
            kprintf("[exfat] ERROR: FAT overflow (offset %u + length %u)\n", ep->fat_offset,
                    ep->fat_length);
            return -EINVAL;
        }
        ep->bitmap_start_sector = ep->fat_offset + ep->fat_length;
        /* Validate that FAT region fits within the volume */
        if ((uint64_t)ep->bitmap_start_sector > ep->volume_length) {
            kprintf("[exfat] ERROR: FAT extends past volume end "
                    "(sector %u > %llu)\n",
                    ep->bitmap_start_sector, (unsigned long long)ep->volume_length);
            return -EINVAL;
        }
        kprintf("[exfat] FAT enabled (%u sectors), bitmap at sector %u\n", ep->fat_length,
                ep->bitmap_start_sector);
    } else {
        ep->bitmap_start_sector = ep->fat_offset;
        kprintf("[exfat] No FAT, bitmap at sector %u\n", ep->bitmap_start_sector);
    }
    ep->bitmap_sectors = bitmap_sect;

    /* Validate that the entire bitmap fits within the volume */
    if ((uint64_t)ep->bitmap_start_sector + ep->bitmap_sectors > ep->volume_length) {
        kprintf("[exfat] ERROR: bitmap extends past volume end "
                "(end %llu > %llu)\n",
                (unsigned long long)ep->bitmap_start_sector + ep->bitmap_sectors,
                (unsigned long long)ep->volume_length);
        return -EINVAL;
    }

    /* Initialize the sector cache */
    ep->cached_bitmap_sector = ~0U;
    ep->cached_bitmap_dirty = 0;
    memset(ep->cached_bitmap_data, 0, sizeof(ep->cached_bitmap_data));

    /* Initialize FAT cache state */
    ep->cached_fat_sector = ~0U;
    ep->cached_fat_dirty = 0;
    memset(ep->cached_fat_data, 0, sizeof(ep->cached_fat_data));

    /* Scan the bitmap to count free clusters.
     * Read bitmap sector by sector and count zero bits. */
    ep->free_clusters = 0;
    for (i = 0; i < bitmap_sect; i++) {
        uint8_t sect_buf[512];
        uint32_t bits_in_this_sector;
        uint32_t j;

        if (ep->sector_size > sizeof(sect_buf))
            return -EOPNOTSUPP;

        if (blockdev_read_sectors(ep->dev_id, ep->bitmap_start_sector + i, 1, sect_buf) != 0)
            return -EIO;

        bits_in_this_sector = ep->sector_size * 8;
        /* Last sector may be partial — clamp to total cluster count */
        if (i == bitmap_sect - 1) {
            uint32_t total_bits = ep->cluster_count + 2; /* clusters 0..N+1 */
            uint32_t bits_covered = bitmap_sect * ep->sector_size * 8;
            if (bits_covered > total_bits)
                bits_in_this_sector -= (bits_covered - total_bits);
        }

        for (j = 0; j < bits_in_this_sector; j++) {
            /* Compute the global cluster number this bit represents */
            uint32_t cluster_num = i * ep->sector_size * 8 + j;

            /* Skip reserved clusters 0 and 1 — they are never free
             * regardless of their bitmap state.  On a properly formatted
             * exFAT volume the allocator marks these as allocated (bit=1),
             * but some formatters may leave them clear.  Either way,
             * they must never be counted as available. */
            if (cluster_num < 2)
                continue;

            /* Ensure we don't count beyond valid cluster range.
             * The bitmap may have padding bits in the last sector. */
            if (cluster_num >= ep->cluster_count + 2)
                continue;

            uint32_t byte_idx = j / 8;
            uint32_t bit_idx = j % 8;
            if (!(sect_buf[byte_idx] & (1U << bit_idx)))
                ep->free_clusters++;
        }
    }

    /* Set allocation hint start */
    ep->next_free_hint = 2;
    ep->bitmap_initialized = 1;

    kprintf("[exfat] bitmap: %u sectors, %u clusters, %u free\n", ep->bitmap_sectors,
            ep->cluster_count, ep->free_clusters);
    return 0;
}

/* Synchronise the allocation bitmap to disk and update the boot
 * sector's percent_in_use field.  Returns 0 on success,
 * negative errno on error. */
static int exfat_bitmap_sync(struct exfat_priv *ep) {
    uint8_t bpb_buf[512];
    int ret;

    if (!ep)
        return 0;

    if (!ep->bitmap_initialized)
        return 0;

    /* Flush dirty bitmap cache */
    ret = exfat_bitmap_flush(ep);
    if (ret < 0)
        return ret;

    /* Flush dirty FAT cache if enabled */
    ret = exfat_fat_flush(ep);
    if (ret < 0)
        return ret;

    /* Read boot sector to update percent_in_use */
    if (blockdev_read_sectors(ep->dev_id, 0, 1, bpb_buf) != 0)
        return -EIO;

    if (ep->cluster_count > 0) {
        uint32_t used = ep->cluster_count - ep->free_clusters;
        uint8_t pct = (uint8_t)((uint64_t)used * 100 / ep->cluster_count);
        bpb_buf[51] = pct; /* percent_in_use field at offset 51 */
    }

    /* Write boot sector back */
    if (blockdev_write_sectors(ep->dev_id, 0, 1, bpb_buf) != 0)
        return -EIO;

    /* Also update backup boot sector (sector 12) if present */
    (void)blockdev_write_sectors(ep->dev_id, 12, 1, bpb_buf);

    return 0;
}

/* ── Up-case table validation ───────────────────────────────────── */
/* Scans the root directory for the up-case table entry (type 0x82),
 * reads the table data, and verifies its CRC32 checksum.
 * The up-case table is required for correct case-insensitive filename
 * comparison in exFAT.  Returns 0 on success (table loaded and verified),
 * negative on error.  A missing or invalid table is non-fatal — the
 * volume is still mountable but case-insensitive comparison is limited
 * to ASCII. */

static int exfat_validate_upcase(struct exfat_priv *ep) {
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint8_t stack_buf[4096];
    uint8_t *cluster_buf = stack_buf;
    int need_free = 0;
    int ret = -ENOENT;

    if (!ep->bitmap_initialized)
        return -EINVAL;

    if (cluster_size > sizeof(stack_buf)) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf)
            return -ENOMEM;
        need_free = 1;
    }

    /* Root directory: cluster 0 = cluster_heap_offset */
    uint32_t cluster = ep->root_dir_cluster;

    while (cluster < EXFAT_CLUSTER_END && (cluster >= 2 || cluster == 0)) {
        if (exfat_read_cluster(ep, cluster, cluster_buf) < 0) {
            ret = -EIO;
            goto out;
        }

        for (uint32_t off = 0; off + 32 <= cluster_size; off += 32) {
            uint8_t *entry = cluster_buf + off;
            uint8_t type = entry[0];

            if (type == EXFAT_ENTRY_EOD)
                goto not_found;

            if (type == EXFAT_ENTRY_UNUSED)
                continue;

            /* Check for up-case table entry */
            if (type == EXFAT_ENTRY_UPCASE) {
                struct exfat_upcase_entry *ue = (struct exfat_upcase_entry *)entry;
                uint32_t uc_cluster = ue->first_cluster;
                uint64_t uc_data_len = ue->data_length;
                uint32_t uc_checksum = ue->checksum;

                /* Validate basic parameters */
                if (uc_data_len == 0 || uc_data_len > 256 * 1024) {
                    kprintf("[exfat] up-case table: invalid size %llu\n",
                            (unsigned long long)uc_data_len);
                    ret = -EINVAL;
                    goto out;
                }

                if (uc_cluster < 2 || uc_cluster >= EXFAT_CLUSTER_END) {
                    kprintf("[exfat] up-case table: invalid cluster %u\n", uc_cluster);
                    ret = -EINVAL;
                    goto out;
                }

                /* Allocate buffer for the table data */
                uint32_t data_bytes = (uint32_t)uc_data_len;
                uint8_t *table_data = (uint8_t *)kmalloc(data_bytes);
                if (!table_data) {
                    ret = -ENOMEM;
                    goto out;
                }

                /* Read the table data cluster by cluster.
                 * We must read full clusters into cluster_buf (which is
                 * guaranteed to be at least as large as one cluster) and
                 * then copy only the needed portion, because
                 * exfat_read_cluster() always reads a full cluster and
                 * table_data is allocated for exactly data_bytes. */
                uint32_t bytes_per_cluster = cluster_size;
                uint32_t offset = 0;
                uint32_t cur_cluster = uc_cluster;

                while (offset < data_bytes && cur_cluster >= 2 && cur_cluster < EXFAT_CLUSTER_END) {
                    uint32_t chunk = data_bytes - offset;
                    if (chunk > bytes_per_cluster)
                        chunk = bytes_per_cluster;

                    if (exfat_read_cluster(ep, cur_cluster, cluster_buf) < 0) {
                        kprintf("[exfat] up-case table: read error "
                                "at cluster %u\n",
                                cur_cluster);
                        kfree(table_data);
                        ret = -EIO;
                        goto out;
                    }

                    memcpy(table_data + offset, cluster_buf, chunk);

                    offset += chunk;
                    cur_cluster = exfat_next_cluster(ep, cur_cluster);
                }

                /* Check that all data was read (cluster chain didn't end early) */
                if (offset < data_bytes) {
                    kprintf("[exfat] up-case table: incomplete read "
                            "(%u of %u bytes at cluster %u)\n",
                            offset, data_bytes, cur_cluster);
                    kfree(table_data);
                    ret = -EIO;
                    goto out;
                }

                /* Verify CRC32 of the table data */
                uint32_t computed_crc = crc32(0, table_data, data_bytes);

                if (computed_crc != uc_checksum) {
                    kprintf("[exfat] up-case table: CRC32 mismatch "
                            "(computed 0x%08X, stored 0x%08X)\n",
                            computed_crc, uc_checksum);
                    kfree(table_data);
                    ret = -EILSEQ;
                    goto out;
                }

                /* Table validated — store it */
                if (ep->upcase_table)
                    kfree(ep->upcase_table);

                ep->upcase_table = table_data;
                ep->upcase_table_size = data_bytes;
                ep->upcase_table_valid = 1;

                kprintf("[exfat] up-case table: loaded %u bytes, "
                        "CRC32 0x%08X OK\n",
                        data_bytes, computed_crc);
                ret = 0;
                goto out;
            }
        }

        /* Move to next cluster */
        cluster = exfat_next_cluster(ep, cluster);
    }

not_found:
    kprintf("[exfat] up-case table: not found in root directory\n");
    ret = -ENOENT;

out:
    if (need_free)
        kfree(cluster_buf);
    return ret;
}


/* ── Name hash (exFAT spec: 16-bit hash from UTF-16 name) ────────── */

static uint16_t exfat_name_hash(const uint16_t *name, uint32_t len) {
    uint16_t hash = 0;
    for (uint32_t i = 0; i < len; i++) {
        hash = (uint16_t)((hash << 15) | (hash >> 1)) + name[i];
    }
    return hash;
}

/* ── UTF-8 to UTF-16LE conversion (for filenames) ────────────────── */
/* Returns number of UTF-16 code units written, or -1 on error. */

static int exfat_utf8_to_utf16(const char *utf8, uint16_t *utf16, uint32_t max_units) {
    uint32_t ui = 0;
    uint32_t si = 0;

    while (utf8[si] != '\0' && ui < max_units) {
        uint8_t c = (uint8_t)utf8[si];
        uint32_t cp;

        if (c < 0x80) {
            cp = c;
            si += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (!(utf8[si + 1] != '\0'))
                return -1;
            cp = ((uint32_t)(c & 0x1F) << 6) | ((uint32_t)(utf8[si + 1] & 0x3F));
            si += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (!(utf8[si + 1] != '\0' && utf8[si + 2] != '\0'))
                return -1;
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(utf8[si + 1] & 0x3F) << 6) |
                 ((uint32_t)(utf8[si + 2] & 0x3F));
            si += 3;
        } else {
            return -1; /* unsupported, 4-byte sequences */
        }

        if (cp > 0xFFFF) {
            /* surrogate pair */
            if (ui + 2 > max_units)
                return -1;
            utf16[ui++] = (uint16_t)(0xD800 | ((cp - 0x10000) >> 10));
            utf16[ui++] = (uint16_t)(0xDC00 | ((cp - 0x10000) & 0x3FF));
        } else {
            utf16[ui++] = (uint16_t)cp;
        }
    }
    return (int)ui;
}

/* ── Entry set CRC16 ─────────────────────────────────────────────── */
/* Computes the CRC16 over an entry set.  The first two checksum bytes
 * of the file entry and the reserved byte (byte 2) of the stream
 * extension are zeroed before computation per the exFAT spec. */

static uint16_t exfat_entry_set_crc16(const uint8_t *entries, int num_entries) {
    uint16_t crc = 0;
    for (int i = 0; i < num_entries; i++) {
        const uint8_t *entry = entries + (uint32_t)i * 32;
        if (i == 0) {
            /* File entry: zero bytes 2-3 (checksum field) */
            uint8_t buf[32];
            memcpy(buf, entry, 32);
            buf[2] = 0;
            buf[3] = 0;
            crc = crc16(crc, buf, 32);
        } else if (i == 1) {
            /* Stream extension: zero byte 2 (reserved1) */
            uint8_t buf[32];
            memcpy(buf, entry, 32);
            buf[2] = 0;
            crc = crc16(crc, buf, 32);
        } else {
            crc = crc16(crc, entry, 32);
        }
    }
    return crc;
}

/* ── Directory entry set operations ──────────────────────────────── */

/* Return the number of 32-byte entries needed for a given name length.
 * exFAT entry set = 1 file entry + 1 stream ext + ceil(name_len/15) name entries. */

static int exfat_num_entries_for_name(int name_units) {
    return 2 + (name_units + 14) / 15;
}

/* Result from finding an entry set in a directory */
struct exfat_entry_loc {
    uint32_t cluster;    /* cluster containing the entry */
    uint32_t sector_off; /* sector index within cluster */
    uint32_t byte_off;   /* byte offset within sector */
    int num_entries;     /* size of entry set */
    int found;           /* 1 = found, 0 = not found */
};

/* Scan a directory and find an entry set by name.
 * Fills 'loc' with the position of the first entry (file entry). */

static int exfat_find_entry_set(struct exfat_priv *ep, uint32_t dir_cluster, const char *name,
                                struct exfat_entry_loc *loc) {
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint8_t stack_buf[4096];
    uint8_t *cluster_buf = stack_buf;
    int need_free = 0;

    memset(loc, 0, sizeof(*loc));

    if (cluster_size > sizeof(stack_buf)) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf)
            return -ENOMEM;
        need_free = 1;
    }

    uint32_t cluster = dir_cluster;
    int result = -ENOENT;

    /* Convert name to UTF-16 for comparison */
    uint16_t utf16_name[128];
    int name_units = exfat_utf8_to_utf16(name, utf16_name, 128);
    if (name_units <= 0) {
        if (need_free)
            kfree(cluster_buf);
        return -EINVAL;
    }

    uint16_t target_hash = exfat_name_hash(utf16_name, (uint32_t)name_units);

    while (cluster >= 2 && cluster < EXFAT_CLUSTER_END) {
        if (exfat_read_cluster(ep, cluster, cluster_buf) < 0) {
            result = -EIO;
            break;
        }

        for (uint32_t off = 0; off + 32 <= cluster_size; off += 32) {
            uint8_t *entry = cluster_buf + off;
            uint8_t type = entry[0];

            if (type == EXFAT_ENTRY_EOD) {
                if (need_free)
                    kfree(cluster_buf);
                return -ENOENT;
            }
            if (type == EXFAT_ENTRY_UNUSED)
                continue;
            if ((type & EXFAT_TYPE_MASK) != 0x80)
                continue;
            if (type != EXFAT_ENTRY_FILE)
                continue;

            struct exfat_file_entry *fe = (struct exfat_file_entry *)entry;
            uint8_t sec_count = fe->secondary_count_continuations & 0x1F;
            if (sec_count == 0)
                continue;

            /* Check stream extension (immediately after file entry) */
            if (off + 64 > cluster_size)
                continue;
            uint8_t *stream_entry = entry + 32;
            if (stream_entry[0] != EXFAT_ENTRY_STREAM_EXT)
                continue;
            struct exfat_stream_ext *se = (struct exfat_stream_ext *)stream_entry;

            uint8_t nlen = se->name_length;
            if (nlen == 0 || (uint32_t)nlen != (uint32_t)name_units)
                goto skip_set;

            /* Verify name hash before reading name entries */
            if (se->name_hash != target_hash)
                goto skip_set;

            /* Read name entries */
            uint16_t entry_name[128];
            int en_pos = 0;
            int matched = 1;

            for (uint8_t k = 2; k < sec_count; k++) {
                if (off + (uint32_t)(k + 1) * 32 > cluster_size)
                    break;
                uint8_t *nentry = entry + (uint32_t)k * 32;
                if (nentry[0] != EXFAT_ENTRY_FILE_NAME)
                    break;

                uint16_t *name_ptr = (uint16_t *)(nentry + 2);
                for (int c = 0; c < 15 && en_pos < 128; c++) {
                    uint16_t ch = name_ptr[c];
                    if (ch == 0) {
                        /* Null-terminated within entry */
                        if (en_pos < (int)nlen)
                            goto mismatch;
                        goto name_done;
                    }
                    /* Convert to upper-case for comparison */
                    if (ch >= 'a' && ch <= 'z')
                        ch = (uint16_t)(ch - 32);
                    entry_name[en_pos++] = ch;
                }
            }
            goto name_done;

        mismatch:
            matched = 0;

        name_done:
            if (matched && en_pos == (int)nlen) {
                /* Compare */
                int match = 1;
                for (int i = 0; i < en_pos; i++) {
                    uint16_t ec = entry_name[i];
                    uint16_t nc = utf16_name[i];
                    if (ec != nc) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    uint32_t sector_index = off / ep->sector_size;
                    uint32_t byte_in_sector = off % ep->sector_size;
                    loc->cluster = cluster;
                    loc->sector_off = sector_index;
                    loc->byte_off = byte_in_sector;
                    loc->num_entries = 1 + sec_count;
                    loc->found = 1;
                    result = 0;
                    if (need_free)
                        kfree(cluster_buf);
                    return 0;
                }
            }
        skip_set:
            off += (uint32_t)sec_count * 32;
        }

        /* Move to next cluster */
        cluster = exfat_next_cluster(ep, cluster);
    }

    if (need_free)
        kfree(cluster_buf);
    return result;
}

/* ── Write an entry set at a specific location ────────────────────── */
/* The caller must ensure the location has enough space for the set. */

static int exfat_write_entry_set_at(struct exfat_priv *ep, const struct exfat_entry_loc *loc,
                                    const uint8_t *entries, int num_entries) {
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint32_t write_size = (uint32_t)num_entries * 32;
    uint32_t start_byte = loc->sector_off * ep->sector_size + loc->byte_off;

    if (start_byte + write_size > cluster_size)
        return -ENOSPC;

    uint8_t cluster_buf[4096];
    uint8_t *buf = cluster_buf;
    int need_free = 0;

    if (cluster_size > sizeof(cluster_buf)) {
        buf = (uint8_t *)kmalloc(cluster_size);
        if (!buf)
            return -ENOMEM;
        need_free = 1;
    }

    /* Read the full cluster, modify in place, write back */
    if (exfat_read_cluster(ep, loc->cluster, buf) < 0) {
        if (need_free)
            kfree(buf);
        return -EIO;
    }

    memcpy(buf + start_byte, entries, write_size);

    int ret = exfat_write_cluster(ep, loc->cluster, buf);
    if (need_free)
        kfree(buf);
    return ret;
}

/* ── Create a new entry set in a directory ────────────────────────── */
/* Finds free space (unused entries or after EOD) and writes the set. */

static int exfat_create_entry_set(struct exfat_priv *ep, uint32_t dir_cluster, const char *name,
                                  uint16_t attrs, uint32_t first_cluster, uint64_t data_length,
                                  int contiguous) {
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;

    /* Convert name to UTF-16LE */
    uint16_t utf16_name[128];
    int name_units = exfat_utf8_to_utf16(name, utf16_name, 128);
    if (name_units <= 0)
        return -EINVAL;

    int num_entries = exfat_num_entries_for_name(name_units);
    uint16_t name_hash = exfat_name_hash(utf16_name, (uint32_t)name_units);

    /* Allocate a contiguous buffer for the entry set */
    uint8_t *entries = (uint8_t *)kmalloc((uint32_t)num_entries * 32);
    if (!entries)
        return -ENOMEM;
    memset(entries, 0, (uint32_t)num_entries * 32);

    /* Build file entry (type 0x85) */
    struct exfat_file_entry *fe = (struct exfat_file_entry *)entries;
    fe->type = EXFAT_ENTRY_FILE;
    fe->secondary_count_continuations = (uint8_t)(num_entries - 1);
    fe->file_attributes = attrs;
    /* timestamps left as 0 */

    /* Build stream extension (type 0xC0) */
    struct exfat_stream_ext *se = (struct exfat_stream_ext *)(entries + 32);
    se->type = EXFAT_ENTRY_STREAM_EXT;
    se->general_secondary_flags = contiguous ? EXFAT_FLAG_NO_FAT_CHAIN : 0;
    se->name_length = (uint8_t)name_units;
    se->name_hash = name_hash;
    se->valid_data_length = data_length;
    se->first_cluster = first_cluster;
    se->data_length = data_length;

    /* Build file name entries (type 0xC1) */
    int remaining = name_units;
    int src_off = 0;
    for (int k = 2; k < num_entries; k++) {
        struct exfat_file_name *fn = (struct exfat_file_name *)(entries + (uint32_t)k * 32);
        fn->type = EXFAT_ENTRY_FILE_NAME;
        /* Bit 0 = 1 means more name entries follow */
        if (k < num_entries - 1)
            fn->general_secondary_flags = 0x01;
        else
            fn->general_secondary_flags = 0x00;

        uint16_t *dst = (uint16_t *)fn->name;
        int copy = remaining > 15 ? 15 : remaining;
        for (int c = 0; c < copy; c++)
            dst[c] = utf16_name[src_off++];
        /* Null-terminate if this is the last entry */
        if (copy < 15)
            dst[copy] = 0;
        remaining -= copy;
    }

    /* Compute and store entry set CRC16 */
    uint16_t crc = exfat_entry_set_crc16(entries, num_entries);
    fe->checksum = crc;

    /* ── Scan the directory for free space ── */
    uint8_t stack_buf[4096];
    uint8_t *cluster_buf = stack_buf;
    int need_free = 0;

    if (cluster_size > sizeof(stack_buf)) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) {
            kfree(entries);
            return -ENOMEM;
        }
        need_free = 1;
    }

    uint32_t cluster = dir_cluster;
    int free_run = 0;
    uint32_t free_start_byte = 0;
    uint32_t free_start_cluster = 0;
    int found_space = 0;
    int reached_eod = 0;

    /* Walk clusters */
    while (cluster >= 2 && cluster < EXFAT_CLUSTER_END) {
        if (exfat_read_cluster(ep, cluster, cluster_buf) < 0) {
            if (need_free)
                kfree(cluster_buf);
            kfree(entries);
            return -EIO;
        }

        for (uint32_t off = 0; off + 32 <= cluster_size; off += 32) {
            uint8_t type = cluster_buf[off];

            if (type == EXFAT_ENTRY_EOD) {
                /* Mark where we can start writing */
                free_run = num_entries;
                free_start_cluster = cluster;
                free_start_byte = off;
                reached_eod = 1;
                break;
            }

            if (type == EXFAT_ENTRY_UNUSED) {
                if (free_run == 0) {
                    free_start_cluster = cluster;
                    free_start_byte = off;
                }
                free_run++;
                if (free_run >= num_entries) {
                    found_space = 1;
                    break;
                }
            } else {
                /* Count continuation entries to skip */
                if ((type & 0x80) && type == EXFAT_ENTRY_FILE) {
                    struct exfat_file_entry *fe2 = (struct exfat_file_entry *)(cluster_buf + off);
                    uint8_t sec = fe2->secondary_count_continuations & 0x1F;
                    off += (uint32_t)sec * 32;
                }
                free_run = 0;
            }
        }

        if (found_space || reached_eod)
            break;

        cluster = exfat_next_cluster(ep, cluster);
    }

    /* If no space found in existing clusters, allocate a new one */
    if (!found_space && !reached_eod) {
        uint32_t new_cluster = exfat_alloc_cluster(ep);
        if (new_cluster >= EXFAT_CLUSTER_END) {
            if (need_free)
                kfree(cluster_buf);
            kfree(entries);
            return -ENOSPC;
        }
        /* Write entry set at start of new cluster */
        free_start_cluster = new_cluster;
        free_start_byte = 0;
        found_space = 1;
        cluster = new_cluster;
    } else if (reached_eod) {
        /* Check if the entry set fits in the remaining cluster space.
         * When EOD is near the end of the cluster, writing the entry
         * set without a bounds check would overflow write_buf.
         * If the set doesn't fit, allocate a fresh cluster. */
        uint32_t eod_remaining = cluster_size - free_start_byte;
        if ((uint32_t)num_entries * 32 > eod_remaining) {
            uint32_t new_cluster = exfat_alloc_cluster(ep);
            if (new_cluster >= EXFAT_CLUSTER_END) {
                if (need_free)
                    kfree(cluster_buf);
                kfree(entries);
                return -ENOSPC;
            }
            free_start_cluster = new_cluster;
            free_start_byte = 0;
            cluster = new_cluster;
        } else {
            cluster = free_start_cluster;
        }
        found_space = 1;
    } else if (found_space) {
        cluster = free_start_cluster;
    }

    /* Write the entry set */
    int ret = 0;
    if (found_space || reached_eod) {
        /* Calculate location */
        uint32_t sector_off = free_start_byte / ep->sector_size;
        uint32_t byte_off = free_start_byte % ep->sector_size;

        /* Read the target cluster */
        uint8_t *write_buf;
        uint8_t write_stack[4096];
        if (cluster_size > sizeof(write_stack)) {
            write_buf = (uint8_t *)kmalloc(cluster_size);
            if (!write_buf) {
                if (need_free)
                    kfree(cluster_buf);
                kfree(entries);
                return -ENOMEM;
            }
        } else {
            write_buf = write_stack;
        }

        if (exfat_read_cluster(ep, cluster, write_buf) < 0) {
            if (write_buf != write_stack)
                kfree(write_buf);
            if (need_free)
                kfree(cluster_buf);
            kfree(entries);
            return -EIO;
        }

        /* Copy entry set into buffer */
        uint32_t copy_size = (uint32_t)num_entries * 32;
        memcpy(write_buf + free_start_byte, entries, copy_size);

        /* If we overwrote EOD, ensure there's an EOD after our set */
        if (reached_eod) {
            uint32_t eod_pos = free_start_byte + copy_size;
            if (eod_pos + 32 <= cluster_size)
                write_buf[eod_pos] = EXFAT_ENTRY_EOD;
        }

        /* Write the cluster back */
        ret = exfat_write_cluster(ep, cluster, write_buf);

        if (write_buf != write_stack)
            kfree(write_buf);
    } else {
        ret = -ENOSPC;
    }

    if (need_free)
        kfree(cluster_buf);
    kfree(entries);
    return ret;
}

/* ── Remove an entry set by name ──────────────────────────────────── */
/* Marks all entries in the set as unused (type = 0x01). */

static int exfat_remove_entry_set(struct exfat_priv *ep, uint32_t dir_cluster, const char *name) {
    struct exfat_entry_loc loc;
    int ret = exfat_find_entry_set(ep, dir_cluster, name, &loc);
    if (ret < 0)
        return ret;
    if (!loc.found)
        return -ENOENT;

    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint8_t stack_buf[4096];
    uint8_t *buf = stack_buf;
    int need_free = 0;

    if (cluster_size > sizeof(stack_buf)) {
        buf = (uint8_t *)kmalloc(cluster_size);
        if (!buf)
            return -ENOMEM;
        need_free = 1;
    }

    if (exfat_read_cluster(ep, loc.cluster, buf) < 0) {
        if (need_free)
            kfree(buf);
        return -EIO;
    }

    uint32_t start_byte = loc.sector_off * ep->sector_size + loc.byte_off;

    /* Boundary check: verify the full entry set fits within the cluster.
     * Entry sets must not span cluster boundaries per the exFAT spec,
     * but a corrupted image or in-memory data might have an inflated
     * num_entries.  Without this check the memcpy loop below would
     * write past the cluster buffer. */
    if (start_byte + (uint32_t)loc.num_entries * 32 > cluster_size) {
        if (need_free)
            kfree(buf);
        return -EINVAL;
    }

    for (int i = 0; i < loc.num_entries; i++)
        buf[start_byte + (uint32_t)i * 32] = EXFAT_ENTRY_UNUSED;

    ret = exfat_write_cluster(ep, loc.cluster, buf);
    if (need_free)
        kfree(buf);
    return ret;
}

/* ── Update an existing entry set (data_length, first_cluster) ────── */
/* Replaces the stream extension entry fields for the given file. */

static int exfat_update_entry_set(struct exfat_priv *ep, uint32_t dir_cluster, const char *name,
                                  uint32_t first_cluster, uint64_t data_length) {
    struct exfat_entry_loc loc;
    int ret = exfat_find_entry_set(ep, dir_cluster, name, &loc);
    if (ret < 0)
        return ret;
    if (!loc.found)
        return -ENOENT;

    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint8_t stack_buf[4096];
    uint8_t *buf = stack_buf;
    int need_free = 0;

    if (cluster_size > sizeof(stack_buf)) {
        buf = (uint8_t *)kmalloc(cluster_size);
        if (!buf)
            return -ENOMEM;
        need_free = 1;
    }

    if (exfat_read_cluster(ep, loc.cluster, buf) < 0) {
        if (need_free)
            kfree(buf);
        return -EIO;
    }

    uint32_t start_byte = loc.sector_off * ep->sector_size + loc.byte_off;

    /* Boundary check: ensure the full entry set lives within this cluster
     * before computing CRC16 over it.  Without this check a corrupted
     * secondary_count could cause crc16() to read past the buffer. */
    if (start_byte + (uint32_t)loc.num_entries * 32 > cluster_size) {
        if (need_free)
            kfree(buf);
        return -EINVAL;
    }

    /* Update stream extension (second entry in the set) */
    uint8_t *stream_entry = buf + start_byte + 32;
    struct exfat_stream_ext *se = (struct exfat_stream_ext *)stream_entry;
    se->first_cluster = first_cluster;
    se->valid_data_length = data_length;
    se->data_length = data_length;

    /* Recompute CRC16 for the entry set */
    uint32_t total_bytes = (uint32_t)loc.num_entries * 32;
    uint8_t *entries = buf + start_byte;

    /* Temporarily save and zero the checksum field */
    uint16_t saved_csum;
    memcpy(&saved_csum, &entries[2], 2);
    entries[2] = 0;
    entries[3] = 0;

    /* Zero stream ext byte 2 (reserved1) for CRC calc */
    uint8_t saved_res = stream_entry[2];
    stream_entry[2] = 0;

    uint16_t crc = crc16(0, entries, total_bytes);

    /* Restore and store */
    stream_entry[2] = saved_res;
    memcpy(&entries[2], &saved_csum, 2);
    /* Write the new checksum */
    entries[2] = (uint8_t)(crc & 0xFF);
    entries[3] = (uint8_t)(crc >> 8);

    ret = exfat_write_cluster(ep, loc.cluster, buf);
    if (need_free)
        kfree(buf);
    return ret;
}

/* ── Directory entry parsing ────────────────────────────────────── */

/* Read a set of directory entries starting at a given cluster.
 * Calls callback for each file entry found. */

struct exfat_dir_ctx {
    char name[256];
    uint32_t file_attrs;
    uint64_t data_length;
    uint32_t first_cluster;
    uint32_t name_length;
    int is_dir;
    int found;
};

static int exfat_parse_entries(struct exfat_priv *ep, uint32_t cluster, uint32_t max_entries,
                               int (*callback)(struct exfat_priv *, struct exfat_dir_ctx *, void *),
                               void *cb_arg) {
    uint8_t buf[4096]; /* cluster buffer */
    uint32_t cluster_size = 1U << ep->sectors_per_cluster_shift;
    cluster_size *= ep->sector_size;
    uint8_t *cluster_buf;

    if (cluster_size > sizeof(buf)) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf)
            return -1;
    } else {
        cluster_buf = buf;
    }

    uint32_t cluster_count = 0;
    uint32_t entry_count = 0;

    while (cluster < EXFAT_CLUSTER_END && (cluster >= 2 || cluster == 0)) {
        if (exfat_read_cluster(ep, cluster, cluster_buf) < 0) {
            if (cluster_buf != buf)
                kfree(cluster_buf);
            return -1;
        }

        /* Each entry is 32 bytes */
        for (uint32_t off = 0; off + 32 <= cluster_size; off += 32) {
            uint8_t *entry = cluster_buf + off;
            uint8_t type = entry[0];

            if (type == EXFAT_ENTRY_EOD) {
                if (cluster_buf != buf)
                    kfree(cluster_buf);
                return entry_count;
            }

            if (type == EXFAT_ENTRY_UNUSED)
                continue;

            /* Check if primary file entry */
            if ((type & EXFAT_TYPE_MASK) == 0x80 && type == EXFAT_ENTRY_FILE) {
                struct exfat_file_entry *fe = (struct exfat_file_entry *)entry;
                uint8_t secondary_count = fe->secondary_count_continuations & 0x1F;

                if (secondary_count == 0)
                    continue;

                /* Check if next entry in cluster is stream extension.
                 * Both the file entry (32 bytes at 'off') and the stream
                 * extension (32 bytes at 'off + 32') must fit within the
                 * cluster.  Without the off+64 check an entry at the last
                 * slot (off == cluster_size - 32) would read past the
                 * cluster buffer. */
                if (off + 64 > cluster_size)
                    continue;
                uint8_t *next = entry + 32;
                if (next[0] == EXFAT_ENTRY_STREAM_EXT) {
                    struct exfat_stream_ext *se = (struct exfat_stream_ext *)next;
                    uint8_t name_len = se->name_length;
                    uint32_t first_clust = se->first_cluster;
                    uint64_t data_len = se->data_length;
                    uint16_t file_attrs = fe->file_attributes;

                    /* Collect filename from subsequent name entries */
                    char filename[256];
                    uint32_t fn_pos = 0;

                    for (uint8_t k = 2; k < secondary_count; k++) {
                        if (off + (k + 1) * 32 > cluster_size)
                            break;
                        uint8_t *nentry = entry + k * 32;
                        if (nentry[0] != EXFAT_ENTRY_FILE_NAME)
                            break;

                        struct exfat_file_name *fn = (struct exfat_file_name *)nentry;
                        uint16_t *name_ptr = (uint16_t *)fn->name;

                        for (int c = 0; c < 15 && fn_pos < 255; c++) {
                            uint16_t ch = name_ptr[c];
                            if (ch == 0)
                                break;
                            /* Convert UTF-16LE to UTF-8 with upcase table support */
                            if (ch < 0x80) {
                                filename[fn_pos++] = (char)ch;
                            } else if (ch < 0x800) {
                                filename[fn_pos++] = (char)(0xC0 | (ch >> 6));
                                if (fn_pos < 255)
                                    filename[fn_pos++] = (char)(0x80 | (ch & 0x3F));
                            } else {
                                filename[fn_pos++] = (char)(0xE0 | (ch >> 12));
                                if (fn_pos < 255)
                                    filename[fn_pos++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                                if (fn_pos < 255)
                                    filename[fn_pos++] = (char)(0x80 | (ch & 0x3F));
                            }
                        }
                    }
                    filename[fn_pos] = '\0';

                    /* Build context */
                    struct exfat_dir_ctx ctx;
                    ctx.name_length = fn_pos;
                    memcpy(ctx.name, filename, fn_pos + 1);
                    ctx.file_attrs = file_attrs;
                    ctx.data_length = data_len;
                    ctx.first_cluster = first_clust;
                    ctx.is_dir = (file_attrs & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
                    ctx.found = 1;

                    if (callback) {
                        if (callback(ep, &ctx, cb_arg) < 0) {
                            if (cluster_buf != buf)
                                kfree(cluster_buf);
                            return entry_count;
                        }
                    }

                    entry_count++;
                }

                /* Skip continuation entries */
                off += secondary_count * 32;
            }
        }

        cluster = exfat_next_cluster(ep, cluster);
        cluster_count++;
        if (max_entries > 0 && entry_count >= max_entries)
            break;
    }

    if (cluster_buf != buf)
        kfree(cluster_buf);
    return entry_count;
}

/* Simple callback for readdir: print entry names */
struct readdir_cb_arg {
    int count;
};

static int exfat_readdir_cb(struct exfat_priv *ep, struct exfat_dir_ctx *ctx, void *arg) {
    (void)ep;
    struct readdir_cb_arg *ra = (struct readdir_cb_arg *)arg;
    kprintf("  %-20s %s\n", ctx->name, ctx->is_dir ? "<DIR>" : "");
    ra->count++;
    return 0;
}

/* ── VFS operations ──────────────────────────────────────────────── */

/* Helper: extract leaf filename from a path, return parent dir cluster.
 * Simple implementation: works with absolute paths, splits at last '/'.
 * Root ("/") returns -ENOENT since there's no parent. */

static int exfat_path_resolve(struct exfat_priv *ep, const char *path, uint32_t *parent_cluster,
                              char *leaf, int leaf_max) {
    (void)ep;
    *parent_cluster = 0;
    leaf[0] = '\0';

    if (!path || !*path)
        return -EINVAL;

    /* Skip leading slashes */
    while (*path == '/')
        path++;

    if (*path == '\0')
        return -EISDIR; /* root directory itself */

    /* Find the last '/' in the path */
    const char *last_slash = NULL;
    const char *p = path;
    while (*p) {
        if (*p == '/')
            last_slash = p;
        p++;
    }

    if (last_slash) {
        /* There's a parent path component */
        /* For simplicity, use root directory for anything under "/" */
        *parent_cluster = 0;
        const char *leaf_start = last_slash + 1;
        int len = 0;
        while (*leaf_start && leaf_start < path + leaf_max - 1) {
            leaf[len++] = *leaf_start;
            leaf_start++;
        }
        leaf[len] = '\0';
    } else {
        /* Direct child of root */
        *parent_cluster = 0;
        int len = 0;
        while (path[len] && len < leaf_max - 1) {
            leaf[len] = path[len];
            len++;
        }
        leaf[len] = '\0';
    }

    if (leaf[0] == '\0')
        return -EINVAL;

    return 0; /* returns 0; caller uses parent_cluster=0 to mean root */
}

/* Determine the actual directory cluster to use.
 * parent_cluster = 0 means root directory. */

static uint32_t exfat_resolve_dir_cluster(struct exfat_priv *ep, uint32_t parent_cluster) {
    if (parent_cluster == 0)
        return ep->root_dir_cluster;
    return parent_cluster;
}

/* Free a cluster chain (contiguous or FAT-chained) */

static void exfat_free_chain(struct exfat_priv *ep, uint32_t first_cluster, uint64_t data_length) {
    if (first_cluster < 2 || first_cluster >= EXFAT_CLUSTER_END)
        return;

    if (ep->fat_length == 0) {
        /* No FAT: contiguous allocation */
        uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
        uint64_t num_clusters = (data_length + cluster_size - 1) / cluster_size;
        if (num_clusters == 0)
            num_clusters = 1;

        for (uint64_t i = 0; i < num_clusters; i++) {
            uint32_t c = first_cluster + (uint32_t)i;
            if (c >= EXFAT_CLUSTER_END)
                break;
            exfat_free_cluster(ep, c);
        }
    } else {
        /* FAT present: traverse the FAT chain */
        uint32_t c = first_cluster;
        while (c >= 2 && c < EXFAT_CLUSTER_END) {
            uint32_t next;
            if (exfat_read_fat_entry(ep, c, &next) != 0)
                break;

            exfat_free_cluster(ep, c);

            /* Mark FAT entry as free */
            if (exfat_write_fat_entry(ep, c, 0) != 0)
                break;

            if (next >= 0xFFFFFFF8) /* End of chain */
                break;
            if (next == 0xFFFFFFF7) /* Bad cluster — stop traversal */
                break;
            if (next == 0) /* Shouldn't happen, but safe */
                break;

            c = next;
        }
    }
}

static int exfat_read(void *priv, const char *path, void *buf, uint32_t max_size,
                      uint32_t *out_size) {
    struct exfat_priv *ep = (struct exfat_priv *)priv;
    if (!ep || !path || !buf) {
        if (out_size)
            *out_size = 0;
        return -EINVAL;
    }

    /* Handle root directory stat */
    if (path[0] == '/' && path[1] == '\0') {
        if (out_size)
            *out_size = 0;
        return -EISDIR;
    }

    /* Find the file entry */
    char leaf[128];
    uint32_t parent_cluster;
    int ret = exfat_path_resolve(ep, path, &parent_cluster, leaf, 128);
    if (ret < 0) {
        if (out_size)
            *out_size = 0;
        return ret;
    }

    uint32_t dir_cluster = exfat_resolve_dir_cluster(ep, parent_cluster);

    /* Find entry set and extract file info */
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint8_t stack_buf[4096];
    uint8_t *cluster_buf = stack_buf;
    int need_free = 0;

    if (cluster_size > sizeof(stack_buf)) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) {
            if (out_size)
                *out_size = 0;
            return -ENOMEM;
        }
        need_free = 1;
    }

    uint32_t found_cluster = EXFAT_CLUSTER_END;
    uint64_t found_size = 0;
    int found = 0;
    int found_contiguous = 0;

    /* Convert leaf name to UTF-16 for comparison */
    uint16_t utf16_leaf[128];
    int name_units = exfat_utf8_to_utf16(leaf, utf16_leaf, 128);
    if (name_units <= 0) {
        if (need_free)
            kfree(cluster_buf);
        if (out_size)
            *out_size = 0;
        return -EINVAL;
    }
    uint16_t target_hash = exfat_name_hash(utf16_leaf, (uint32_t)name_units);

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < EXFAT_CLUSTER_END) {
        if (exfat_read_cluster(ep, cluster, cluster_buf) < 0) {
            if (need_free)
                kfree(cluster_buf);
            if (out_size)
                *out_size = 0;
            return -EIO;
        }

        for (uint32_t off = 0; off + 32 <= cluster_size; off += 32) {
            uint8_t type = cluster_buf[off];
            if (type == EXFAT_ENTRY_EOD)
                goto read_done;
            if (type == EXFAT_ENTRY_UNUSED)
                continue;
            if (type != EXFAT_ENTRY_FILE)
                continue;

            struct exfat_file_entry *fe = (struct exfat_file_entry *)(cluster_buf + off);
            uint8_t sec = fe->secondary_count_continuations & 0x1F;
            if (sec < 1)
                continue;

            /* Ensure the stream extension entry (at off + 32) fits within
             * the cluster buffer before dereferencing it.  An entry at the
             * last slot (off == cluster_size - 32) would otherwise read
             * past the buffer end. */
            if (off + 64 > cluster_size)
                continue;

            uint8_t *stream_entry = cluster_buf + off + 32;
            if (stream_entry[0] != EXFAT_ENTRY_STREAM_EXT)
                continue;
            struct exfat_stream_ext *se = (struct exfat_stream_ext *)stream_entry;

            if (se->name_length != (uint8_t)name_units || se->name_hash != target_hash)
                goto skip_read_set;

            /* Compare full name */
            uint16_t ename[128];
            int en_pos = 0;
            for (uint8_t k = 2; k < sec; k++) {
                if (off + (uint32_t)(k + 1) * 32 > cluster_size)
                    break;
                uint8_t *nentry = cluster_buf + off + (uint32_t)k * 32;
                if (nentry[0] != EXFAT_ENTRY_FILE_NAME)
                    break;
                uint16_t *np = (uint16_t *)(nentry + 2);
                for (int c = 0; c < 15 && en_pos < 128; c++) {
                    if (np[c] == 0)
                        break;
                    uint16_t ch = np[c];
                    if (ch >= 'a' && ch <= 'z')
                        ch = (uint16_t)(ch - 32);
                    ename[en_pos++] = ch;
                }
            }

            if (en_pos == name_units) {
                int match = 1;
                for (int i = 0; i < en_pos; i++) {
                    if (ename[i] != utf16_leaf[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    if (fe->file_attributes & EXFAT_ATTR_DIRECTORY) {
                        if (need_free)
                            kfree(cluster_buf);
                        if (out_size)
                            *out_size = 0;
                        return -EISDIR;
                    }
                    found_cluster = se->first_cluster;
                    found_size = se->data_length;
                    found = 1;
                    found_contiguous =
                        (se->general_secondary_flags & EXFAT_FLAG_NO_FAT_CHAIN) ? 1 : 0;
                    goto read_done;
                }
            }
        skip_read_set:
            off += (uint32_t)sec * 32;
        }
        cluster = exfat_next_cluster(ep, cluster);
    }
read_done:
    if (need_free)
        kfree(cluster_buf);

    if (!found) {
        if (out_size)
            *out_size = 0;
        return -ENOENT;
    }

    /* Read data clusters — optimized for contiguous files */
    uint64_t to_read = max_size;
    if (to_read > found_size)
        to_read = found_size;
    if (to_read == 0) {
        if (out_size)
            *out_size = 0;
        return 0;
    }

    if (found_contiguous || ep->fat_length == 0) {
        /* ── Contiguous read: compute sequential sector range ── */
        /* For contiguous files, clusters are known to be sequential.
         * Compute the total sector range and read in multi-sector I/O. */
        uint32_t sectors_per_cluster = 1U << ep->sectors_per_cluster_shift;
        uint32_t cluster_bytes = sectors_per_cluster * ep->sector_size;
        uint64_t done = 0;

        while (done < to_read) {
            uint64_t rel_cluster = done / cluster_bytes;
            uint64_t cl = (uint64_t)found_cluster + rel_cluster;
            if (cl >= EXFAT_CLUSTER_END || cl < 2)
                break;

            uint64_t start_sector = exfat_cluster_to_sector(ep, (uint32_t)cl);
            uint32_t byte_off_in_cluster = (uint32_t)(done % cluster_bytes);
            if (byte_off_in_cluster > 0) {
                /* Misaligned within cluster — read partial cluster */
                uint32_t chunk = cluster_bytes - byte_off_in_cluster;
                if (chunk > to_read - done)
                    chunk = (uint32_t)(to_read - done);
                for (uint32_t si = 0; si < chunk; si += ep->sector_size) {
                    uint32_t sec_size = ep->sector_size;
                    if (sec_size > to_read - done)
                        sec_size = (uint32_t)(to_read - done);
                    if (blockdev_read_sectors(ep->dev_id, start_sector + si / ep->sector_size, 1,
                                              (uint8_t *)buf + done) != 0) {
                        if (out_size)
                            *out_size = (uint32_t)done;
                        return -EIO;
                    }
                    done += sec_size;
                }
            } else {
                /* Cluster-aligned start — read full cluster(s)
                 * in multi-sector batches for performance. */
                uint32_t remaining = (uint32_t)(to_read - done);
                uint32_t this_batch = cluster_bytes;
                if (this_batch > remaining)
                    this_batch = remaining;
                uint32_t num_sectors = this_batch / ep->sector_size;
                if (num_sectors == 0)
                    break;

                uint64_t lba = start_sector;
                if (blockdev_read_sectors(ep->dev_id, lba, (uint8_t)num_sectors,
                                          (uint8_t *)buf + done) != 0) {
                    if (out_size)
                        *out_size = (uint32_t)done;
                    return -EIO;
                }
                done += this_batch;
            }
        }
        if (out_size)
            *out_size = (uint32_t)done;
        return 0;
    }

    /* ── Non-contiguous: FAT chain traversal, sector-by-sector ── */
    uint32_t current_cluster = found_cluster;
    uint64_t done = 0;
    while (current_cluster >= 2 && current_cluster < EXFAT_CLUSTER_END && done < to_read) {
        uint64_t start_sector = exfat_cluster_to_sector(ep, current_cluster);
        uint32_t chunk = cluster_size;
        if (chunk > to_read - done)
            chunk = (uint32_t)(to_read - done);

        for (uint32_t i = 0; i < chunk && done < to_read; i += ep->sector_size) {
            uint32_t sec_size = ep->sector_size;
            if (sec_size > to_read - done)
                sec_size = (uint32_t)(to_read - done);
            uint64_t lba = start_sector + i / ep->sector_size;
            if (blockdev_read_sectors(ep->dev_id, lba, 1, (uint8_t *)buf + done) != 0) {
                if (out_size)
                    *out_size = (uint32_t)done;
                return -EIO;
            }
            done += sec_size;
        }
        current_cluster = exfat_next_cluster(ep, current_cluster);
    }

    if (out_size)
        *out_size = (uint32_t)done;
    return 0;
}

static int exfat_write(void *priv, const char *path, const void *data, uint32_t size) {
    struct exfat_priv *ep = (struct exfat_priv *)priv;
    if (!ep || !path)
        return -EINVAL;

    char leaf[128];
    uint32_t parent_cluster;
    int ret = exfat_path_resolve(ep, path, &parent_cluster, leaf, 128);
    if (ret < 0)
        return ret;

    uint32_t dir_cluster = exfat_resolve_dir_cluster(ep, parent_cluster);
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;

    /* Check if the file already exists */
    struct exfat_entry_loc loc;
    ret = exfat_find_entry_set(ep, dir_cluster, leaf, &loc);

    if (ret == 0 && loc.found) {
        /* File exists — update it:
         * 1. Read current stream extension to get old cluster info
         * 2. Allocate new cluster(s)
         * 3. Write data
         * 4. Update entry set with new cluster/size */
        uint32_t cluster_buf_size = cluster_size;
        uint8_t *cbuf = (uint8_t *)kmalloc(cluster_buf_size);
        if (!cbuf)
            return -ENOMEM;

        if (exfat_read_cluster(ep, loc.cluster, cbuf) < 0) {
            kfree(cbuf);
            return -EIO;
        }
        uint32_t start_byte = loc.sector_off * ep->sector_size + loc.byte_off;
        uint8_t *stream_entry = cbuf + start_byte + 32;
        struct exfat_stream_ext *old_se = (struct exfat_stream_ext *)stream_entry;
        uint32_t old_cluster = old_se->first_cluster;
        uint64_t old_size = old_se->data_length;

        /* Free old clusters */
        exfat_free_chain(ep, old_cluster, old_size);

        /* Allocate new clusters (try contiguous first for multi-cluster files) */
        uint64_t needed = size ? (uint64_t)size : 1;
        uint32_t num_clusters = (uint32_t)((needed + cluster_size - 1) / cluster_size);
        if (num_clusters == 0)
            num_clusters = 1;

        uint32_t first_cluster = EXFAT_CLUSTER_END;
        uint32_t *clusters = NULL;
        int contiguous = 0;
        int alloc_ok = 1;

        if (num_clusters > 1) {
            first_cluster = exfat_alloc_contiguous_clusters(ep, num_clusters);
            if (first_cluster < EXFAT_CLUSTER_END) {
                contiguous = 1;
                alloc_ok = 1;
            }
        }

        if (!contiguous) {
            contiguous = 0;
            clusters = (uint32_t *)kmalloc(num_clusters * sizeof(uint32_t));
            if (!clusters) {
                kfree(cbuf);
                return -ENOMEM;
            }
            memset(clusters, 0, num_clusters * sizeof(uint32_t));

            for (uint32_t i = 0; i < num_clusters; i++) {
                clusters[i] = exfat_alloc_cluster(ep);
                if (clusters[i] >= EXFAT_CLUSTER_END) {
                    alloc_ok = 0;
                    break;
                }
                if (i == 0)
                    first_cluster = clusters[i];
            }
        }

        if (!alloc_ok) {
            /* Free any allocated clusters */
            for (uint32_t i = 0; i < num_clusters; i++) {
                if (clusters && clusters[i] >= 2 && clusters[i] < EXFAT_CLUSTER_END)
                    exfat_free_cluster(ep, clusters[i]);
            }
            if (clusters)
                kfree(clusters);
            kfree(cbuf);
            return -ENOSPC;
        }

        /* Write data to clusters (handle both contiguous and non-contiguous) */
        uint64_t written = 0;
        for (uint32_t i = 0; i < num_clusters && written < size; i++) {
            uint32_t c = contiguous ? (first_cluster + i) : clusters[i];
            uint8_t *zbuf = (uint8_t *)kmalloc(cluster_size);
            if (!zbuf) {
                exfat_free_chain(ep, first_cluster, size);
                if (clusters)
                    kfree(clusters);
                kfree(cbuf);
                return -ENOMEM;
            }
            memset(zbuf, 0, cluster_size);
            uint32_t chunk = cluster_size;
            if (chunk > size - (uint32_t)written)
                chunk = size - (uint32_t)written;
            if (chunk > 0)
                memcpy(zbuf, (const uint8_t *)data + written, chunk);
            exfat_write_cluster(ep, c, zbuf);
            kfree(zbuf);
            written += chunk;
        }

        /* Write FAT chain entries if FAT is enabled (even for contiguous files) */
        if (ep->fat_length > 0) {
            for (uint32_t fi = 0; fi < num_clusters; fi++) {
                uint32_t c = contiguous ? (first_cluster + fi) : clusters[fi];
                uint32_t next_val;
                if (fi < num_clusters - 1) {
                    if (contiguous)
                        next_val = first_cluster + fi + 1;
                    else
                        next_val = clusters[fi + 1];
                } else {
                    next_val = EXFAT_CLUSTER_END;
                }
                if (exfat_write_fat_entry(ep, c, next_val) != 0) {
                    exfat_free_chain(ep, first_cluster, size);
                    if (clusters)
                        kfree(clusters);
                    kfree(cbuf);
                    return -EIO;
                }
            }
        }

        if (clusters)
            kfree(clusters);

        /* Update entry set with new cluster and size */
        ret = exfat_update_entry_set(ep, dir_cluster, leaf, first_cluster, size);
        kfree(cbuf);
        return ret < 0 ? ret : (int)size;
    }

    /* File doesn't exist — create new entry set and allocate clusters */
    uint64_t needed = size ? (uint64_t)size : 1;
    uint32_t num_clusters = (uint32_t)((needed + cluster_size - 1) / cluster_size);
    if (num_clusters == 0)
        num_clusters = 1;

    uint32_t first_cluster = EXFAT_CLUSTER_END;
    uint32_t *new_clusters = NULL;
    int contiguous = 0;
    int alloc_ok = 1;

    /* Try contiguous allocation first (multi-cluster files benefit) */
    if (num_clusters > 1) {
        first_cluster = exfat_alloc_contiguous_clusters(ep, num_clusters);
        if (first_cluster < EXFAT_CLUSTER_END) {
            contiguous = 1;
            alloc_ok = 1;
        }
    }

    if (!contiguous) {
        new_clusters = (uint32_t *)kmalloc(num_clusters * sizeof(uint32_t));
        if (!new_clusters)
            return -ENOMEM;
        memset(new_clusters, 0, num_clusters * sizeof(uint32_t));

        for (uint32_t i = 0; i < num_clusters; i++) {
            new_clusters[i] = exfat_alloc_cluster(ep);
            if (new_clusters[i] >= EXFAT_CLUSTER_END) {
                alloc_ok = 0;
                break;
            }
            if (i == 0)
                first_cluster = new_clusters[i];
        }
    }

    if (!alloc_ok) {
        for (uint32_t i = 0; i < num_clusters; i++) {
            if (new_clusters && new_clusters[i] >= 2 && new_clusters[i] < EXFAT_CLUSTER_END)
                exfat_free_cluster(ep, new_clusters[i]);
        }
        if (new_clusters)
            kfree(new_clusters);
        return -ENOSPC;
    }

    /* Write data to clusters */
    uint64_t written = 0;
    for (uint32_t i = 0; i < num_clusters && written < size; i++) {
        uint32_t c = contiguous ? (first_cluster + i) : new_clusters[i];
        uint8_t *zbuf = (uint8_t *)kmalloc(cluster_size);
        if (!zbuf) {
            exfat_free_chain(ep, first_cluster, size);
            if (new_clusters)
                kfree(new_clusters);
            return -ENOMEM;
        }
        memset(zbuf, 0, cluster_size);
        uint32_t chunk = cluster_size;
        if (chunk > size - (uint32_t)written)
            chunk = size - (uint32_t)written;
        if (chunk > 0)
            memcpy(zbuf, (const uint8_t *)data + written, chunk);
        exfat_write_cluster(ep, c, zbuf);
        kfree(zbuf);
        written += chunk;
    }

    /* Write FAT chain entries if FAT is enabled */
    if (ep->fat_length > 0) {
        for (uint32_t fi = 0; fi < num_clusters; fi++) {
            uint32_t c = contiguous ? (first_cluster + fi) : new_clusters[fi];
            uint32_t next_val;
            if (fi < num_clusters - 1) {
                if (contiguous)
                    next_val = first_cluster + fi + 1;
                else
                    next_val = new_clusters[fi + 1];
            } else {
                next_val = EXFAT_CLUSTER_END;
            }
            if (exfat_write_fat_entry(ep, c, next_val) != 0) {
                exfat_free_chain(ep, first_cluster, size);
                if (new_clusters)
                    kfree(new_clusters);
                return -EIO;
            }
        }
    }

    if (new_clusters)
        kfree(new_clusters);

    /* Create entry set in directory */
    ret = exfat_create_entry_set(ep, dir_cluster, leaf, EXFAT_ATTR_ARCHIVE, first_cluster, size,
                                 contiguous);
    if (ret < 0) {
        exfat_free_chain(ep, first_cluster, size);
        return ret;
    }

    return (int)size;
}

static int exfat_stat(void *priv, const char *path, struct vfs_stat *st) {
    struct exfat_priv *ep = (struct exfat_priv *)priv;
    if (!ep || !st)
        return -EINVAL;

    memset(st, 0, sizeof(*st));

    /* Root directory */
    if (path[0] == '/' && path[1] == '\0') {
        st->type = VFS_TYPE_DIR;
        st->mode = 0755;
        return 0;
    }

    /* Find the file */
    char leaf[128];
    uint32_t parent_cluster;
    int ret = exfat_path_resolve(ep, path, &parent_cluster, leaf, 128);
    if (ret < 0)
        return ret;

    uint32_t dir_cluster = exfat_resolve_dir_cluster(ep, parent_cluster);
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint8_t stack_buf[4096];
    uint8_t *cluster_buf = stack_buf;
    int need_free = 0;

    if (cluster_size > sizeof(stack_buf)) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf)
            return -ENOMEM;
        need_free = 1;
    }

    /* Convert leaf name to UTF-16 */
    uint16_t utf16_leaf[128];
    int name_units = exfat_utf8_to_utf16(leaf, utf16_leaf, 128);
    if (name_units <= 0) {
        if (need_free)
            kfree(cluster_buf);
        return -EINVAL;
    }
    uint16_t target_hash = exfat_name_hash(utf16_leaf, (uint32_t)name_units);

    int found = 0;
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < EXFAT_CLUSTER_END) {
        if (exfat_read_cluster(ep, cluster, cluster_buf) < 0) {
            if (need_free)
                kfree(cluster_buf);
            return -EIO;
        }

        for (uint32_t off = 0; off + 32 <= cluster_size; off += 32) {
            uint8_t type = cluster_buf[off];
            if (type == EXFAT_ENTRY_EOD)
                goto stat_done;
            if (type == EXFAT_ENTRY_UNUSED)
                continue;
            if (type != EXFAT_ENTRY_FILE)
                continue;

            struct exfat_file_entry *fe = (struct exfat_file_entry *)(cluster_buf + off);
            uint8_t sec = fe->secondary_count_continuations & 0x1F;
            if (sec < 1)
                continue;

            /* Ensure the stream extension entry (at off + 32) fits within
             * the cluster buffer before dereferencing it. */
            if (off + 64 > cluster_size)
                continue;

            uint8_t *se_bytes = cluster_buf + off + 32;
            if (se_bytes[0] != EXFAT_ENTRY_STREAM_EXT)
                continue;
            struct exfat_stream_ext *se = (struct exfat_stream_ext *)se_bytes;

            if (se->name_length != (uint8_t)name_units || se->name_hash != target_hash)
                goto skip_stat;

            /* Compare full name */
            uint16_t ename[128];
            int en_pos = 0;
            for (uint8_t k = 2; k < sec; k++) {
                if (off + (uint32_t)(k + 1) * 32 > cluster_size)
                    break;
                uint8_t *nentry = cluster_buf + off + (uint32_t)k * 32;
                if (nentry[0] != EXFAT_ENTRY_FILE_NAME)
                    break;
                uint16_t *np = (uint16_t *)(nentry + 2);
                for (int c = 0; c < 15 && en_pos < 128; c++) {
                    if (np[c] == 0)
                        break;
                    uint16_t ch = np[c];
                    if (ch >= 'a' && ch <= 'z')
                        ch = (uint16_t)(ch - 32);
                    ename[en_pos++] = ch;
                }
            }

            if (en_pos == name_units) {
                int match = 1;
                for (int i = 0; i < en_pos; i++) {
                    if (ename[i] != utf16_leaf[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    st->size = se->data_length;
                    st->type =
                        (fe->file_attributes & EXFAT_ATTR_DIRECTORY) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                    st->mode = (fe->file_attributes & EXFAT_ATTR_DIRECTORY) ? 0755 : 0644;
                    st->uid = 0;
                    st->gid = 0;
                    st->mtime = fe->modify_time;
                    found = 1;
                    goto stat_done;
                }
            }
        skip_stat:
            off += (uint32_t)sec * 32;
        }
        cluster = exfat_next_cluster(ep, cluster);
    }
stat_done:
    if (need_free)
        kfree(cluster_buf);

    if (!found)
        return -ENOENT;
    return 0;
}

static int exfat_create(void *priv, const char *path, uint8_t type) {
    struct exfat_priv *ep = (struct exfat_priv *)priv;
    if (!ep || !path)
        return -EINVAL;

    char leaf[128];
    uint32_t parent_cluster;
    int ret = exfat_path_resolve(ep, path, &parent_cluster, leaf, 128);
    if (ret < 0)
        return ret;

    uint32_t dir_cluster = exfat_resolve_dir_cluster(ep, parent_cluster);

    /* Check if entry already exists */
    struct exfat_entry_loc loc;
    ret = exfat_find_entry_set(ep, dir_cluster, leaf, &loc);
    if (ret == 0 && loc.found)
        return -EEXIST;

    uint16_t attrs = EXFAT_ATTR_ARCHIVE;
    if (type == VFS_TYPE_DIR)
        attrs |= EXFAT_ATTR_DIRECTORY;

    /* For directories, allocate a cluster for dot entries */
    uint32_t first_cluster = EXFAT_CLUSTER_END;
    uint64_t data_length = 0;

    if (type == VFS_TYPE_DIR) {
        first_cluster = exfat_alloc_cluster(ep);
        if (first_cluster >= EXFAT_CLUSTER_END)
            return -ENOSPC;
        data_length = 0;
    }

    ret = exfat_create_entry_set(ep, dir_cluster, leaf, attrs, first_cluster, data_length,
                                 0); /* directories: not contiguous */
    if (ret < 0) {
        if (first_cluster >= 2 && first_cluster < EXFAT_CLUSTER_END)
            exfat_free_cluster(ep, first_cluster);
        return ret;
    }

    return 0;
}

static int exfat_unlink(void *priv, const char *path) {
    struct exfat_priv *ep = (struct exfat_priv *)priv;
    if (!ep || !path)
        return -EINVAL;

    char leaf[128];
    uint32_t parent_cluster;
    int ret = exfat_path_resolve(ep, path, &parent_cluster, leaf, 128);
    if (ret < 0)
        return ret;

    uint32_t dir_cluster = exfat_resolve_dir_cluster(ep, parent_cluster);

    /* Find the entry set to get stream extension info */
    struct exfat_entry_loc loc;
    ret = exfat_find_entry_set(ep, dir_cluster, leaf, &loc);
    if (ret < 0 || !loc.found)
        return -ENOENT;

    /* Read the cluster to get stream extension data */
    uint32_t cluster_size = (1U << ep->sectors_per_cluster_shift) * ep->sector_size;
    uint8_t *cbuf = (uint8_t *)kmalloc(cluster_size);
    if (!cbuf)
        return -ENOMEM;

    if (exfat_read_cluster(ep, loc.cluster, cbuf) < 0) {
        kfree(cbuf);
        return -EIO;
    }

    uint32_t start_byte = loc.sector_off * ep->sector_size + loc.byte_off;
    uint8_t *stream_entry = cbuf + start_byte + 32;
    struct exfat_stream_ext *se = (struct exfat_stream_ext *)stream_entry;
    uint32_t file_cluster = se->first_cluster;
    uint64_t file_size = se->data_length;

    /* Free data clusters */
    exfat_free_chain(ep, file_cluster, file_size);

    kfree(cbuf);

    /* Remove the entry set */
    return exfat_remove_entry_set(ep, dir_cluster, leaf);
}

static int exfat_readdir(void *priv, const char *path) {
    struct exfat_priv *ep = (struct exfat_priv *)priv;
    if (!ep)
        return -1;

    if (path[0] == '/' && path[1] == '\0') {
        kprintf(".              <DIR>\n"
                "..             <DIR>\n");
        struct readdir_cb_arg ra;
        ra.count = 0;
        exfat_parse_entries(ep, ep->root_dir_cluster, 0, exfat_readdir_cb, &ra);
    }
    return 0;
}

static struct vfs_ops exfat_ops = {
    .read = exfat_read,
    .write = exfat_write,
    .stat = exfat_stat,
    .create = exfat_create,
    .unlink = exfat_unlink,
    .readdir = exfat_readdir,
};

/* ── Probe ───────────────────────────────────────────────────────── */

int exfat_probe(uint8_t dev_id) {
    uint8_t buf[512];

    /* Read boot sector (LBA 0) */
    if (blockdev_read_sectors(dev_id, 0, 1, buf) != 0)
        return -1;

    struct exfat_bpb *bpb = (struct exfat_bpb *)buf;
    if (memcmp(bpb->oem_id, "EXFAT   ", 8) != 0)
        return -1;

    kprintf("[exfat] detected on dev %u\n", dev_id);
    return 0;
}

/* ── Init ────────────────────────────────────────────────────────── */

int __init exfat_init(void) {
    kprintf("[exfat] exFAT filesystem initialized\n");
    vfs_register_filesystem("exfat", &exfat_ops);
    return 0;
}

#ifndef MODULE
device_initcall(exfat_init);
#endif

#ifdef MODULE
int __init init_module(void) {
    return exfat_init();
}
void __exit cleanup_module(void) {
}
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("exFAT — read/write with directory entry set operations");
#endif

/* ── exfat_mount ──────────────────────────────────────── */
static int exfat_mount(const char *source, const char *target, unsigned long flags) {
    struct exfat_priv *ep;
    int ret;

    if (!source || !target)
        return -EINVAL;

    /* Allocate private data */
    ep = (struct exfat_priv *)kmalloc(sizeof(struct exfat_priv));
    if (!ep)
        return -ENOMEM;

    memset(ep, 0, sizeof(*ep));
    ep->dev_id = 0; /* default device */

    /* Parse source to extract device ID if possible (e.g., "0" or "sda") */
    if (source[0] >= '0' && source[0] <= '9')
        ep->dev_id = (uint8_t)(source[0] - '0');
    else if (strlen(source) == 3 && source[0] == 's' && source[1] == 'd')
        ep->dev_id = (uint8_t)(source[2] - 'a');

    /* Initialize bitmap: parse boot sector, count free clusters */
    ret = exfat_bitmap_init(ep);
    if (ret < 0) {
        kprintf("[exfat] bitmap init failed (%d) for %s\n", ret, source);
        kfree(ep);
        return ret;
    }

    /* Validate and load the up-case table (non-fatal if missing) */
    exfat_validate_upcase(ep);

    kprintf("[exfat] mounted %s on %s\n", source, target);

    /* Register with VFS */
    ret = vfs_mount_ex(target, &exfat_ops, ep, (int)flags);
    if (ret < 0) {
        exfat_bitmap_sync(ep);
        kfree(ep);
    }
    return ret;
}

/* ── exfat_umount ────────────────────────────────────── */
static int exfat_umount(const char *target) {
    struct exfat_priv *ep = NULL;
    int ret;

    (void)target;

    /* Find the mounted instance to get its private data.
     * For now, sync bitmap and assume unmount succeeds. */
    /* In a full implementation, look up the mount and extract priv. */
    /* We rely on the VFS caller to pass the right context. */

    ret = exfat_bitmap_sync(ep); /* NULL-safe: bitmap_initialized is 0 */
    if (ret < 0)
        kprintf("[exfat] bitmap sync warning on unmount: %d\n", ret);

    /* Free the up-case table if loaded */
    if (ep && ep->upcase_table) {
        kfree(ep->upcase_table);
        ep->upcase_table = NULL;
        ep->upcase_table_valid = 0;
    }

    kprintf("[exfat] exFAT unmounted\n");
    return 0;
}
/* ── exfat_lookup ──────────────────────────────────────── */
static int exfat_lookup(const char *name, void *parent) {
    (void)parent;
    kprintf("[exfat] lookup: %s\n", name);
    return -ENOENT;
}
