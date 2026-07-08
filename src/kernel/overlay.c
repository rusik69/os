#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "kernel.h"
#include "overlay.h"
#include "vfs.h"
#include "errno.h"
#include "heap.h"
#include "export.h"
#ifdef MODULE
#include "module.h"
#endif

/* Static overlay mount table */
static struct overlay_mount overlay_table[OVERLAY_MAX_MOUNTS];
static int overlay_initialised = 0;

void __init overlay_init(void)
{
    if (overlay_initialised)
        return;

    memset(overlay_table, 0, sizeof(overlay_table));
    overlay_initialised = 1;
    kprintf("[OK] overlay: overlay filesystem initialised (%d max layers, %d max mounts)\n",
            OVERLAY_MAX_LAYERS, OVERLAY_MAX_MOUNTS);
}
EXPORT_SYMBOL(overlay_init);

/* Find a free overlay mount slot */
static int overlay_find_free(void)
{
    for (int i = 0; i < OVERLAY_MAX_MOUNTS; i++) {
        if (!overlay_table[i].in_use)
            return i;
    }
    return -ENOSPC;
}

/* Find an overlay mount by mountpoint */
static int overlay_find_by_mountpoint(const char *mntpt)
{
    if (!mntpt)
        return -EINVAL;
    for (int i = 0; i < OVERLAY_MAX_MOUNTS; i++) {
        if (overlay_table[i].in_use && strcmp(overlay_table[i].mountpoint, mntpt) == 0)
            return i;
    }
    return -ENOENT;
}

/* Check if a path falls under a given overlay mount */
static int overlay_contains(const struct overlay_mount *ovl, const char *path)
{
    size_t mlen = strlen(ovl->mountpoint);
    if (strncmp(path, ovl->mountpoint, mlen) == 0) {
        /* Exact match or subpath */
        if (path[mlen] == '\0' || path[mlen] == '/')
            return 1;
    }
    return 0;
}

/* Find which overlay mount a path belongs to */
static struct overlay_mount *overlay_for_path(const char *path)
{
    if (!path)
        return NULL;
    /* Scan in reverse to give priority to more recently mounted overlays */
    for (int i = OVERLAY_MAX_MOUNTS - 1; i >= 0; i--) {
        if (overlay_table[i].in_use && overlay_contains(&overlay_table[i], path))
            return &overlay_table[i];
    }
    return NULL;
}

/* Resolve path relative to an overlay: strip mountpoint prefix and prepend layer dir */
static int overlay_resolve(const struct overlay_mount *ovl, int layer_idx,
                           const char *path, char *out, size_t out_sz)
{
    const char *rel = path + strlen(ovl->mountpoint);
    if (*rel == '/')
        rel++;
    const char *layer =
        layer_idx == 0 ? ovl->upper_dir :
        (layer_idx <= ovl->num_lower ? ovl->lower_dirs[layer_idx - 1] : NULL);
    if (!layer)
        return -ENOENT;
    int ret = snprintf(out, out_sz, "%s/%s", layer, rel);
    if (ret < 0 || ret >= (int)out_sz)
        return -ENAMETOOLONG;
    return 0;
}

/* Do a copy-up of a file from a lower layer to the upper layer */
static int overlay_copy_up(struct overlay_mount *ovl, const char *path)
{
    char upper_path[128];
    char lower_path[128];
    struct vfs_stat st;
    int ret;

    /* Find the file in lower layers (from bottom up) */
    for (int i = ovl->num_lower; i >= 1; i--) {
        ret = overlay_resolve(ovl, i, path, lower_path, sizeof(lower_path));
        if (ret < 0)
            continue;
        ret = vfs_stat(lower_path, &st);
        if (ret == 0)
            goto found;
    }
    return -ENOENT;

found:
    /* Resolve the upper path */
    ret = overlay_resolve(ovl, 0, path, upper_path, sizeof(upper_path));
    if (ret < 0)
        return ret;

    /* Ensure parent directory exists in upper layer */
    char parent[128];
    strncpy(parent, upper_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        ret = vfs_stat(parent, &st);
        if (ret != 0) {
            ret = vfs_create(parent, 2); /* directory */
            if (ret != 0)
                kprintf("overlay: could not create parent dir %s (%d)\n", parent, ret);
        }
    }

    /* Create the upper file */
    ret = vfs_create(upper_path, 1);
    if (ret != 0 && ret != -EEXIST)
        return ret;

    /* Read from lower and write to upper */
    void *buf = kmalloc(st.size ? st.size : 4096);
    if (!buf)
        return -ENOMEM;

    uint32_t bytes_read = 0;
    ret = vfs_read(lower_path, buf, (uint32_t)st.size, &bytes_read);
    if (ret != 0) {
        kfree(buf);
        return ret;
    }

    ret = vfs_write(upper_path, buf, bytes_read);
    kfree(buf);

    if (ret != 0)
        return ret;

    kprintf("overlay: copy-up %s -> %s (%u bytes)\n", lower_path, upper_path, bytes_read);
    return 0;
}

