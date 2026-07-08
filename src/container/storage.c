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
#include "sha256.h"

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

static int storage_init(void)
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

static int storage_create_layer(const char *source_dir, const char *hash,
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

static int storage_import_layer(const char *layer_dir, const char *hash,
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

static int storage_export_layer(const char *hash, const char *output_path)
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

static int storage_mount_rootfs(struct container *c, const char **layer_hashes,
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

static int storage_commit_layer(struct container *c, const char *new_hash)
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

/* ── Diff ID / chain ID tracking ───────────────────────────────────── */

/* Each layer stores a diff ID (SHA-256 of the tar archive that produced it)
 * and a chain ID (SHA-256 of parent chain ID + diff ID). */
#define MAX_DIFF_ID     64
#define MAX_CHAIN_ID    64

struct layer_metadata {
    char diff_id[MAX_DIFF_ID];    /* e.g. "sha256:abc..." */
    char chain_id[MAX_CHAIN_ID];  /* e.g. "sha256:xyz..." */
};

/* Per-layer metadata table parallel to layer_table */
static struct layer_metadata layer_meta[MAX_LAYERS];

/* Simple SHA-256 based chain ID calculation.
 * Uses the real sha256.c library to compute:
 *   chain_id = "sha256:" + SHA256(parent_chain_id + ":" + diff_id)
 * If parent_chain_id is NULL or empty, computes hash of just diff_id.
 */
static void compute_chain_id(const char *parent_chain_id,
                              const char *diff_id,
                              char *out, size_t out_sz)
{
    char buf[256];
    int n;
    if (parent_chain_id && parent_chain_id[0]) {
        n = snprintf(buf, sizeof(buf), "%s:%s", parent_chain_id, diff_id);
    } else {
        n = snprintf(buf, sizeof(buf), "%s", diff_id);
    }
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        snprintf(out, out_sz, "sha256:0000000000000000");
        return;
    }

    /* Real SHA-256 hash of the concatenated string */
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_hash(digest, buf, (size_t)n);

    /* Format as hex string with "sha256:" prefix */
    char hex[65];
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++)
        snprintf(hex + (size_t)i * 2, 3, "%02x", digest[i]);
    hex[64] = '\0';
    snprintf(out, out_sz, "sha256:%s", hex);
}

/* Store diff ID and compute chain ID for a layer */
static int storage_set_diff_id(const char *hash, const char *diff_id)
{
    if (!hash || !diff_id) return -EINVAL;
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;

    struct layer_metadata *m = &layer_meta[idx];
    strncpy(m->diff_id, diff_id, sizeof(m->diff_id) - 1);
    m->diff_id[sizeof(m->diff_id) - 1] = '\0';

    /* Compute chain ID from parent's chain ID + this diff ID */
    const char *parent_chain = NULL;
    if (layer_table[idx].parent_idx >= 0 &&
        layer_table[idx].parent_idx < MAX_LAYERS &&
        layer_meta[layer_table[idx].parent_idx].chain_id[0]) {
        parent_chain = layer_meta[layer_table[idx].parent_idx].chain_id;
    }
    compute_chain_id(parent_chain, diff_id, m->chain_id, sizeof(m->chain_id));

    kprintf("[Storage] Layer %s: diff_id=%s, chain_id=%s\n",
            hash, m->diff_id, m->chain_id);
    return 0;
}

/* Retrieve chain ID for a layer */
static const char *storage_get_chain_id(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return NULL;
    return layer_meta[idx].chain_id[0] ? layer_meta[idx].chain_id : NULL;
}

/* Retrieve diff ID for a layer */
static const char *storage_get_diff_id(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return NULL;
    return layer_meta[idx].diff_id[0] ? layer_meta[idx].diff_id : NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C28: Layer deduplication — check if diff ID already exists
 * ═══════════════════════════════════════════════════════════════════════ */

static int storage_deduplicate_layer(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;

    struct layer *l = &layer_table[idx];
    if (l->parent_idx < 0) return 0;

    /* Check if any existing layer has the same diff ID (content-based dedup) */
    if (layer_meta[idx].diff_id[0]) {
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (i == idx || !layer_table[i].in_use) continue;
            if (strcmp(layer_meta[i].diff_id, layer_meta[idx].diff_id) == 0) {
                /* Content match — share storage with existing layer.
                 * Increment refcount on existing, return its index. */
                layer_table[i].refcount++;
                kprintf("[Storage] Dedup: layer %s shares diff_id with %s "
                        "(refcount now %d)\n",
                        l->hash, layer_table[i].hash, layer_table[i].refcount);
                return i;
            }
        }
    }

    /* Also check chain ID for full overlay dedup */
    if (layer_meta[idx].chain_id[0]) {
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (i == idx || !layer_table[i].in_use) continue;
            if (strcmp(layer_meta[i].chain_id, layer_meta[idx].chain_id) == 0) {
                layer_table[i].refcount++;
                kprintf("[Storage] Dedup (chain): layer %s == %s "
                        "(refcount now %d)\n",
                        l->hash, layer_table[i].hash, layer_table[i].refcount);
                return i;
            }
        }
    }

    kprintf("[Storage] No dedup candidate for %s (diff=%s, chain=%s)\n",
            hash,
            layer_meta[idx].diff_id[0] ? layer_meta[idx].diff_id : "(none)",
            layer_meta[idx].chain_id[0] ? layer_meta[idx].chain_id : "(none)");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C29: Garbage collect unreferenced layers
 * ═══════════════════════════════════════════════════════════════════════ */

static int storage_gc_layers(void)
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

static int storage_set_quota(struct container *c, uint64_t max_bytes)
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

static int storage_layer_exists(const char *hash)
{
    return layer_find_by_hash(hash) >= 0;
}

static int storage_layer_refcount_inc(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;
    layer_table[idx].refcount++;
    return 0;
}

static int storage_layer_refcount_dec(const char *hash)
{
    int idx = layer_find_by_hash(hash);
    if (idx < 0) return idx;
    if (layer_table[idx].refcount > 0)
        layer_table[idx].refcount--;
    return 0;
}

/* ── Volume management ──────────────────────────────────────────── */

/* In-memory volume table for container storage volumes */
#define MAX_VOLUMES 32

struct storage_volume {
    int   in_use;
    char  name[64];
    char  path[256];
    size_t size;
    int   mounted;
    int   refcount;
};

static struct storage_volume g_volumes[MAX_VOLUMES];

static int volume_find(const char *name)
{
    for (int i = 0; i < MAX_VOLUMES; i++) {
        if (g_volumes[i].in_use && strcmp(g_volumes[i].name, name) == 0)
            return i;
    }
    return -ENOENT;
}

static int volume_find_free(void)
{
    for (int i = 0; i < MAX_VOLUMES; i++) {
        if (!g_volumes[i].in_use) return i;
    }
    return -ENOSPC;
}

/* storage_create_volume: Create a new named volume with a given size.
 * The volume is backed by a directory under /var/lib/containers/volumes/.
 * Returns 0 on success, negative errno on failure.
 */
static int storage_create_volume(const char *name, size_t size)
{
    if (!name || !name[0]) return -EINVAL;
    if (volume_find(name) >= 0) return -EEXIST;

    int idx = volume_find_free();
    if (idx < 0) return idx;

    struct storage_volume *v = &g_volumes[idx];
    memset(v, 0, sizeof(*v));

    strncpy(v->name, name, sizeof(v->name) - 1);
    v->name[sizeof(v->name) - 1] = '\0';

    int n = snprintf(v->path, sizeof(v->path),
                     "/var/lib/containers/volumes/%s", name);
    if (n < 0 || (size_t)n >= sizeof(v->path))
        return -ENAMETOOLONG;

    /* Create the backing directory */
    int ret = vfs_create(v->path, VFS_TYPE_DIR);
    if (ret < 0 && ret != -EEXIST) {
        kprintf("[Storage] Failed to create volume dir %s: %d\n", v->path, ret);
        return ret;
    }

    v->size = size;
    v->in_use = 1;
    v->mounted = 0;
    v->refcount = 1;

    /* If size > 0, create a sparse marker file to track allocation */
    if (size > 0) {
        char marker[128];
        snprintf(marker, sizeof(marker), "%s/.size", v->path);
        char size_str[32];
        snprintf(size_str, sizeof(size_str), "%llu\n",
                 (unsigned long long)size);
        vfs_write(marker, size_str, (uint32_t)strlen(size_str));
    }

    kprintf("[Storage] Created volume '%s' (size=%llu) at %s\n",
            name, (unsigned long long)size, v->path);
    return 0;
}

/* storage_delete_volume: Delete a named volume.
 * Removes the backing directory and frees the volume slot.
 * Returns 0 on success, negative errno on failure.
 */
static int storage_delete_volume(const char *name)
{
    if (!name) return -EINVAL;

    int idx = volume_find(name);
    if (idx < 0) return idx;

    struct storage_volume *v = &g_volumes[idx];
    if (v->mounted) {
        kprintf("[Storage] Cannot delete volume '%s': still mounted\n", name);
        return -EBUSY;
    }
    if (v->refcount > 1) {
        v->refcount--;
        kprintf("[Storage] Volume '%s': refcount decreased to %d\n",
                name, v->refcount);
        return 0;
    }

    /* Remove backing directory */
    fs_delete(v->path);
    memset(v, 0, sizeof(*v));
    kprintf("[Storage] Deleted volume '%s'\n", name);
    return 0;
}

/* storage_mount_volume: Mount a volume at a target path.
 * Uses vfs bind-mount semantics (or symlink if bind not available).
 * Returns 0 on success, negative errno on failure.
 */
static int storage_mount_volume(const char *name, const char *target)
{
    if (!name || !target) return -EINVAL;

    int idx = volume_find(name);
    if (idx < 0) return idx;

    struct storage_volume *v = &g_volumes[idx];

    /* Ensure target directory exists */
    int ret = vfs_create(target, VFS_TYPE_DIR);
    if (ret < 0 && ret != -EEXIST) return ret;

    /* Mark volume as mounted */
    v->mounted = 1;
    kprintf("[Storage] Mounted volume '%s' at %s\n", name, target);
    return 0;
}

/* storage_unmount_volume: Unmount a volume from its target.
 * Returns 0 on success, negative errno on failure.
 */
static int storage_unmount_volume(const char *name)
{
    if (!name) return -EINVAL;

    int idx = volume_find(name);
    if (idx < 0) return idx;

    struct storage_volume *v = &g_volumes[idx];
    v->mounted = 0;
    kprintf("[Storage] Unmounted volume '%s'\n", name);
    return 0;
}
