#ifndef TMPFS_H
#define TMPFS_H

#include "types.h"
#include "vfs.h"
#include "numa_mem.h"

/*
 * tmpfs — RAM-backed filesystem for /tmp, /dev/shm, etc.
 *
 * All data lives in kernel heap memory. Supports files, directories,
 * and symbolic links. Simple flat inode table (no tree).
 *
 * Mount:
 *   vfs_mount("/tmp", &tmpfs_vfs_ops, NULL);
 *
 * Internally uses the VFS stat structure for metadata and kmalloc'd
 * buffers for file contents.
 *
 * Size limits: per-mount size quotas can be set via tmpfs_mount_with_limit().
 * When the limit is exceeded, write operations return -ENOSPC.
 */

#define TMPFS_MAX_INODES 256
#define TMPFS_MAX_NAME   64
#define TMPFS_BLOCK_SIZE 4096

/* Default size limit when none is specified (0 = unlimited). */
#define TMPFS_SIZE_UNLIMITED 0ULL

/* Inode types */
#define TMPFS_TYPE_FILE  1
#define TMPFS_TYPE_DIR   2
#define TMPFS_TYPE_LINK  3
#define TMPFS_TYPE_CHR   4  /* character device node */
#define TMPFS_TYPE_BLK   5  /* block device node */

/* Device number for mknod */
struct tmpfs_devno {
    uint16_t major;
    uint16_t minor;
};

/* ── Swap-backing per-page tracking ──────────────────────────────── */
#define TMPFS_MAX_SWAP_PAGES 32  /* covers files up to 128 KB */

struct tmpfs_swap_slot {
    int      swap_dev;       /* swap device index, -1 = slot unused */
    uint32_t swap_slot;      /* slot index on swap_dev */
};

/* In-memory inode */
struct tmpfs_inode {
    int      in_use;
    uint8_t  type;
    char     name[TMPFS_MAX_NAME];
    uint32_t size;
    uint8_t  *data;          /* file content (virtual addr of kmalloc'd or page-allocated buffer) */
    uint8_t  uid, gid;
    uint16_t mode;
    uint32_t parent;         /* index of parent dir */
    struct tmpfs_devno dev;  /* device major/minor for device nodes */

    /* NUMA-aware page-based storage tracking */
    uint64_t data_phys;      /* physical addr of page allocation (0 = kmalloc'd) */
    int      numa_node;      /* NUMA node the data pages were allocated from */

    /* Swap backing — page-out to swap device */
    struct tmpfs_swap_slot swap_map[TMPFS_MAX_SWAP_PAGES];
    uint32_t               swap_npages;   /* number of pages tracked in swap_map */
    int                    is_swapped;    /* 1 = data is entirely on swap device */
};

/* Mount an empty tmpfs — returns 0 on success */
int tmpfs_mount(void);

/* Mount tmpfs with a per-mount size limit (in bytes).
 * Writes that would exceed this limit return -ENOSPC.
 * Pass TMPFS_SIZE_UNLIMITED (0) for no limit. */
int tmpfs_mount_with_limit(uint64_t max_bytes);

/* Unmount and free all data */
int tmpfs_unmount(void);

/* VFS operations for tmpfs */
extern struct vfs_ops tmpfs_vfs_ops;

/* Initialize tmpfs subsystem */
void tmpfs_init(void);

/* ── Swap-backing API ──────────────────────────────────────────────── */

/**
 * tmpfs_swap_out_inode() - Evict a tmpfs inode's data pages to the swap device.
 * @idx:  Index of the inode to evict.
 * Returns 0 on success, negative errno on failure.
 *
 * Only works for TMPFS_TYPE_FILE inodes whose data was page-allocated
 * (data_phys != 0).  Each page is written to the swap device via
 * swap_out(), the physical pages are freed, and is_swapped is set to 1.
 * On next read/write the pages are transparently restored.
 */
int tmpfs_swap_out_inode(int idx);

/**
 * tmpfs_swap_in_inode() - Restore a swapped-out tmpfs inode's data from swap.
 * @idx:  Index of the inode to restore.
 * Returns 0 on success, negative errno on failure.
 *
 * Allocates fresh physical pages and reads each swap slot back via
 * swap_in().  Clears is_swapped and frees the swap slots.
 */
int tmpfs_swap_in_inode(int idx);

/**
 * tmpfs_try_evict() - Try to evict idle tmpfs inodes under memory pressure.
 * @target_pages:  Try to evict at least this many pages worth of data.
 *
 * Scans the inode table for file inodes that are page-allocated and not
 * already swapped, evicting the least-recently-written ones until
 * @target_pages have been freed.  Returns the number of pages actually
 * evicted, or negative errno.
 *
 * Designed to be called from the OOM / kswapd path when free memory
 * is low.
 */
int tmpfs_try_evict(int target_pages);

#endif /* TMPFS_H */
