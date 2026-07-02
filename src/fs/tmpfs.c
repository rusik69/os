#define KERNEL_INTERNAL
#include "types.h"
#include "tmpfs.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "fs.h" /* for FS_MODE_FILE, FS_MODE_DIR etc */
#include "errno.h"
#include "numa_mem.h"
#include "cpu_topology.h"
#include "page_allocator_ext.h"
#include "swap.h"

static struct tmpfs_inode inodes[TMPFS_MAX_INODES];
static int tmpfs_mounted = 0;

/* ── Per-mount size accounting ──────────────────────────────────────── */
static uint64_t tmpfs_size_limit  = TMPFS_SIZE_UNLIMITED; /* 0 = unlimited */
static uint64_t tmpfs_used_bytes  = 0;                     /* total data bytes */

/* ── Per-mount inode quota ─────────────────────────────────────────── */
static uint32_t tmpfs_max_inodes  = TMPFS_INODE_UNLIMITED; /* 0 = unlimited */
static uint32_t tmpfs_used_inodes = 0;                      /* total used inodes */

/* ── NUMA-aware page allocation helpers (forward declarations) ────── */
static uint8_t *tmpfs_alloc_pages_numa(uint32_t size, uint64_t *out_phys);
static void tmpfs_free_pages_or_kmem(uint8_t *ptr, uint64_t phys,
                                     uint32_t size);

/* ── Directory hash lookup helpers (forward declarations) ──────────── */
static uint32_t tmpfs_hash_name(const char *name, int len);
static int tmpfs_dir_htable_alloc(int dir_idx);
static void tmpfs_dir_htable_free(int dir_idx);
static int tmpfs_dir_insert(int dir_idx, int child_idx);
static void tmpfs_dir_remove(int dir_idx, int child_idx);

/* ── helpers ───────────────────────────────────────────────────── */

static int alloc_inode(void) {
    /* Check inode quota before allocating */
    if (tmpfs_max_inodes != TMPFS_INODE_UNLIMITED &&
        tmpfs_used_inodes >= tmpfs_max_inodes) {
        return -ENOSPC;
    }
    for (int i = 1; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) {
            inodes[i].in_use = 1;
            inodes[i].is_swapped = 0;
            inodes[i].swap_npages = 0;
            for (int j = 0; j < TMPFS_MAX_SWAP_PAGES; j++) {
                inodes[i].swap_map[j].swap_dev = -1;
                inodes[i].swap_map[j].swap_slot = 0;
            }
            tmpfs_used_inodes++;
            return i;
        }
    }
    return -ENOSPC;
}

static void free_inode(int idx) {
    if (idx < 0 || idx >= TMPFS_MAX_INODES) return;
    /* Subtract freed data from the used-bytes counter */
    if (inodes[idx].data && inodes[idx].size > 0) {
        if (tmpfs_used_bytes >= inodes[idx].size)
            tmpfs_used_bytes -= inodes[idx].size;
        else
            tmpfs_used_bytes = 0;
    }
    /* Free data using the appropriate method */
    if (inodes[idx].is_swapped) {
        /* Inode is swapped out — free the swap slots instead */
        for (uint32_t i = 0; i < inodes[idx].swap_npages; i++) {
            if (inodes[idx].swap_map[i].swap_dev >= 0) {
                swap_free_slot(inodes[idx].swap_map[i].swap_dev,
                               inodes[idx].swap_map[i].swap_slot);
                inodes[idx].swap_map[i].swap_dev  = -1;
                inodes[idx].swap_map[i].swap_slot = 0;
            }
        }
    } else {
        tmpfs_free_pages_or_kmem(inodes[idx].data, inodes[idx].data_phys,
                                 inodes[idx].size);
    }
    inodes[idx].in_use = 0;
    inodes[idx].data = NULL;
    inodes[idx].data_phys = 0;
    inodes[idx].size = 0;
    inodes[idx].numa_node = 0;
    inodes[idx].is_swapped = 0;
    inodes[idx].swap_npages = 0;

    /* Free per-directory hash table if this was a directory */
    if (inodes[idx].type == TMPFS_TYPE_DIR)
        tmpfs_dir_htable_free(idx);

    tmpfs_used_inodes--;
}

static int find_inode(const char *path) {
    if (!path || path[0] != '/') return -EINVAL;
    if (path[1] == '\0') return 0; /* root dir */
    const char *name = path + 1;
    /* skip trailing slash */
    int len = (int)strlen(name);
    if (len > 0 && name[len-1] == '/') len--;
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) continue;
        if ((int)strlen(inodes[i].name) == len &&
            memcmp(inodes[i].name, name, (size_t)len) == 0)
            return i;
    }
    return -EINVAL;
}

static int find_inode_in_dir(int dir_idx, const char *name) {
    if (dir_idx < 0 || dir_idx >= TMPFS_MAX_INODES) return -EINVAL;
    if (!inodes[dir_idx].in_use || inodes[dir_idx].type != TMPFS_TYPE_DIR)
        return -EINVAL;

    struct tmpfs_dir_htable *ht = inodes[dir_idx].dir_htable;
    if (!ht)
        return -EINVAL;

    int len = (int)strlen(name);
    uint32_t hash = tmpfs_hash_name(name, len);
    uint32_t bucket = hash & (TMPFS_HASH_BUCKETS - 1);

    struct tmpfs_dirent *entry = ht->buckets[bucket];
    while (entry) {
        int idx = (int)entry->inode_idx;
        if (idx >= 0 && idx < TMPFS_MAX_INODES && inodes[idx].in_use) {
            if ((int)strlen(inodes[idx].name) == len &&
                memcmp(inodes[idx].name, name, (size_t)len) == 0)
                return idx;
        }
        entry = entry->next;
    }
    return -EINVAL;
}

/* ── NUMA-aware page allocation helpers ───────────────────────────── */

