#ifndef FS_MOUNT_PROP_H
#define FS_MOUNT_PROP_H

#include "types.h"

/* Propagation type flags (Linux-compatible) */
#define MS_SHARED     0x00000001  /* mount events propagate both ways */
#define MS_PRIVATE    0x00000002  /* mount events do not propagate */
#define MS_SLAVE      0x00000004  /* receives from master but does not send back */
#define MS_UNBINDABLE 0x00000008  /* cannot be bind-mounted */

/* Maximum mounts we track propagation for */
#define MOUNT_PROP_MAX 16

/* MOUNT_ATTR flags (Linux-compatible) */
#define MOUNT_ATTR_RDONLY        0x00000001
#define MOUNT_ATTR_NOSUID        0x00000002
#define MOUNT_ATTR_NODEV         0x00000004
#define MOUNT_ATTR_NOEXEC        0x00000008
#define MOUNT_ATTR_RELATIME      0x00000010

/* Propagation attribute structure (simplified version of mount_attr) */
struct mount_attr {
    uint64_t attr_set;     /* propagation flags to set */
    uint64_t attr_clr;     /* propagation flags to clear */
    uint64_t propagation;  /* MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE */
    uint64_t userns_fd;    /* user namespace fd (unused) */
};

/* Per-mount propagation state */
struct mount_prop_entry {
    int      in_use;
    char     mountpoint[64];
    uint32_t prop_flags;   /* MS_SHARED, MS_PRIVATE, MS_SLAVE, or MS_UNBINDABLE */
};

/* Set propagation attributes on a mount point.
 * dirfd: AT_FDCWD or a directory fd (unused in this simplified model).
 * path: path of the mount point.
 * flags: AT_EMPTY_PATH, AT_RECURSIVE, etc.
 * attr: pointer to a mount_attr structure.
 * size: sizeof(struct mount_attr).
 * Returns 0 on success, negative on error. */
int mount_setattr(int dirfd, const char *path, uint32_t flags,
                  const struct mount_attr *attr, size_t size);

/* Query the propagation type for a given path.
 * Returns the MS_* flags on success, or a negative error if not found. */
int mount_get_propagation(const char *path, uint32_t *prop_flags);

/* Propagate a mount event to shared/slave peers (stub for now).
 * Called when a new mount happens on a shared mount point. */
int mount_propagate_event(const char *source_mount, const char *target_path);

/* Initialise the mount propagation subsystem. */
void mount_prop_init(void);

#endif /* FS_MOUNT_PROP_H */