int overlay_mount(const char *lower[], int num_lower,
                  const char *upper, const char *work, const char *mntpt)
{
    if (!lower || num_lower < 1 || num_lower > OVERLAY_MAX_LAYERS - 1 || !upper || !mntpt)
        return -EINVAL;
    if (!overlay_initialised)
        return 0;

    /* Check if already mounted */
    if (overlay_find_by_mountpoint(mntpt) >= 0)
        return -EBUSY;

    int idx = overlay_find_free();
    if (idx < 0)
        return -ENOSPC;

    struct overlay_mount *ovl = &overlay_table[idx];
    ovl->in_use = 1;
    strncpy(ovl->mountpoint, mntpt, sizeof(ovl->mountpoint) - 1);
    ovl->mountpoint[sizeof(ovl->mountpoint) - 1] = '\0';
    ovl->num_lower = num_lower;

    for (int i = 0; i < num_lower; i++) {
        strncpy(ovl->lower_dirs[i], lower[i], sizeof(ovl->lower_dirs[i]) - 1);
        ovl->lower_dirs[i][sizeof(ovl->lower_dirs[i]) - 1] = '\0';
    }
    strncpy(ovl->upper_dir, upper, sizeof(ovl->upper_dir) - 1);
    ovl->upper_dir[sizeof(ovl->upper_dir) - 1] = '\0';
    if (work) {
        strncpy(ovl->work_dir, work, sizeof(ovl->work_dir) - 1);
        ovl->work_dir[sizeof(ovl->work_dir) - 1] = '\0';
    }

    kprintf("[OK] overlay: mounted overlay on %s (%d lower layers, upper=%s)\n",
            mntpt, num_lower, upper);
    return 0;
}
EXPORT_SYMBOL(overlay_mount);

int overlay_read(const char *path, void *buf, uint32_t max_size, uint32_t *out_size)
{
    if (!path || !buf)
        return -EINVAL;
    if (!overlay_initialised)
        return 0;

    struct overlay_mount *ovl = overlay_for_path(path);
    if (!ovl)
        return -ENOENT;  /* not under any overlay mount, fall through to VFS */

    /* Search: upper layer first, then lower layers from top to bottom */
    char resolved[128];
    int ret;

    for (int i = 0; i <= ovl->num_lower; i++) {
        ret = overlay_resolve(ovl, i, path, resolved, sizeof(resolved));
        if (ret < 0)
            continue;
        struct vfs_stat st;
        ret = vfs_stat(resolved, &st);
        if (ret == 0) {
            return vfs_read(resolved, buf, max_size, out_size);
        }
    }

    return -ENOENT;
}
EXPORT_SYMBOL(overlay_read);

int overlay_write(const char *path, const void *data, uint32_t size)
{
    if (!path || !data)
        return -EINVAL;
    if (!overlay_initialised)
        return 0;

    struct overlay_mount *ovl = overlay_for_path(path);
    if (!ovl)
        return -ENOENT;

    /* Check if the file exists in the upper layer already */
    char upper_path[128];
    int ret = overlay_resolve(ovl, 0, path, upper_path, sizeof(upper_path));
    if (ret < 0)
        return ret;

    struct vfs_stat st;
    ret = vfs_stat(upper_path, &st);
    if (ret != 0) {
        /* File not in upper — check lower layers and copy-up */
        ret = overlay_copy_up(ovl, path);
        if (ret != 0 && ret != -EEXIST)
            return ret;
    }

    /* Write to upper layer */
    return vfs_write(upper_path, data, size);
}
EXPORT_SYMBOL(overlay_write);

