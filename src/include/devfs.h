#ifndef DEVFS_H
#define DEVFS_H

#include "types.h"

/*
 * devfs.h — Dynamic device filesystem
 *
 * Drivers register character device nodes that appear under /dev/.
 * Built-in devices (/dev/null, /dev/zero, /dev/random, /dev/kmsg)
 * are always available.
 *
 * Usage in a driver init:
 *
 *   static int mydev_read(void *priv, void *buf, uint32_t max, uint32_t *out) {
 *       // fill buf with up to max bytes
 *       return 0;
 *   }
 *   devfs_register_device("mydev", my_priv, mydev_read, NULL);
 *
 * The device appears as /dev/mydev and is listed in /dev/ readdir.
 */

/* VFS operations exported for mounting */
extern struct vfs_ops devfs_ops;

/**
 * devfs_init - Initialise and mount the /dev device filesystem.
 * Called once during boot (or when the devfs module is loaded).
 */
void devfs_init(void);

/**
 * devfs_register_device - Register a dynamic device node in /dev/
 * @name:      Device node name (e.g. "ttyS0" creates "/dev/ttyS0").
 *             Must not contain '/', max 47 chars.
 * @priv:      Private data pointer passed to callbacks.
 * @read_fn:   Optional read callback.  Called when userspace reads the
 *             device.  Should write up to @max_size bytes into @buf and
 *             set *@out_size to the number of bytes written.  Return 0
 *             on success or negative on error.  May be NULL (reads
 *             return 0 bytes / EOF).
 * @write_fn:  Optional write callback.  Called when userspace writes to
 *             the device.  Should consume @size bytes from @data.
 *             Return the number of bytes consumed (typically @size) or
 *             negative on error.  May be NULL (writes are silently
 *             accepted and discarded).
 *
 * Returns: 0 on success, -1 on failure (table full, duplicate name,
 *          or invalid name).
 */
int devfs_register_device(const char *name, void *priv,
                          int (*read_fn)(void *priv, void *buf,
                                         uint32_t max_size, uint32_t *out_size),
                          int (*write_fn)(void *priv, const void *data,
                                          uint32_t size));

/**
 * devfs_unregister_device - Remove a dynamic device node from /dev/
 * @name:  Device node name (must match exactly).
 *
 * After this call the device node disappears from /dev/ listings and
 * operations on it will fail with -1.
 *
 * Returns: 0 on success, -1 if no device with that name is registered.
 */
int devfs_unregister_device(const char *name);

#endif /* DEVFS_H */
