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

void overlay_init(void)
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
    ret = vfs_read(lower_path, buf, st.size, &bytes_read);
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
        return -ENOSYS;

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
        return -ENOSYS;

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
        return -ENOSYS;

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
