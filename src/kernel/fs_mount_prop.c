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

/* ── Per-mount attribute tracking (MOUNT_ATTR_*) ────────────────────── */

struct mount_attr_state {
    uint32_t attr_set;    /* MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | ... */
    uint32_t prop_flags;  /* MS_PRIVATE | MS_SHARED | MS_SLAVE | MS_UNBINDABLE */
};

/* Per-mount attribute state table */
static struct {
    int      in_use;
    char     mountpoint[64];
    struct mount_attr_state attr;
} mount_attr_table[MOUNT_PROP_MAX];

static int mount_attr_initialised;

static void mount_attr_init(void)
{
    if (mount_attr_initialised)
        return;
    memset(mount_attr_table, 0, sizeof(mount_attr_table));
    mount_attr_initialised = 1;
    kprintf("[OK] mount_attr: mount attribute tracking initialised (%d entries)\n",
            MOUNT_PROP_MAX);
}

static int attr_entry_find_free(void)
{
    for (int i = 0; i < MOUNT_PROP_MAX; i++) {
        if (!mount_attr_table[i].in_use)
            return i;
    }
    return -ENOSPC;
}

static int attr_entry_find_by_path(const char *path)
{
    if (!path)
        return -EINVAL;
    for (int i = 0; i < MOUNT_PROP_MAX; i++) {
        if (mount_attr_table[i].in_use && strcmp(mount_attr_table[i].mountpoint, path) == 0)
            return i;
    }
    return -ENOENT;
}

static int attr_entry_set(const char *path, const struct mount_attr_state *state)
{
    int idx = attr_entry_find_by_path(path);
    if (idx >= 0) {
        mount_attr_table[idx].attr = *state;
        return 0;
    }
    idx = attr_entry_find_free();
    if (idx < 0)
        return -ENOSPC;
    mount_attr_table[idx].in_use = 1;
    strncpy(mount_attr_table[idx].mountpoint, path,
            sizeof(mount_attr_table[idx].mountpoint) - 1);
    mount_attr_table[idx].mountpoint[sizeof(mount_attr_table[idx].mountpoint) - 1] = '\0';
    mount_attr_table[idx].attr = *state;
    return 0;
}

/* ── Enhanced mount_setattr with MOUNT_ATTR_* support ───────────────── */

int mount_setattr(int dirfd, const char *path, uint32_t flags,
                  const struct mount_attr *attr, size_t size)
{
    (void)dirfd;
    (void)flags;

    if (!mount_prop_initialised)
        return 0;

    /* Lazily initialise the attribute table */
    if (!mount_attr_initialised)
        mount_attr_init();

    if (!path || !attr || size < sizeof(struct mount_attr))
        return -EINVAL;

    /* ── Determine the new propagation type ───────────────────────── */
    uint32_t new_prop = MS_DEFAULT_PROPAGATION;

    if (attr->propagation & MS_SHARED)
        new_prop = MS_SHARED;
    else if (attr->propagation & MS_SLAVE)
        new_prop = MS_SLAVE;
    else if (attr->propagation & MS_UNBINDABLE)
        new_prop = MS_UNBINDABLE;
    else if (attr->propagation & MS_PRIVATE)
        new_prop = MS_PRIVATE;

    /* attr_set overrides propagation */
    if (attr->attr_set & MS_SHARED)     new_prop = MS_SHARED;
    if (attr->attr_set & MS_SLAVE)      new_prop = MS_SLAVE;
    if (attr->attr_set & MS_UNBINDABLE) new_prop = MS_UNBINDABLE;
    if (attr->attr_set & MS_PRIVATE)    new_prop = MS_PRIVATE;

    /* ── Apply MOUNT_ATTR_* attribute modifications ───────────────── */

    struct mount_attr_state new_state;
    int existing_idx = attr_entry_find_by_path(path);
    if (existing_idx >= 0) {
        new_state = mount_attr_table[existing_idx].attr;
    } else {
        new_state.attr_set = 0;
        new_state.prop_flags = new_prop;
    }

    /* Apply attr_set: add these attributes */
    new_state.attr_set |= (uint32_t)(attr->attr_set & 0xFFFFFFFFULL);

    /* Apply attr_clr: remove these attributes */
    new_state.attr_set &= ~(uint32_t)(attr->attr_clr & 0xFFFFFFFFULL);

    /* Clamp to known flags */
    uint32_t known = MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID |
                     MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC |
                     MOUNT_ATTR_RELATIME;
    new_state.attr_set &= known;
    new_state.prop_flags = new_prop;

    kprintf("[mount_setattr] %s: attr_set=0x%x, attr_clr=0x%x, prop=0x%x\n",
            path,
            (unsigned)(attr->attr_set & 0xFFFFFFFFULL),
            (unsigned)(attr->attr_clr & 0xFFFFFFFFULL),
            (unsigned)new_prop);

    /* Store propagation in existing table */
    int ret = entry_set(path, new_prop);
    if (ret < 0)
        return ret;

    /* Store full attributes in new table */
    return attr_entry_set(path, &new_state);
}

/* ── Legacy mount_get_propagation ───────────────────────────────────── */

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
    if (!mount_prop_initialised)
        return 0;

    if (!source_mount || !target_path)
        return -EINVAL;

    /* Find the propagation type of the source mount */
    int src_idx = entry_find_by_path(source_mount);
    if (src_idx < 0)
        return src_idx;  /* -ENOENT if not tracked */

    uint32_t src_prop = mount_prop_table[src_idx].prop_flags;

    kprintf("[mount_prop] propagate: event on %s (prop=0x%x) -> %s\n",
            source_mount, src_prop, target_path);

    /* Only SHARED mounts propagate events to their peers */
    if (src_prop != MS_SHARED)
        return 0;

    /* If the source is SHARED, propagate to all SLAVE mounts that
     * have the source as their master.  For simplicity, we check
     * all tracked mounts and propagate to those with MS_SLAVE.
     *
     * In a full implementation we would also walk the mount tree
     * to find shared peer groups and slave lists. */
    for (int i = 0; i < MOUNT_PROP_MAX; i++) {
        if (!mount_prop_table[i].in_use)
            continue;
        if (mount_prop_table[i].prop_flags == MS_SLAVE) {
            /* This slave mount receives the propagation.
             * In a real kernel we would recursively apply the
             * mount/unmount event to this mount's subtree. */
            kprintf("[mount_prop]   -> propagating to slave: %s\n",
                    mount_prop_table[i].mountpoint);
        }
    }

    return 0;
}

/* ── Stub: fs_mount_propagate ─────────────────────────────── */
int fs_mount_propagate(void *mnt)
{
    (void)mnt;
    kprintf("[fs_mount] fs_mount_propagate: not yet implemented\n");
    return 0;
}
/* ── Stub: fs_mount_umount_propagate ─────────────────────────────── */
int fs_mount_umount_propagate(void *mnt)
{
    (void)mnt;
    kprintf("[fs_mount] fs_mount_umount_propagate: not yet implemented\n");
    return 0;
}
/* ── Stub: fs_mount_set_group ─────────────────────────────── */
int fs_mount_set_group(void *mnt, int group_id)
{
    (void)mnt;
    (void)group_id;
    kprintf("[fs_mount] fs_mount_set_group: not yet implemented\n");
    return 0;
}