/*
 * order_for_size() - Compute the page order for a given byte size.
 * Returns the smallest order such that 2^order * PAGE_SIZE >= size.
 * Clamped to MAX_ORDER-1 to avoid exceeding the maximum allocation.
 */
static inline int order_for_size(uint32_t size)
{
    int order = 0;
    uint64_t page_capacity = PAGE_SIZE;
    while (page_capacity < (uint64_t)size && order < MAX_ORDER - 1) {
        order++;
        page_capacity <<= 1;
    }
    return order;
}

/*
 * tmpfs_alloc_pages_numa() - Allocate pages for file data from the
 * local NUMA node.
 *
 * @size:     Required byte size.
 * @out_phys: Output: physical address of the allocated region, or 0
 *            on failure.  Only valid for contiguous page allocations.
 *
 * Returns a kernel virtual address pointer, or NULL on failure.
 *
 * For small allocations (< PAGE_SIZE), falls back to kmalloc.
 * For larger allocations, uses alloc_pages_node() to allocate
 * physically contiguous pages from the local NUMA node.
 */
static uint8_t *tmpfs_alloc_pages_numa(uint32_t size, uint64_t *out_phys)
{
    *out_phys = 0;

    /* Small allocations: use kmalloc (no NUMA affinity needed) */
    if (size < PAGE_SIZE / 4) {
        uint8_t *ptr = (uint8_t *)kmalloc(size < 128 ? 128 : size);
        return ptr;
    }

    /* Large allocations: use NUMA-aware page allocator */
    int node = numa_home_node();
    int order = order_for_size(size);
    uint64_t phys = alloc_pages_node(node, GFP_KERNEL | GFP_ZERO, order);
    if (phys == 0) {
        /* Fall back to kmalloc if NUMA allocation fails */
        return (uint8_t *)kmalloc(size < 128 ? 128 : size);
    }

    *out_phys = phys;
    return (uint8_t *)PHYS_TO_VIRT(phys);
}

/*
 * tmpfs_free_pages_or_kmem() - Free memory allocated by
 * tmpfs_alloc_pages_numa().
 *
 * @ptr:   Virtual address to free (may be NULL).
 * @phys:  Physical address of page allocation (0 if kmalloc'd).
 * @size:  Original allocation size (used for order calculation).
 */
