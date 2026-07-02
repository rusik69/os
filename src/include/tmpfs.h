#ifndef TMPFS_H
#define TMPFS_H

#include "types.h"
#include "vfs.h"
#include "numa_mem.h"
#include "ioctl.h"

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

/* Default inode limit when none is specified (0 = unlimited). */
#define TMPFS_INODE_UNLIMITED 0U

/* ── Directory O(1) hash lookup ─────────────────────────────────── */
#define TMPFS_HASH_BUCKETS 16

/* Entry in a directory's per-directory hash table (chained) */
struct tmpfs_dirent {
    uint32_t               inode_idx;   /* index of the child inode */
    struct tmpfs_dirent   *next;        /* next entry in this bucket */
};

/* Per-directory hash table (allocated on demand for dir inodes) */
struct tmpfs_dir_htable {
    struct tmpfs_dirent   *buckets[TMPFS_HASH_BUCKETS];
};

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

    /* Huge page backing: 1 = data is a single 2MB huge page */
    int is_huge;

    /* Per-directory O(1) hash lookup table (NULL for non-directories) */
    struct tmpfs_dir_htable *dir_htable;

    /* KSM (Kernel Same-page Merging) tracking: 1 = data pages are
     * registered with KSM for scanning and potential merging */
    int ksm_registered;

    /* ── Extended attributes (user. namespace) ─────────────────────── */
    struct tmpfs_xattr_entry *xattrs;  /* dynamically allocated array */
    int xattr_count;                   /* number of used entries */
    int xattr_capacity;                /* capacity of xattrs array */
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

/* ── Quota / size-limit API ────────────────────────────────────────── */

/**
 * tmpfs_set_inode_limit() - Set the per-mount inode count quota.
 * @max_inodes:  Maximum number of inodes allowed (0 = unlimited).
 *               Clamped to TMPFS_MAX_INODES.
 * Returns 0 on success, -EINVAL if fewer inodes are currently in use.
 */
int tmpfs_set_inode_limit(uint32_t max_inodes);

/**
 * tmpfs_get_inode_limit() - Get the current inode quota.
 * Returns the configured maximum inode count (0 = unlimited).
 */
uint32_t tmpfs_get_inode_limit(void);

/**
 * tmpfs_get_used_inodes() - Get the number of inodes currently in use.
 */
uint32_t tmpfs_get_used_inodes(void);

/**
 * tmpfs_set_size_limit() - Set the per-mount data size limit.
 * @max_bytes:  Maximum data bytes allowed (0 = unlimited).
 * Returns 0 on success, -ENOSPC if already over the new limit.
 */
int tmpfs_set_size_limit(uint64_t max_bytes);

/**
 * tmpfs_get_size_limit() - Get the current data size limit.
 * Returns the configured maximum (0 = unlimited).
 */
uint64_t tmpfs_get_size_limit(void);

/**
 * tmpfs_get_used_bytes() - Get the number of data bytes currently stored.
 */
uint64_t tmpfs_get_used_bytes(void);

/**
 * tmpfs_set_quota() - Set both inode count and data size limits at once.
 * @max_inodes:  Maximum inodes (0 = unlimited).
 * @max_bytes:   Maximum data bytes (0 = unlimited).
 * Existing inode/data counts are validated against the new limits.
 * Returns 0 on success, negative errno on conflict.
 */
int tmpfs_set_quota(uint32_t max_inodes, uint64_t max_bytes);

/**
 * tmpfs_statfs() - Fill a vfs_statfs structure with tmpfs quota info.
 * @buf:  Output buffer to populate.
 * Returns 0 on success.
 */
int tmpfs_statfs(struct vfs_statfs *buf);

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

/* ── Huge page support API ──────────────────────────────────────────── */

/**
 * tmpfs_huge_get_enabled() - Check whether tmpfs huge page support is enabled.
 * Returns 1 if enabled, 0 if disabled.
 */
int tmpfs_huge_get_enabled(void);

/**
 * tmpfs_set_huge_enabled() - Enable or disable tmpfs huge page support.
 * @enabled:  1 to enable, 0 to disable.
 */
void tmpfs_set_huge_enabled(int enabled);

/**
 * tmpfs_huge_alloc() - Allocate a 2MB page for tmpfs data storage.
 * @node:  NUMA node to allocate from.
 *
 * Returns physical address of the 2MB page (zeroed), or 0 on failure.
 * Uses alloc_pages_node() with order-9 (2MB), falling back on failure.
 */
uint64_t tmpfs_huge_alloc(int node);