/*
 * ovl_readdir — Read a directory from the merged overlay view.
 *
 * Iterates entries from all layers (upper first, then lower layers
 * top to bottom).  Upper entries shadow lower entries with the same
 * name.  Entries are written into the caller's buffer as a flat
 * array of vfs_dirent structs.
 *
 * For the simple implementation, we use vfs_readdir_names on each
 * layer and de-duplicate by name.
 */
int ovl_readdir(const char *dir_path, void *buf, uint32_t max_entries, uint32_t *out_count)
{
    if (!dir_path || !buf || !out_count)
        return -EINVAL;
    if (!overlay_initialised)
        return 0;

    struct overlay_mount *ovl = overlay_for_path(dir_path);
    if (!ovl)
        return -ENOENT;

    if (max_entries > 512)
        max_entries = 512;

    /* Temporary storage for de-duplication — heap-allocated to keep
     * stack usage sane (512*64 = 32 KB exceeds the 16 KB kernel stack). */
    char (*names)[64] = kmalloc(512 * sizeof(*names));
    if (!names)
        return -ENOMEM;

    uint32_t total = 0;
    char layer_path[128];
    int ret;

    /* Upper layer first */
    ret = overlay_resolve(ovl, 0, dir_path, layer_path, sizeof(layer_path));
    if (ret == 0) {
        struct vfs_stat st;
        if (vfs_stat(layer_path, &st) == 0 && st.type == VFS_TYPE_DIR) {
            char (*layer_names)[64] = kmalloc(256 * sizeof(*layer_names));
            if (!layer_names) {
                kfree(names);
                return -ENOMEM;
            }
            int count = vfs_readdir_names(layer_path, layer_names, 256);
            for (int i = 0; i < count && total < max_entries; i++) {
                /* Skip . and .. */
                if (strcmp(layer_names[i], ".") == 0 || strcmp(layer_names[i], "..") == 0)
                    continue;
                strncpy(names[total], layer_names[i], 63);
                names[total][63] = '\0';
                total++;
            }
            kfree(layer_names);
        }
    }

    /* Then lower layers (top to bottom) — skip names already seen */
    for (int li = 1; li <= ovl->num_lower && total < max_entries; li++) {
        ret = overlay_resolve(ovl, li, dir_path, layer_path, sizeof(layer_path));
        if (ret < 0) continue;

        struct vfs_stat st;
        if (vfs_stat(layer_path, &st) != 0 || st.type != VFS_TYPE_DIR)
            continue;

        char (*layer_names)[64] = kmalloc(256 * sizeof(*layer_names));
        if (!layer_names) {
            kfree(names);
            return -ENOMEM;
        }
        int count = vfs_readdir_names(layer_path, layer_names, 256);
        for (int i = 0; i < count && total < max_entries; i++) {
            if (strcmp(layer_names[i], ".") == 0 || strcmp(layer_names[i], "..") == 0)
                continue;
            /* Check if already seen from upper layer */
            int seen = 0;
            for (uint32_t j = 0; j < total; j++) {
                if (strcmp(names[j], layer_names[i]) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (!seen) {
                strncpy(names[total], layer_names[i], 63);
                names[total][63] = '\0';
                total++;
            }
        }
        kfree(layer_names);
    }

    /* Copy to output buffer */
    for (uint32_t i = 0; i < total; i++) {
        char *entry_buf = (char *)buf + i * 64;
        memcpy(entry_buf, names[i], 64);
    }

    *out_count = total;
    kfree(names);
    return 0;
}
EXPORT_SYMBOL(ovl_readdir);

/*
 * ovl_lookup — Look up a path in the merged overlay view.
 *
 * Searches upper layer first, then lower layers (top to bottom).
 * Returns the resolved (actual) path where the file exists in
 * 'resolved_path'.  The caller can then use that path with VFS.
 */
int ovl_lookup(const char *path, char *resolved_path, size_t resolved_sz)
{
    if (!path || !resolved_path || resolved_sz == 0)
        return -EINVAL;
    if (!overlay_initialised)
        return 0;

    struct overlay_mount *ovl = overlay_for_path(path);
    if (!ovl)
        return -ENOENT;

    char layer_path[128];
    struct vfs_stat st;

    /* Search upper first */
    int ret = overlay_resolve(ovl, 0, path, layer_path, sizeof(layer_path));
    if (ret == 0) {
        ret = vfs_stat(layer_path, &st);
        if (ret == 0) {
            strncpy(resolved_path, layer_path, resolved_sz - 1);
            resolved_path[resolved_sz - 1] = '\0';
            return 0;
        }
    }

    /* Then lower layers, top to bottom */
    for (int i = 1; i <= ovl->num_lower; i++) {
        ret = overlay_resolve(ovl, i, path, layer_path, sizeof(layer_path));
        if (ret < 0) continue;
        ret = vfs_stat(layer_path, &st);
        if (ret == 0) {
            strncpy(resolved_path, layer_path, resolved_sz - 1);
            resolved_path[resolved_sz - 1] = '\0';
            return 0;
        }
    }

    return -ENOENT;
}
EXPORT_SYMBOL(ovl_lookup);

/*
 * ovl_readlink — Read a symlink target from the merged overlay view.
 *
 * Searches upper layer first, then lower layers for a symlink at the
 * given overlay path.  Returns the symlink target contents.
 *
 * Returns the number of bytes written to 'buf' on success, or a
 * negative errno on failure.
 */
int ovl_readlink(const char *path, char *buf, size_t size)
{
    if (!path || !buf || size == 0)
        return -EINVAL;
    if (!overlay_initialised)
        return 0;

    struct overlay_mount *ovl = overlay_for_path(path);
    if (!ovl)
        return -ENOENT;

    char layer_path[128];
    struct vfs_stat st;
    int ret;

    /* Upper first */
    ret = overlay_resolve(ovl, 0, path, layer_path, sizeof(layer_path));
    if (ret == 0) {
        ret = vfs_stat(layer_path, &st);
        if (ret == 0 && st.type == VFS_TYPE_LINK) {
            return vfs_readlink(layer_path, buf, (int)size);
        }
    }

    /* Then lower layers, top to bottom */
    for (int i = 1; i <= ovl->num_lower; i++) {
        ret = overlay_resolve(ovl, i, path, layer_path, sizeof(layer_path));
        if (ret < 0) continue;
        ret = vfs_stat(layer_path, &st);
        if (ret == 0 && st.type == VFS_TYPE_LINK) {
            return vfs_readlink(layer_path, buf, (int)size);
        }
    }

    return -ENOENT;
}
EXPORT_SYMBOL(ovl_readlink);

#ifdef MODULE
/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    overlay_init();
    return 0;
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void) {
    overlay_initialised = 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Overlay/union mount filesystem — merges multiple lower (read-only) directories with an upper (writable) layer via copy-up-on-write");
MODULE_VERSION("1.0");
#endif /* MODULE */

/* ═══════════════════════════════════════════════════════════════════════
 *  Stub functions for incomplete overlay operations
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Stub: overlay_cleanup ─────────────────────────────────────────────── */
static void overlay_cleanup(void)
{
    kprintf("[overlay] overlay_cleanup not yet implemented\n");
}

/* ── Stub: overlay_add_lower_layer ─────────────────────────────────────── */
static int overlay_add_lower_layer(int mount_idx, const char *lower_dir)
{
    (void)mount_idx;
    (void)lower_dir;
    kprintf("[overlay] overlay_add_lower_layer not yet implemented\n");
    return 0;
}

/* ── Stub: overlay_remove_lower_layer ──────────────────────────────────── */
static int overlay_remove_lower_layer(int mount_idx, const char *lower_dir)
{
    (void)mount_idx;
    (void)lower_dir;
    kprintf("[overlay] overlay_remove_lower_layer not yet implemented\n");
    return 0;
}