static void tmpfs_free_pages_or_kmem(uint8_t *ptr, uint64_t phys,
                                     uint32_t size)
{
    if (ptr == NULL)
        return;

    if (phys != 0) {
        /* Was page-allocated */
        int order = order_for_size(size);
        free_node_pages(phys, order);
    } else {
        /* Was kmalloc'd */
        kfree(ptr);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Swap backing — page-out to swap device ──────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * tmpfs_swap_out_inode() - Evict all data pages of a file inode to the
 * swap device.
 *
 * @idx:  Index of the inode to evict.
 * Returns 0 on success, negative errno on failure.
 *
 * Each of the inode's page-sized chunks is written to the swap device
 * via swap_out().  After all pages are safely on the swap device, the
 * physical pages are freed and inodes[idx].is_swapped is set.
 *
 * Prerequisites:
 *   - The inode must be a TMPFS_TYPE_FILE with page-allocated data
 *     (data_phys != 0), and must NOT be already swapped out.
 *   - The file size must not exceed TMPFS_MAX_SWAP_PAGES * PAGE_SIZE.
 */
int tmpfs_swap_out_inode(int idx)
{
    if (idx < 0 || idx >= TMPFS_MAX_INODES)
        return -EINVAL;
    if (!inodes[idx].in_use)
        return -ENOENT;
    if (inodes[idx].type != TMPFS_TYPE_FILE)
        return -EINVAL;
    if (inodes[idx].is_swapped)
        return 0;                          /* already on swap */
    if (inodes[idx].data_phys == 0)
        return -EOPNOTSUPP;                /* kmalloc'd buffer, not swappable */

    uint32_t npages = (inodes[idx].size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > TMPFS_MAX_SWAP_PAGES)
        return -ENOSPC;                    /* file too large for our swap map */

    uint32_t i;
    for (i = 0; i < npages; i++) {
        uint64_t page_phys = inodes[idx].data_phys + (uint64_t)i * PAGE_SIZE;
        uint32_t slot;
        int dev;
        int ret = swap_out(page_phys, &slot, &dev);
        if (ret < 0) {
            /* Roll back: free slots already allocated */
            for (uint32_t j = 0; j < i; j++) {
                swap_free_slot(inodes[idx].swap_map[j].swap_dev,
                               inodes[idx].swap_map[j].swap_slot);
                inodes[idx].swap_map[j].swap_dev  = -1;
                inodes[idx].swap_map[j].swap_slot = 0;
            }
            inodes[idx].swap_npages = 0;
            return ret;
        }
        inodes[idx].swap_map[i].swap_dev  = dev;
        inodes[idx].swap_map[i].swap_slot = slot;
    }

    inodes[idx].swap_npages = npages;
    inodes[idx].is_swapped  = 1;

    /* Free the physical pages */
    int order = order_for_size(inodes[idx].size);
    free_node_pages(inodes[idx].data_phys, order);
    inodes[idx].data      = NULL;
    inodes[idx].data_phys = 0;

    return 0;
}

/*
 * tmpfs_swap_in_inode() - Restore a swapped-out inode's data from the
 * swap device back into physical memory.
 *
 * @idx:  Index of the inode to restore.
 * Returns 0 on success, negative errno on failure.
 *
 * Allocates fresh physically-contiguous pages, reads each swap slot
 * back via swap_in(), frees the swap slots, and clears is_swapped.
 * The inode is then ready for normal read/write access.
 */
int tmpfs_swap_in_inode(int idx)
{
    if (idx < 0 || idx >= TMPFS_MAX_INODES)
        return -EINVAL;
    if (!inodes[idx].in_use)
        return -ENOENT;
    if (!inodes[idx].is_swapped)
        return 0;                          /* not swapped, nothing to do */
    if (inodes[idx].swap_npages == 0)
        return -EINVAL;                    /* swapped flag set but no swap map */

    uint32_t size = inodes[idx].size;
    int order = order_for_size(size);
    uint64_t new_phys = 0;

    /* Allocate fresh pages from the local NUMA node */
    int node = numa_home_node();
    new_phys = alloc_pages_node(node, GFP_KERNEL | GFP_ZERO, order);
    if (new_phys == 0)
        return -ENOMEM;

    uint32_t npages = inodes[idx].swap_npages;
    uint32_t i;
    for (i = 0; i < npages; i++) {
        int dev  = inodes[idx].swap_map[i].swap_dev;
        uint32_t slot = inodes[idx].swap_map[i].swap_slot;
        uint64_t page_phys = new_phys + (uint64_t)i * PAGE_SIZE;

        int ret = swap_in(dev, slot, page_phys);
        if (ret < 0) {
            /* Free already-restored pages and the rest of the allocation */
            if (i > 0)
                free_node_pages(new_phys, order);
            else
                free_node_pages(new_phys, order);
            return ret;
        }
    }

    /* Free all swap slots now that data is back in memory */
    for (i = 0; i < npages; i++) {
        if (inodes[idx].swap_map[i].swap_dev >= 0) {
            swap_free_slot(inodes[idx].swap_map[i].swap_dev,
                           inodes[idx].swap_map[i].swap_slot);
            inodes[idx].swap_map[i].swap_dev  = -1;
            inodes[idx].swap_map[i].swap_slot = 0;
        }
    }

    inodes[idx].data       = (uint8_t *)PHYS_TO_VIRT(new_phys);
    inodes[idx].data_phys  = new_phys;
    inodes[idx].swap_npages = 0;
    inodes[idx].is_swapped  = 0;

    return 0;
}

/*
 * tmpfs_try_evict() - Try to evict idle tmpfs inodes under memory
 * pressure.
 *
 * @target_pages:  Minimum number of pages to evict.
 *
 * Scans the inode table for page-allocated file inodes that are not
 * already swapped out.  Evicts them until @target_pages have been
 * freed.
 *
 * Returns the number of pages actually evicted (may be less than
 * @target_pages if there aren't enough candidates), or negative errno.
 *
 * Designed to be called from the OOM / kswapd reclaim path.
 */
int tmpfs_try_evict(int target_pages)
{
    if (target_pages <= 0)
        return 0;

    int total_evicted = 0;

    for (int i = 0; i < TMPFS_MAX_INODES && total_evicted < target_pages; i++) {
        if (!inodes[i].in_use)
            continue;
        if (inodes[i].type != TMPFS_TYPE_FILE)
            continue;
        if (inodes[i].is_swapped)
            continue;
        if (inodes[i].data_phys == 0)
            continue;                        /* kmalloc'd, can't be swapped */

        uint32_t npages = (inodes[i].size + PAGE_SIZE - 1) / PAGE_SIZE;

        int ret = tmpfs_swap_out_inode(i);
        if (ret == 0) {
            total_evicted += (int)npages;
            kprintf("[tmpfs] evicted inode %d (%u pages) to swap\n",
                    i, npages);
        }
        /* On error, try the next inode */
    }

    return total_evicted;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Directory O(1) hash lookup ──────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * tmpfs_hash_name() - DJB2 hash of a fixed-length name string.
 * @name:  Pointer to the name.
 * @len:   Length of the name in bytes.
 *
 * Returns a 32-bit hash value.
 */
static inline uint32_t tmpfs_hash_name(const char *name, int len)
{
    uint32_t hash = 5381;
    for (int i = 0; i < len; i++)
        hash = ((hash << 5) + hash) + (uint8_t)name[i];
    return hash;
}

/*
 * tmpfs_dir_htable_alloc() - Allocate and initialise a per-directory
 * hash table for the given directory inode.
 * @dir_idx:  Index of the directory inode.
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 * If the directory already has a hash table, returns 0 (no-op).
 */
static int tmpfs_dir_htable_alloc(int dir_idx)
{
    if (dir_idx < 0 || dir_idx >= TMPFS_MAX_INODES)
        return -EINVAL;
    if (inodes[dir_idx].dir_htable)
        return 0; /* already allocated */

    struct tmpfs_dir_htable *ht = (struct tmpfs_dir_htable *)
        kmalloc(sizeof(struct tmpfs_dir_htable));
    if (!ht)
        return -ENOMEM;

    for (int i = 0; i < TMPFS_HASH_BUCKETS; i++)
        ht->buckets[i] = NULL;

    inodes[dir_idx].dir_htable = ht;
    return 0;
}

/*
 * tmpfs_dir_htable_free() - Free a directory's hash table and all its
 * entries.
 * @dir_idx:  Index of the directory inode.
 */
static void tmpfs_dir_htable_free(int dir_idx)
{
    if (dir_idx < 0 || dir_idx >= TMPFS_MAX_INODES)
        return;
    struct tmpfs_dir_htable *ht = inodes[dir_idx].dir_htable;
    if (!ht)
        return;

    for (int i = 0; i < TMPFS_HASH_BUCKETS; i++) {
        struct tmpfs_dirent *entry = ht->buckets[i];
        while (entry) {
            struct tmpfs_dirent *next = entry->next;
            kfree(entry);
            entry = next;
        }
        ht->buckets[i] = NULL;
    }

    kfree(ht);
    inodes[dir_idx].dir_htable = NULL;
}

/*
 * tmpfs_dir_insert() - Insert a child inode into its parent directory's
 * hash table.
 * @dir_idx:    Index of the parent directory.
 * @child_idx:  Index of the child inode to insert.
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
static int tmpfs_dir_insert(int dir_idx, int child_idx)
{
    if (dir_idx < 0 || dir_idx >= TMPFS_MAX_INODES)
        return -EINVAL;
    if (!inodes[dir_idx].in_use || inodes[dir_idx].type != TMPFS_TYPE_DIR)
        return -EINVAL;
    if (child_idx < 0 || child_idx >= TMPFS_MAX_INODES)
        return -EINVAL;
    if (!inodes[child_idx].in_use)
        return -EINVAL;

    struct tmpfs_dir_htable *ht = inodes[dir_idx].dir_htable;
    if (!ht)
        return -EINVAL;

    int len = (int)strlen(inodes[child_idx].name);
    uint32_t hash = tmpfs_hash_name(inodes[child_idx].name, len);
    uint32_t bucket = hash & (TMPFS_HASH_BUCKETS - 1);

    struct tmpfs_dirent *entry = (struct tmpfs_dirent *)
        kmalloc(sizeof(struct tmpfs_dirent));
    if (!entry)
        return -ENOMEM;

    entry->inode_idx = (uint32_t)child_idx;
    entry->next = ht->buckets[bucket];
    ht->buckets[bucket] = entry;
    return 0;
}

/*
 * tmpfs_dir_remove() - Remove a child inode from its parent directory's
 * hash table.
 * @dir_idx:    Index of the parent directory.
 * @child_idx:  Index of the child inode to remove.
 *
 * Safe to call even if the entry doesn't exist in the table.
 */
static void tmpfs_dir_remove(int dir_idx, int child_idx)
{
    if (dir_idx < 0 || dir_idx >= TMPFS_MAX_INODES)
        return;
    if (!inodes[dir_idx].in_use || inodes[dir_idx].type != TMPFS_TYPE_DIR)
        return;

    struct tmpfs_dir_htable *ht = inodes[dir_idx].dir_htable;
    if (!ht)
        return;

    int len = (int)strlen(inodes[child_idx].name);
    uint32_t hash = tmpfs_hash_name(inodes[child_idx].name, len);
    uint32_t bucket = hash & (TMPFS_HASH_BUCKETS - 1);

    struct tmpfs_dirent *entry = ht->buckets[bucket];
    struct tmpfs_dirent *prev = NULL;
    while (entry) {
        if (entry->inode_idx == (uint32_t)child_idx) {
            if (prev)
                prev->next = entry->next;
            else
                ht->buckets[bucket] = entry->next;
            kfree(entry);
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

/* ── VFS operations ────────────────────────────────────────────── */

static int tmpfs_read(void *priv, const char *path, void *buf, uint32_t max, uint32_t *out) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_FILE)
        return -EINVAL;

    /* Transparently restore from swap if needed */
    if (inodes[idx].is_swapped) {
        int ret = tmpfs_swap_in_inode(idx);
        if (ret < 0)
            return ret;
    }

    uint32_t copy = inodes[idx].size < max ? inodes[idx].size : max;
    if (copy > 0 && inodes[idx].data)
        memcpy(buf, inodes[idx].data, copy);
    *out = copy;
    return 0;
}

static int tmpfs_write(void *priv, const char *path, const void *buf, uint32_t size) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_FILE)
        return -EINVAL;

    /* Transparently restore from swap if needed */
    if (inodes[idx].is_swapped) {
        int ret = tmpfs_swap_in_inode(idx);
        if (ret < 0)
            return ret;
    }

    /* ── Size-limit enforcement ──────────────────────────────────── */
    uint32_t old_size = inodes[idx].size;
    if (tmpfs_size_limit != TMPFS_SIZE_UNLIMITED && size > old_size) {
        uint64_t delta = (uint64_t)(size - old_size);
        if (tmpfs_used_bytes + delta > tmpfs_size_limit) {
            return -ENOSPC; /* write would exceed per-mount size limit */
        }
    }

    /* Reallocate buffer if needed */
    if (inodes[idx].size < size || !inodes[idx].data) {
        /* Free old allocation first (if any) */
        if (inodes[idx].data) {
            tmpfs_free_pages_or_kmem(inodes[idx].data, inodes[idx].data_phys,
                                     inodes[idx].size);
            inodes[idx].data = NULL;
            inodes[idx].data_phys = 0;
        }

        /* Allocate new buffer with NUMA-aware page allocation */
        uint64_t new_phys = 0;
        uint8_t *new = tmpfs_alloc_pages_numa(size, &new_phys);
        if (!new) return -ENOMEM;

        inodes[idx].data = new;
        inodes[idx].data_phys = new_phys;
        inodes[idx].numa_node = numa_home_node();
    }
    memcpy(inodes[idx].data, buf, size);
    inodes[idx].size = size;

    /* Update the total used-bytes counter */
    if (size > old_size)
        tmpfs_used_bytes += (uint64_t)(size - old_size);
    else if (size < old_size)
        tmpfs_used_bytes -= (uint64_t)(old_size - size);

    return 0;
}

static int tmpfs_mkdir(const char *path) {
    if (find_inode(path) >= 0) return -EEXIST; /* exists */
    /* parent must exist */
    /* extract parent dir and basename */
    char dir[TMPFS_MAX_NAME*2], name[TMPFS_MAX_NAME];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -EEXIST; /* only root */
    int dirlen = (int)(slash - path);
    if (dirlen > (int)sizeof(dir)-1) return -EINVAL;
    memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
    if (dirlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    int parent = find_inode(dir);
    if (parent < 0) return -EINVAL;
    if (inodes[parent].type != TMPFS_TYPE_DIR) return -EINVAL;

    int len = (int)strlen(slash + 1);
    if (len > TMPFS_MAX_NAME - 1) return -EINVAL;
    memcpy(name, slash + 1, (size_t)len + 1);
    if (find_inode_in_dir(parent, name) >= 0) return -EINVAL;

    int idx = alloc_inode();
    if (idx < 0) return -EINVAL;
    inodes[idx].type = TMPFS_TYPE_DIR;
    inodes[idx].parent = (uint32_t)parent;
    memcpy(inodes[idx].name, name, (size_t)len + 1);
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    inodes[idx].data_phys = 0;
    inodes[idx].numa_node = numa_home_node();
    inodes[idx].uid = 0; inodes[idx].gid = 0;
    inodes[idx].mode = FS_MODE_DIR;

    /* Allocate per-directory hash table for the new subdirectory */
    {
        int hret = tmpfs_dir_htable_alloc(idx);
        if (hret < 0) {
            tmpfs_used_inodes--;
            inodes[idx].in_use = 0;
            return hret;
        }
    }

    /* Insert into parent directory's hash table */
    {
        int iret = tmpfs_dir_insert(parent, idx);
        if (iret < 0) {
            tmpfs_dir_htable_free(idx);
            tmpfs_used_inodes--;
            inodes[idx].in_use = 0;
            return iret;
        }
    }

    return 0;
}

static int tmpfs_create(void *priv, const char *path, uint8_t type) {
    (void)priv;
    if (find_inode(path) >= 0) return -EINVAL;
    if (type == FS_TYPE_DIR) return tmpfs_mkdir(path);
    if (type != FS_TYPE_FILE && type != FS_TYPE_LINK) return -EINVAL;

    char dir[TMPFS_MAX_NAME*2], name[TMPFS_MAX_NAME];
    const char *slash = strrchr(path, '/');
    if (!slash) return -EINVAL;
    int dirlen = (int)(slash - path);
    if (dirlen > (int)sizeof(dir)-1) return -EINVAL;
    memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
    if (dirlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    int parent = find_inode(dir);
    if (parent < 0) return -EINVAL;

    int len = (int)strlen(slash + 1);
    if (len > TMPFS_MAX_NAME - 1) return -EINVAL;
    memcpy(name, slash + 1, (size_t)len + 1);

    int idx = alloc_inode();
    if (idx < 0) return -EINVAL;
    inodes[idx].type = (type == FS_TYPE_LINK) ? TMPFS_TYPE_LINK : TMPFS_TYPE_FILE;
    inodes[idx].parent = (uint32_t)parent;
    memcpy(inodes[idx].name, name, (size_t)len + 1);
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    inodes[idx].data_phys = 0;
    inodes[idx].numa_node = numa_home_node();
    inodes[idx].uid = 0; inodes[idx].gid = 0;
    inodes[idx].mode = (type == FS_TYPE_LINK) ? 0777 : FS_MODE_FILE;
    memset(&inodes[idx].dev, 0, sizeof(inodes[idx].dev));

    /* Insert into parent directory's hash table */
    {
        int iret = tmpfs_dir_insert(parent, idx);
        if (iret < 0) {
            inodes[idx].in_use = 0;
            tmpfs_used_inodes--;
            return iret;
        }
    }

    return 0;
}

static int tmpfs_unlink(void *priv, const char *path) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0) return -EINVAL;

    /* Remove from parent directory's hash table */
    int parent = (int)inodes[idx].parent;
    if (parent >= 0 && parent < TMPFS_MAX_INODES &&
        inodes[parent].in_use && inodes[parent].type == TMPFS_TYPE_DIR) {
        tmpfs_dir_remove(parent, idx);
    }

    free_inode(idx);
    return 0;
}

static int tmpfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0) return -EINVAL;
    st->size = inodes[idx].size;
    switch (inodes[idx].type) {
        case TMPFS_TYPE_DIR:  st->type = VFS_TYPE_DIR;  break;
        case TMPFS_TYPE_LINK: st->type = VFS_TYPE_LINK; break;
        case TMPFS_TYPE_CHR:  st->type = VFS_TYPE_CHR;  break;
        case TMPFS_TYPE_BLK:  st->type = VFS_TYPE_BLK;  break;
        default:              st->type = VFS_TYPE_FILE;  break;
    }
    st->uid = inodes[idx].uid;
    st->gid = inodes[idx].gid;
    st->mode = inodes[idx].mode;
    st->dev_major = inodes[idx].dev.major;
    st->dev_minor = inodes[idx].dev.minor;
    st->ino = (uint32_t)idx;
    st->nlink = 1;
    return 0;
}