/**
 * tmpfs_huge_free() - Free a 2MB page previously allocated by tmpfs_huge_alloc().
 * @phys:  Physical address of the 2MB page (may be 0, in which case this is a no-op).
 */
void tmpfs_huge_free(uint64_t phys);

/**
 * tmpfs_huge_split() - Mark that a huge page was split (statistics only).
 * @phys:  Physical address of the 2MB page that was split.
 */
void tmpfs_huge_split(uint64_t phys);

/* ── Huge page statistics ──────────────────────────────────────────── */

uint64_t tmpfs_huge_get_alloc_count(void);
uint64_t tmpfs_huge_get_free_count(void);
uint64_t tmpfs_huge_get_split_count(void);
uint64_t tmpfs_huge_get_fail_count(void);

/* ── KSM (Kernel Same-page Merging) support ──────────────────────────── */

/**
 * tmpfs_register_ksm() - Register a tmpfs inode's data pages with KSM.
 * @idx:  Index of the inode to register.
 *
 * Registers the pages backing this inode with the KSM subsystem so that
 * identical pages across processes can be merged, saving memory.
 * Only works for page-allocated inodes (data_phys != 0).
 * Returns 0 on success, negative errno on failure.
 */
int tmpfs_register_ksm(int idx);

/**
 * tmpfs_unregister_ksm() - Unregister a tmpfs inode's pages from KSM.
 * @idx:  Index of the inode to unregister.
 *
 * Removes the pages from KSM tracking, typically called when the inode's
 * data is freed or reallocated.
 * Returns 0 on success, negative errno on failure.
 */
int tmpfs_unregister_ksm(int idx);

/* ── Extended attributes (user. namespace) ──────────────────────────── */

#define TMPFS_XATTR_NAME_MAX   64
#define TMPFS_XATTR_VALUE_MAX  256
#define TMPFS_XATTR_MAX_ENTRIES 4

/* A single extended attribute entry stored per-inode */
struct tmpfs_xattr_entry {
    char   name[TMPFS_XATTR_NAME_MAX];
    char   value[TMPFS_XATTR_VALUE_MAX];
    uint16_t value_size;
    int    in_use;
};

/* Set a user. extended attribute on a tmpfs inode.
 * @idx: inode index
 * @name: full xattr name including "user." prefix
 * @value: value data
 * @size: value size in bytes
 * Returns 0 on success, negative errno on failure. */
int tmpfs_xattr_set(int idx, const char *name, const void *value, size_t size);

/* Get a user. extended attribute value from a tmpfs inode.
 * @idx: inode index
 * @name: full xattr name including "user." prefix
 * @value: output buffer
 * @size: size of output buffer
 * Returns number of bytes written on success, negative errno on failure. */
int tmpfs_xattr_get(int idx, const char *name, void *value, size_t size);

/* List all user. extended attribute names on a tmpfs inode.
 * @idx: inode index
 * @buf: output buffer
 * @size: size of output buffer
 * Names are written as null-terminated strings.
 * Returns total bytes written on success, or negative errno. */
int tmpfs_xattr_list(int idx, char *buf, size_t size);

/* Remove a user. extended attribute from a tmpfs inode.
 * @idx: inode index
 * @name: full xattr name including "user." prefix
 * Returns 0 on success, -ENOENT if not found, negative errno on error. */
int tmpfs_xattr_remove(int idx, const char *name);

/* Free all extended attribute storage for a tmpfs inode.
 * Called automatically by free_inode(). */
void tmpfs_xattr_free(int idx);

/* ── tmpfs ioctl commands ──────────────────────────────────────────── */

#define TMPFS_IOC_MADVISE_MERGEABLE   _IO('t', 1)  /* Register file pages with KSM for merging */
#define TMPFS_IOC_UNMERGEABLE         _IO('t', 2)  /* Unregister file pages from KSM */

/**
 * tmpfs_madvise() - Apply madvise advice to a tmpfs inode's pages.
 * @idx:    Index of the inode.
 * @advice: madvise advice value (MADV_MERGEABLE or MADV_UNMERGEABLE).
 *
 * Returns 0 on success, negative errno on failure.
 */
int tmpfs_madvise(int idx, int advice);

/**
 * tmpfs_ioctl() - Handle ioctl commands on tmpfs files.
 * @priv: Filesystem private data (unused).
 * @path: Absolute path to the file.
 * @cmd:  ioctl command number.
 * @arg:  ioctl argument (unused for MADVISE commands).
 *
 * Returns 0 on success, negative errno on failure.
 */
int tmpfs_ioctl(void *priv, const char *path, uint64_t cmd, uint64_t arg);

#endif /* TMPFS_H */
