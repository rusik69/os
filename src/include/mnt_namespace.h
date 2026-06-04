#ifndef MNT_NAMESPACE_H
#define MNT_NAMESPACE_H

#include "types.h"
#include "vfs.h"

/*
 * mnt_namespace.h — Mount namespace (Item 112)
 *
 * Each mount namespace holds its own mount table.  Processes that share
 * a mount namespace see the same mounted filesystems.  CLONE_NEWNS
 * creates a new namespace with a deep copy of the parent's mount table;
 * subsequent mount/umount operations affect only that namespace.
 *
 * The root (initial) namespace wraps the global mount table.
 * Per-namespace tables are dynamically allocated and reference-counted.
 */

/* Reference-counted mount namespace descriptor */
struct mnt_namespace {
    int              refcount;      /* number of processes using this ns */
    struct vfs_mount mounts[VFS_MAX_MOUNTS]; /* per-ns mount table */
    int              num_mounts;    /* number of valid entries in mounts[] */
};

/* ── API ─────────────────────────────────────────────────────────── */

/* Create the root (initial) mount namespace by copying the global mounts.
 * Called once during VFS init.  Returns a namespace with refcount=1. */
struct mnt_namespace *mnt_ns_create_root(void);

/* Create a new mount namespace by deep-copying @src.
 * The new namespace has refcount=1.  Returns NULL on allocation failure. */
struct mnt_namespace *mnt_ns_copy(const struct mnt_namespace *src);

/* Drop a reference; frees the namespace when refcount reaches 0. */
void mnt_ns_put(struct mnt_namespace *ns);

/* Acquire a reference (call when a process adopts a namespace). */
struct mnt_namespace *mnt_ns_get(struct mnt_namespace *ns);

/* Return the current process's mount namespace, or the initial one. */
struct mnt_namespace *mnt_ns_current(void);

/* Mount a filesystem in the given namespace.  Returns 0 on success. */
int mnt_ns_mount(struct mnt_namespace *ns, const char *mountpoint,
                 struct vfs_ops *ops, void *priv, int flags);

/* Unmount a filesystem in the given namespace.  Returns 0 on success. */
int mnt_ns_umount(struct mnt_namespace *ns, const char *mountpoint);

/* Resolve the best-matching mount for a path within a namespace.
 * Returns a pointer into the namespace's mounts[] array, or NULL. */
struct vfs_mount *mnt_ns_resolve(struct mnt_namespace *ns, const char *path);

/* List mounted filesystem paths in a namespace (for /proc/mounts). */
int mnt_ns_list_mounts(struct mnt_namespace *ns, char mounts[][64], int max);

/* Sync all mounted filesystems in a namespace. */
void mnt_ns_sync(struct mnt_namespace *ns);

#endif /* MNT_NAMESPACE_H */