static int tmpfs_readdir(void *priv, const char *path) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_DIR) return -EINVAL;
    kprintf("tmpfs: %s\n", path);
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) continue;
        if (inodes[i].parent != (uint32_t)idx) continue;
        const char *t;
        switch (inodes[i].type) {
            case TMPFS_TYPE_DIR:  t = "D"; break;
            case TMPFS_TYPE_LINK: t = "L"; break;
            case TMPFS_TYPE_CHR:  t = "C"; break;
            case TMPFS_TYPE_BLK:  t = "B"; break;
            default:              t = "F"; break;
        }
        if (inodes[i].type == TMPFS_TYPE_CHR || inodes[i].type == TMPFS_TYPE_BLK) {
            kprintf("  [%s] %s (%d,%d)\n", t, inodes[i].name,
                    inodes[i].dev.major, inodes[i].dev.minor);
        } else {
            kprintf("  [%s] %s (%u bytes)\n", t, inodes[i].name, inodes[i].size);
        }
    }
    return 0;
}

static int tmpfs_readdir_names(void *priv, const char *path, char names[][64], int max) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_DIR) return 0;
    int count = 0;
    for (int i = 0; i < TMPFS_MAX_INODES && count < max; i++) {
        if (!inodes[i].in_use) continue;
        if (inodes[i].parent != (uint32_t)idx) continue;
        int len = (int)strlen(inodes[i].name);
        int copylen = len < 63 ? len : 63;
        memcpy(names[count], inodes[i].name, (size_t)copylen);
        names[count][copylen] = '\0';
        count++;
    }
    return count;
}

