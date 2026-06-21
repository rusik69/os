#ifndef OVERLAY_H
#define OVERLAY_H

#include "types.h"

/* Maximum number of lower layers plus one upper layer */
#define OVERLAY_MAX_LAYERS 4

/* Maximum concurrent overlay mounts */
#define OVERLAY_MAX_MOUNTS 8

/* Overlay mount descriptor */
struct overlay_mount {
    int      in_use;
    char     mountpoint[64];
    char     lower_dirs[OVERLAY_MAX_LAYERS][64];  /* lower layer paths */
    int      num_lower;                           /* how many lower layers */
    char     upper_dir[64];                       /* upper (writable) layer */
    char     work_dir[64];                        /* work directory for copy-up */
};

/* Mount an overlay filesystem.
 * lower: array of paths for lower (read-only) layers (must include at least one).
 * upper: path to the upper (writable) layer.
 * mntpt: where to mount the merged view.
 * Returns 0 on success, negative on error. */
int overlay_mount(const char *lower[], int num_lower,
                  const char *upper, const char *work, const char *mntpt);

/* Read from the merged overlay view.
 * The path is resolved across layers (upper wins over lower).
 * Returns bytes read on success, negative on error. */
int overlay_read(const char *path, void *buf, uint32_t max_size, uint32_t *out_size);

/* Write to the overlay: if the file exists only in a lower layer,
 * a copy-up is performed first.  Returns 0 on success, negative on error. */
int overlay_write(const char *path, const void *data, uint32_t size);

/* Initialise the overlay subsystem. */
void overlay_init(void);

/*
 * Read a directory from the merged overlay view.
 * Iterates entries from both upper and lower layers, returning
 * unique entries (upper entries shadow lower ones with the same name).
 * dir_path: path to the directory (under the overlay mountpoint).
 * buf: output buffer for directory entries (struct vfs_dirent array).
 * max_entries: maximum number of entries to return.
 * out_count: actual number of entries written.
 * Returns 0 on success, negative on error.
 */
int ovl_readdir(const char *dir_path, void *buf, uint32_t max_entries, uint32_t *out_count);

/*
 * Look up a path in the merged overlay view.
 * Searches upper layer first, then lower layers (top to bottom).
 * resolved_path: output buffer for the actual path where the file exists.
 * Returns 0 on success (found), negative on error (ENOENT if not found).
 */
int ovl_lookup(const char *path, char *resolved_path, size_t resolved_sz);

/*
 * Read a symlink target from the merged overlay view.
 * Searches upper then lower layers for the symlink, reads its target.
 * buf: output buffer for the link target.
 * size: size of output buffer.
 * Returns number of bytes written on success, negative on error.
 */
int ovl_readlink(const char *path, char *buf, size_t size);

#endif /* OVERLAY_H */
