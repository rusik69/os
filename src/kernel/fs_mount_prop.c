#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "kernel.h"
#include "fs_mount_prop.h"
#include "vfs.h"
#include "errno.h"

/* Propagation type for newly mounted filesystems (default: PRIVATE) */
#define MS_DEFAULT_PROPAGATION MS_PRIVATE

/* Per-mount propagation table */
static struct mount_prop_entry mount_prop_table[MOUNT_PROP_MAX];
static int mount_prop_initialised = 0;

void mount_prop_init(void)
{
    if (mount_prop_initialised)
        return;

    memset(mount_prop_table, 0, sizeof(mount_prop_table));
    mount_prop_initialised = 1;
    kprintf("[OK] mount_prop: mount propagation tracking initialised (%d entries)\n",
            MOUNT_PROP_MAX);
}

/* Find a free slot */
static int entry_find_free(void)
{
    for (int i = 0; i < MOUNT_PROP_MAX; i++) {
        if (!mount_prop_table[i].in_use)
            return i;
    }
    return -ENOSPC;
}

/* Find an entry by mountpoint */
static int entry_find_by_path(const char *path)
{
    if (!path)
        return -EINVAL;
    for (int i = 0; i < MOUNT_PROP_MAX; i++) {
        if (mount_prop_table[i].in_use && strcmp(mount_prop_table[i].mountpoint, path) == 0)
            return i;
    }
    return -ENOENT;
}

/* Add or update propagation tracking for a mount point */
static int entry_set(const char *path, uint32_t prop_flags)
{
    int idx = entry_find_by_path(path);
    if (idx >= 0) {
        mount_prop_table[idx].prop_flags = prop_flags;
        return 0;
    }

    idx = entry_find_free();
    if (idx < 0)
        return -ENOSPC;

    struct mount_prop_entry *e = &mount_prop_table[idx];
    e->in_use = 1;
    strncpy(e->mountpoint, path, sizeof(e->mountpoint) - 1);
    e->mountpoint[sizeof(e->mountpoint) - 1] = '\0';
    e->prop_flags = prop_flags;
    return 0;
}

int mount_setattr(int dirfd, const char *path, uint32_t flags,
                  const struct mount_attr *attr, size_t size)
{
    (void)dirfd;
    (void)flags;

    if (!mount_prop_initialised)
        return -ENOSYS;
    if (!path || !attr || size < sizeof(struct mount_attr))
        return -EINVAL;

    /* Determine the new propagation flags */
    uint32_t new_prop = MS_DEFAULT_PROPAGATION;

    if (attr->propagation & MS_SHARED)
        new_prop = MS_SHARED;
    else if (attr->propagation & MS_SLAVE)
        new_prop = MS_SLAVE;
    else if (attr->propagation & MS_UNBINDABLE)
        new_prop = MS_UNBINDABLE;
    else if (attr->propagation & MS_PRIVATE)
        new_prop = MS_PRIVATE;

    /* Apply attr_clr: mask out cleared flags */
    /* (In a full implementation we'd do a proper merge) */
    if (attr->attr_set & MS_SHARED)     new_prop = MS_SHARED;
    if (attr->attr_set & MS_SLAVE)      new_prop = MS_SLAVE;
    if (attr->attr_set & MS_UNBINDABLE) new_prop = MS_UNBINDABLE;
    if (attr->attr_set & MS_PRIVATE)    new_prop = MS_PRIVATE;

    return entry_set(path, new_prop);
}

int mount_get_propagation(const char *path, uint32_t *prop_flags)
{
    if (!mount_prop_initialised || !path || !prop_flags)
        return -EINVAL;

    int idx = entry_find_by_path(path);
    if (idx < 0)
        return idx;

    *prop_flags = mount_prop_table[idx].prop_flags;
    return 0;
}

int mount_propagate_event(const char *source_mount, const char *target_path)
{
    (void)source_mount;
    (void)target_path;

    if (!mount_prop_initialised)
        return -ENOSYS;

    /* Stub: In a full implementation this would walk the mount tree
     * and propagate mount events according to propagation types.
     * Shared mounts propagate both ways, slave mounts receive from master,
     * private and unbindable are isolated. */
    kprintf("mount_prop: propagate event from %s to %s (stub)\n",
            source_mount, target_path);
    return 0;
}