static int tmpfs_truncate(void *priv, const char *path, uint32_t len) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0) return -EINVAL;

    uint32_t old_size = inodes[idx].size;

    if (inodes[idx].is_swapped) {
        if (len == 0) {
            /* Truncate to zero: free all swap slots */
            for (uint32_t i = 0; i < inodes[idx].swap_npages; i++) {
                if (inodes[idx].swap_map[i].swap_dev >= 0) {
                    swap_free_slot(inodes[idx].swap_map[i].swap_dev,
                                   inodes[idx].swap_map[i].swap_slot);
                    inodes[idx].swap_map[i].swap_dev  = -1;
                    inodes[idx].swap_map[i].swap_slot = 0;
                }
            }
            inodes[idx].swap_npages = 0;
            inodes[idx].is_swapped  = 0;
            inodes[idx].size = 0;
        } else {
            /* Truncate to non-zero: swap in first, then truncate */
            int ret = tmpfs_swap_in_inode(idx);
            if (ret < 0)
                return ret;
            /* Fall through to normal truncate path below */
            goto do_truncate;
        }
    } else {
do_truncate:
        if (len == 0 && inodes[idx].data) {
            tmpfs_free_pages_or_kmem(inodes[idx].data, inodes[idx].data_phys,
                                     inodes[idx].size);
            inodes[idx].data = NULL;
            inodes[idx].data_phys = 0;
            inodes[idx].size = 0;
        } else if (len < inodes[idx].size) {
            inodes[idx].size = len;
        }
    }

    /* Update used-bytes counter for shrinkage */
    if (inodes[idx].size < old_size) {
        uint64_t freed = (uint64_t)(old_size - inodes[idx].size);
        if (tmpfs_used_bytes >= freed)
            tmpfs_used_bytes -= freed;
        else
            tmpfs_used_bytes = 0;
    }

    return 0;
}

