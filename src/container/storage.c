/*
 * storage.c — Container storage driver abstraction (Items C16–C30)
 *
 * Provides layer management, overlay mounts, and storage operations.
 * All non-existent VFS/FS function calls are implemented as stubs
 * or replaced with practical alternatives.
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "overlay.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "fs.h"
#include "vfs.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define MAX_LAYER_HASH    64
#define MAX_LAYER_PATH    256
#define MAX_LAYERS        128
#define LAYER_DATA_DIR    "/var/lib/containers/layers"

/* ── Layer descriptor ───────────────────────────────────────────────── */

struct layer {
    char   in_use;
    char   hash[MAX_LAYER_HASH];
    char   path[MAX_LAYER_PATH];
    int    refcount;
    int    parent_idx;
};

static struct layer layer_table[MAX_LAYERS];
static int storage_initialised = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_init(void)
{
    if (storage_initialised) return 0;

    memset(layer_table, 0, sizeof(layer_table));

    int ret = vfs_create(LAYER_DATA_DIR, VFS_TYPE_DIR);
    if (ret < 0 && ret != -EEXIST) {
        kprintf("[Storage] Failed to create %s: err=%d\n", LAYER_DATA_DIR, ret);
        return ret;
    }

    storage_initialised = 1;
    kprintf("[Storage] Storage subsystem initialised (%d max layers)\n", MAX_LAYERS);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Layer management helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static int layer_find_free(void)
{
    for (int i = 0; i < MAX_LAYERS; i++)
        if (!layer_table[i].in_use) return i;
    return -ENOSPC;
}

static int layer_find_by_hash(const char *hash)
{
    for (int i = 0; i < MAX_LAYERS; i++)
        if (layer_table[i].in_use && strcmp(layer_table[i].hash, hash) == 0)
            return i;
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C19: Create a layer from a source directory
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_create_layer(const char *source_dir, const char *hash,
                         const char *parent_hash)
{
    if (!source_dir || !hash) return -EINVAL;

    int idx = layer_find_free();
    if (idx < 0) return idx;

    struct layer *l = &layer_table[idx];
    char layer_path[MAX_LAYER_PATH];

    int n = snprintf(layer_path, sizeof(layer_path), "%s/%s",
                     LAYER_DATA_DIR, hash);
    if (n < 0 || (size_t)n >= sizeof(layer_path)) return -ENAMETOOLONG;

    /* Create layer directory */
    n = vfs_create(layer_path, VFS_TYPE_DIR);
    if (n < 0 && n != -EEXIST) return n;

    /* Copy via read/write loop using VFS */
    /*
     * In production: iterate source_dir entries via vfs_readdir_names() and
     * copy each file/dir. Simplified: just mark the layer as created.
     */
    kprintf("[Storage] Created layer %s at %s\n", hash, layer_path);

    l->in_use = 1;
    strncpy(l->hash, hash, sizeof(l->hash) - 1);
    l->hash[sizeof(l->hash) - 1] = '\0';
    strncpy(l->path, layer_path, sizeof(l->path) - 1);
    l->path[sizeof(l->path) - 1] = '\0';
    l->refcount = 1;
    l->parent_idx = (parent_hash && parent_hash[0])
                    ? layer_find_by_hash(parent_hash) : -1;

    kprintf("[Storage] Created layer %s (parent=%s)\n", hash,
            parent_hash ? parent_hash : "(none)");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C22: Import a layer (register existing directory)
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_import_layer(const char *layer_dir, const char *hash,
                         const char *parent_hash)
{
    if (!layer_dir || !hash) return -EINVAL;

    /* Verify the layer directory exists via stat */
    struct vfs_stat st;
    int ret = vfs_stat(layer_dir, &st);
    if (ret < 0) return -ENOENT;

    return storage_create_layer(layer_dir, hash, parent_hash);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C23: Export a layer (indicate it's registered)
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_export_layer(const char *hash, const char *output_path)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;

    int ret = vfs_create(output_path, VFS_TYPE_DIR);
    if (ret < 0 && ret != -EEXIST) return ret;

    kprintf("[Storage] Export layer %s → %s\n", hash, output_path);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C24: Mount container rootfs from image layers via overlay
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_mount_rootfs(struct container *c, const char **layer_hashes,
                          int num_layers)
{
    if (!c || !c->in_use || !layer_hashes || num_layers <= 0)
        return -EINVAL;

    /* Resolve layer paths into lowerdir array */
    const char *lower[MAX_LAYERS];
    int num_lower = 0;

    for (int i = num_layers - 1; i >= 0 && num_lower < MAX_LAYERS; i--) {
        int idx = layer_find_by_hash(layer_hashes[i]);
        if (idx < 0) {
            kprintf("[Storage] Layer %s not found\n", layer_hashes[i]);
            return -ENOENT;
        }
        lower[num_lower++] = layer_table[idx].path;
    }

    /* Upper dir is a new directory in the container's data dir */
    char upper[CONTAINER_STATE_PATH];
    int n = snprintf(upper, sizeof(upper), "%s/upper", c->data_dir);
    if (n < 0 || (size_t)n >= sizeof(upper)) return -ENAMETOOLONG;

    n = vfs_create(upper, VFS_TYPE_DIR);
    if (n < 0 && n != -EEXIST) return n;

    /* Work dir for overlay */
    char work[CONTAINER_STATE_PATH];
    n = snprintf(work, sizeof(work), "%s/work", c->data_dir);
    if (n < 0 || (size_t)n >= sizeof(work)) return -ENAMETOOLONG;

    n = vfs_create(work, VFS_TYPE_DIR);
    if (n < 0 && n != -EEXIST) return n;

    /* Mount the overlay */
    int ret = overlay_mount(lower, num_lower, upper, work, c->rootfs_path);
    if (ret < 0) {
        kprintf("[Storage] overlay_mount failed for %s: err=%d\n", c->id, ret);
        return ret;
    }

    kprintf("[Storage] Mounted rootfs for %s (%d layers)\n", c->id, num_layers);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C25: Commit container changes as new layer
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_commit_layer(struct container *c, const char *new_hash)
{
    if (!c || !c->in_use || !new_hash) return -EINVAL;

    char upperdir[CONTAINER_STATE_PATH];
    int n = snprintf(upperdir, sizeof(upperdir), "%s/upper", c->data_dir);
    if (n < 0 || (size_t)n >= sizeof(upperdir)) return -ENAMETOOLONG;

    /* Verify upperdir exists */
    struct vfs_stat st;
    int ret_chk = vfs_stat(upperdir, &st);
    if (ret_chk < 0) return -ENOENT;

    return storage_create_layer(upperdir, new_hash, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C28: Layer deduplication (stub — basic tracking)
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_deduplicate_layer(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;

    struct layer *l = &layer_table[idx];
    if (l->parent_idx < 0) return 0;

    kprintf("[Storage] Dedup requested for %s (parent=%d)\n",
            hash, l->parent_idx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C29: Garbage collect unreferenced layers
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_gc_layers(void)
{
    int removed = 0;
    for (int i = 0; i < MAX_LAYERS; i++) {
        struct layer *l = &layer_table[i];
        if (l->in_use && l->refcount <= 0) {
            fs_delete(l->path);
            memset(l, 0, sizeof(*l));
            removed++;
        }
    }
    if (removed > 0)
        kprintf("[Storage] GC removed %d unreferenced layers\n", removed);
    return removed;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C30: Set storage quota (track in descriptor)
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_set_quota(struct container *c, uint64_t max_bytes)
{
    if (!c || !c->in_use) return -EINVAL;
    c->memory_limit = max_bytes;
    kprintf("[Storage] Quota set for %s: %llu bytes\n",
            c->id, (unsigned long long)max_bytes);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Lookup
 * ═══════════════════════════════════════════════════════════════════════ */

int storage_layer_exists(const char *hash)
{
    return layer_find_by_hash(hash) >= 0;
}

int storage_layer_refcount_inc(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;
    layer_table[idx].refcount++;
    return 0;
}

int storage_layer_refcount_dec(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;
    if (layer_table[idx].refcount > 0)
        layer_table[idx].refcount--;
    return 0;
}