/* ── Symlink support ──────────────────────────────────────────── */

static int tmpfs_symlink(void *priv, const char *target, const char *linkpath) {
    (void)priv;
    /* Create the link inode via the generic create helper */
    if (tmpfs_create(priv, linkpath, FS_TYPE_LINK) < 0)
        return -EINVAL;

    int idx = find_inode(linkpath);
    if (idx < 0) return -EINVAL;

    /* Store the target path in the inode's data buffer */
    size_t target_len = strlen(target);
    if (target_len == 0) {
        inodes[idx].data = NULL;
        inodes[idx].size = 0;
        return 0;
    }

    inodes[idx].data = kmalloc(target_len + 1);
    if (!inodes[idx].data) {
        tmpfs_unlink(priv, linkpath);
        return -ENOMEM;
    }
    memcpy(inodes[idx].data, target, target_len + 1);
    inodes[idx].size = (uint32_t)target_len;
    return 0;
}

static int tmpfs_readlink(void *priv, const char *path, char *buf, int bufsize) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_LINK)
        return -EINVAL;
    if (bufsize <= 0) return -EINVAL;

    if (!inodes[idx].data || inodes[idx].size == 0) {
        buf[0] = '\0';
        return 0;
    }

    int copy_len = (int)inodes[idx].size;
    if (copy_len >= bufsize)
        copy_len = bufsize - 1;
    memcpy(buf, inodes[idx].data, (size_t)copy_len);
    buf[copy_len] = '\0';
    return copy_len;
}

/* ── mknod support ────────────────────────────────────────────── */

static int tmpfs_mknod(void *priv, const char *path, uint16_t mode, uint16_t dev_major, uint16_t dev_minor) {
    (void)priv;
    if (find_inode(path) >= 0) return -EEXIST; /* already exists */

    char dir[TMPFS_MAX_NAME*2], name[TMPFS_MAX_NAME];
    const char *slash = strrchr(path, '/');
    if (!slash) return -EEXIST;
    int dirlen = (int)(slash - path);
    if (dirlen > (int)sizeof(dir)-1) return -EINVAL;
    memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
    if (dirlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    int parent = find_inode(dir);
    if (parent < 0 || inodes[parent].type != TMPFS_TYPE_DIR)
        return -EINVAL;

    int len = (int)strlen(slash + 1);
    if (len > TMPFS_MAX_NAME - 1) return -EINVAL;
    memcpy(name, slash + 1, (size_t)len + 1);
    if (find_inode_in_dir(parent, name) >= 0) return -EINVAL;

    int idx = alloc_inode();
    if (idx < 0) return -EINVAL;
    /* Determine device node type from mode bits */
    if (mode & S_IFCHR)
        inodes[idx].type = TMPFS_TYPE_CHR;
    else if (mode & S_IFBLK)
        inodes[idx].type = TMPFS_TYPE_BLK;
    else
        inodes[idx].type = TMPFS_TYPE_FILE;
    inodes[idx].parent = (uint32_t)parent;
    memcpy(inodes[idx].name, name, (size_t)len + 1);
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    inodes[idx].data_phys = 0;
    inodes[idx].numa_node = numa_home_node();
    inodes[idx].uid = 0; inodes[idx].gid = 0;
    inodes[idx].mode = mode;
    inodes[idx].dev.major = dev_major;
    inodes[idx].dev.minor = dev_minor;

    /* Insert into parent directory's hash table */
    {
        int iret = tmpfs_dir_insert(parent, idx);
        if (iret < 0) {
            inodes[idx].in_use = 0;
            tmpfs_used_inodes--;
            return iret;
        }
    }

    return 0;
}

/*
 * tmpfs_rename — Rename/move a file or directory in tmpfs.
 *
 * Finds the inode by old_path, then updates its name and parent
 * index to match new_path.  This is an O(n) flat-table operation.
 */
static int tmpfs_rename(void *priv, const char *old_path, const char *new_path)
{
    (void)priv;

    /* Find the old inode */
    int old_idx = find_inode(old_path);
    if (old_idx < 0) return -ENOENT;

    /* Remember old parent before we modify anything */
    int old_parent = (int)inodes[old_idx].parent;

    /* Ensure the new path doesn't already exist */
    if (find_inode(new_path) >= 0) return -EEXIST;

    /* Extract the new leaf name (last component after '/') */
    const char *new_name = new_path;
    const char *last_slash = strrchr(new_path, '/');
    if (last_slash) new_name = last_slash + 1;

    int name_len = (int)strlen(new_name);
    if (name_len == 0) return -EINVAL;
    if (name_len >= TMPFS_MAX_NAME) return -ENAMETOOLONG;

    /* Determine the new parent directory */
    char parent_path[128];
    int new_parent = old_idx; /* default: keep same parent */

    if (last_slash && last_slash > new_path) {
        /* Extract the directory portion of new_path */
        size_t dir_len = (size_t)(last_slash - new_path);
        if (dir_len >= sizeof(parent_path)) return -ENAMETOOLONG;
        memcpy(parent_path, new_path, dir_len);
        parent_path[dir_len] = '\0';
        new_parent = find_inode(parent_path);
        if (new_parent < 0) return -ENOENT;
    } else if (last_slash == new_path) {
        /* New path is in root directory */
        new_parent = 0;
    }

    /* Remove from old parent's hash table (using the old name) */
    if (old_parent >= 0 && old_parent < TMPFS_MAX_INODES &&
        inodes[old_parent].in_use &&
        inodes[old_parent].type == TMPFS_TYPE_DIR) {
        tmpfs_dir_remove(old_parent, old_idx);
    }

    /* Update the inode */
    memcpy(inodes[old_idx].name, new_name, (size_t)name_len);
    inodes[old_idx].name[name_len] = '\0';
    inodes[old_idx].parent = (uint32_t)new_parent;

    /* Insert into new parent's hash table (using the new name) */
    if (new_parent >= 0 && new_parent < TMPFS_MAX_INODES &&
        inodes[new_parent].in_use &&
        inodes[new_parent].type == TMPFS_TYPE_DIR) {
        int iret = tmpfs_dir_insert(new_parent, old_idx);
        if (iret < 0) {
            /* Roll back parent; the old name is lost in the single flat
             * inode table but the inode is still reachable via old_path's
             * caller retry. */
            inodes[old_idx].parent = (uint32_t)old_parent;
            return iret;
        }
    }

    return 0;
}

/* ── O_TMPFILE: create an unnamed temporary file ────────────────────
 *
 * Creates an inode that is not linked into any directory.  The file
 * exists only as long as a file descriptor references it.  When the
 * last fd is closed, the inode and its data are freed.
 *
 * Item 455: TMPFS O_TMPFILE
 */
static int tmpfs_tmpfile(void *priv, uint32_t mode)
{
    (void)priv; (void)mode;

    int idx = alloc_inode();
    if (idx < 0)
        return -ENOSPC;

    inodes[idx].type = TMPFS_TYPE_FILE;
    inodes[idx].name[0] = '\0';       /* no name — unnamed */
    inodes[idx].parent = (uint32_t)-1; /* no parent — not in any directory */
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    inodes[idx].data_phys = 0;
    inodes[idx].numa_node = numa_home_node();
    inodes[idx].uid = 0;
    inodes[idx].gid = 0;
    inodes[idx].mode = (uint16_t)(mode & 0777);

    kprintf("[tmpfs] O_TMPFILE: created unnamed inode %d\n", idx);
    return idx; /* return inode index as file handle */
}

struct vfs_ops tmpfs_vfs_ops = {
    .read        = tmpfs_read,
    .write       = tmpfs_write,
    .create      = tmpfs_create,
    .unlink      = tmpfs_unlink,
    .stat        = tmpfs_stat,
    .readdir     = tmpfs_readdir,
    .readdir_names = tmpfs_readdir_names,
    .truncate    = tmpfs_truncate,
    .symlink     = tmpfs_symlink,
    .readlink    = tmpfs_readlink,
    .mknod       = tmpfs_mknod,
    .rename      = tmpfs_rename,
    .tmpfile     = tmpfs_tmpfile,
};

/* ══════════════════════════════════════════════════════════════════════
 * ── Quota / size-limit API implementation ───────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

int tmpfs_set_inode_limit(uint32_t max_inodes)
{
    /* Clamp to the hard table limit */
    if (max_inodes > TMPFS_MAX_INODES) {
        return -EINVAL;
    }
    /* Cannot set limit below current usage */
    if (max_inodes != TMPFS_INODE_UNLIMITED &&
        tmpfs_used_inodes > max_inodes) {
        return -EINVAL;
    }
    tmpfs_max_inodes = max_inodes;
    return 0;
}

uint32_t tmpfs_get_inode_limit(void)
{
    return tmpfs_max_inodes;
}

uint32_t tmpfs_get_used_inodes(void)
{
    return tmpfs_used_inodes;
}

int tmpfs_set_size_limit(uint64_t max_bytes)
{
    /* Cannot set limit below current usage */
    if (max_bytes != TMPFS_SIZE_UNLIMITED &&
        tmpfs_used_bytes > max_bytes) {
        return -ENOSPC;
    }
    tmpfs_size_limit = max_bytes;
    return 0;
}

uint64_t tmpfs_get_size_limit(void)
{
    return tmpfs_size_limit;
}

uint64_t tmpfs_get_used_bytes(void)
{
    return tmpfs_used_bytes;
}

int tmpfs_set_quota(uint32_t max_inodes, uint64_t max_bytes)
{
    int ret;

    /* Validate inode limit first */
    ret = tmpfs_set_inode_limit(max_inodes);
    if (ret < 0)
        return ret;

    /* Then validate size limit */
    ret = tmpfs_set_size_limit(max_bytes);
    if (ret < 0) {
        /* Roll back inode limit */
        tmpfs_max_inodes = TMPFS_INODE_UNLIMITED;
        return ret;
    }

    return 0;
}

int tmpfs_statfs(struct vfs_statfs *buf)
{
    if (!buf)
        return -EINVAL;

    memset(buf, 0, sizeof(*buf));
    buf->f_type    = 0x01021994;  /* tmpfs magic (same as generic ext2-ish) */
    buf->f_bsize   = TMPFS_BLOCK_SIZE;
    buf->f_blocks  = 0;  /* tmpfs has no backing store */
    buf->f_bfree   = 0;
    buf->f_bavail  = 0;
    buf->f_namelen = TMPFS_MAX_NAME - 1;

    /* Report inode quotas */
    buf->f_files   = (tmpfs_max_inodes != TMPFS_INODE_UNLIMITED)
                         ? (uint64_t)tmpfs_max_inodes
                         : TMPFS_MAX_INODES;
    buf->f_ffree   = (buf->f_files > (uint64_t)tmpfs_used_inodes)
                         ? (buf->f_files - (uint64_t)tmpfs_used_inodes)
                         : 0ULL;

    return 0;
}

int tmpfs_mount(void) {
    if (tmpfs_mounted) return -EINVAL;
    /* Clear all inodes */
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        inodes[i].in_use = 0;
        inodes[i].data = NULL;
        inodes[i].data_phys = 0;
        inodes[i].numa_node = 0;
        inodes[i].dir_htable = NULL;
    }
    /* Reset size accounting (unlimited) */
    tmpfs_size_limit = TMPFS_SIZE_UNLIMITED;
    tmpfs_used_bytes = 0;
    /* Reset inode quota (unlimited) */
    tmpfs_max_inodes = TMPFS_INODE_UNLIMITED;
    tmpfs_used_inodes = 0;
    /* Create root directory */
    inodes[0].in_use = 1;
    inodes[0].type = TMPFS_TYPE_DIR;
    inodes[0].name[0] = '\0';
    inodes[0].parent = 0;
    inodes[0].size = 0;
    inodes[0].data = NULL;
    inodes[0].data_phys = 0;
    inodes[0].numa_node = numa_home_node();
    inodes[0].uid = 0; inodes[0].gid = 0;
    inodes[0].mode = 0755;
    inodes[0].is_swapped = 0;
    inodes[0].swap_npages = 0;
    for (int j = 0; j < TMPFS_MAX_SWAP_PAGES; j++) {
        inodes[0].swap_map[j].swap_dev = -1;
        inodes[0].swap_map[j].swap_slot = 0;
    }
    /* Allocate per-directory hash table for root */
    tmpfs_dir_htable_alloc(0);
    tmpfs_used_inodes = 1;  /* root dir inode */
    tmpfs_mounted = 1;
    return 0;
}

int tmpfs_mount_with_limit(uint64_t max_bytes) {
    int ret = tmpfs_mount();
    if (ret == 0 && max_bytes > 0)
        tmpfs_size_limit = max_bytes;
    return ret;
}

int tmpfs_unmount(void) {
    if (!tmpfs_mounted) return -EINVAL;
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (inodes[i].in_use)
            free_inode(i);
    }
    tmpfs_mounted = 0;
    return 0;
}

void __init tmpfs_init(void) {
    tmpfs_mount();
    kprintf("[OK] tmpfs initialized\n");
}
#include "module.h"
fs_initcall(tmpfs_init);

/* ── tmpfs_umount ──────────────────────────────────────── */
static int tmpfs_umount(const char *target)
{
    (void)target;
    return tmpfs_unmount();
}
/* ── tmpfs_lookup ──────────────────────────────────────── */
static int tmpfs_lookup(const char *name, void *parent)
{
    int ino = (int)(uintptr_t)parent;

    if (ino < 0 || ino >= TMPFS_MAX_INODES)
        return -ENOENT;
    if (!inodes[ino].in_use || inodes[ino].type != TMPFS_TYPE_DIR)
        return -ENOENT;

    struct tmpfs_dir_htable *ht = inodes[ino].dir_htable;
    if (!ht)
        return -ENOENT;

    int len = (int)strlen(name);
    uint32_t hash = tmpfs_hash_name(name, len);
    uint32_t bucket = hash & (TMPFS_HASH_BUCKETS - 1);

    struct tmpfs_dirent *entry = ht->buckets[bucket];
    while (entry) {
        int idx = (int)entry->inode_idx;
        if (idx >= 0 && idx < TMPFS_MAX_INODES && inodes[idx].in_use &&
            inodes[idx].parent == (uint32_t)ino &&
            strncmp(inodes[idx].name, name, sizeof(inodes[idx].name)) == 0) {
            return idx;
        }
        entry = entry->next;
    }
    return -ENOENT;
}
/* ── tmpfs_sync ─────────────────────────────────────── */
static int tmpfs_sync(void *file)
{
    (void)file;
    return 0;
}

/* ── Public exports for swap-backing API ──────────────── */
EXPORT_SYMBOL(tmpfs_swap_out_inode);
EXPORT_SYMBOL(tmpfs_swap_in_inode);
EXPORT_SYMBOL(tmpfs_try_evict);

/* ── Public exports for quota / size-limit API ────────── */
EXPORT_SYMBOL(tmpfs_set_inode_limit);
EXPORT_SYMBOL(tmpfs_get_inode_limit);
EXPORT_SYMBOL(tmpfs_get_used_inodes);
EXPORT_SYMBOL(tmpfs_set_size_limit);
EXPORT_SYMBOL(tmpfs_get_size_limit);
EXPORT_SYMBOL(tmpfs_get_used_bytes);
EXPORT_SYMBOL(tmpfs_set_quota);
EXPORT_SYMBOL(tmpfs_statfs);
