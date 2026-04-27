#include "vfs.h"
#include "fs.h"
#include "string.h"
#include "printf.h"

/* ------------------------------------------------------------------
 * SMFS backend — adapts the existing fs.c API to vfs_ops
 * ------------------------------------------------------------------ */

static int smfs_read(void *priv, const char *path, void *buf,
                     uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    return fs_read_file(path, buf, max_size, out_size);
}

static int smfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;
    return fs_write_file(path, data, size);
}

static int smfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    uint32_t size; uint8_t type;
    int r = fs_stat(path, &size, &type);
    if (r < 0) return r;
    st->size = size;
    st->type = type;
    return 0;
}

static int smfs_create(void *priv, const char *path, uint8_t type) {
    (void)priv;
    return fs_create(path, type);
}

static int smfs_unlink(void *priv, const char *path) {
    (void)priv;
    return fs_delete(path);
}

static int smfs_readdir(void *priv, const char *path) {
    (void)priv;
    return fs_list(path);
}

static struct vfs_ops smfs_ops = {
    .read    = smfs_read,
    .write   = smfs_write,
    .stat    = smfs_stat,
    .create  = smfs_create,
    .unlink  = smfs_unlink,
    .readdir = smfs_readdir,
};

/* ------------------------------------------------------------------
 * Mount table
 * ------------------------------------------------------------------ */

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static int num_mounts = 0;

int vfs_mount(const char *mountpoint, struct vfs_ops *ops, void *priv) {
    if (num_mounts >= VFS_MAX_MOUNTS) return -1;
    size_t mlen = strlen(mountpoint);
    if (mlen >= 64) mlen = 63;
    memcpy(mounts[num_mounts].mountpoint, mountpoint, mlen);
    mounts[num_mounts].mountpoint[mlen] = '\0';
    mounts[num_mounts].ops  = ops;
    mounts[num_mounts].priv = priv;
    num_mounts++;
    return 0;
}

/* Find the best-matching mount for a path */
static struct vfs_mount *resolve(const char *path) {
    struct vfs_mount *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < num_mounts; i++) {
        size_t mlen = strlen(mounts[i].mountpoint);
        if (strncmp(path, mounts[i].mountpoint, mlen) == 0) {
            if (mlen > best_len) { best = &mounts[i]; best_len = mlen; }
        }
    }
    return best;
}

/* ------------------------------------------------------------------
 * Public VFS API
 * ------------------------------------------------------------------ */

int vfs_read(const char *path, void *buf, uint32_t max, uint32_t *out_size) {
    struct vfs_mount *m = resolve(path);
    if (!m || !m->ops->read) return -1;
    return m->ops->read(m->priv, path, buf, max, out_size);
}

int vfs_write(const char *path, const void *data, uint32_t size) {
    struct vfs_mount *m = resolve(path);
    if (!m || !m->ops->write) return -1;
    return m->ops->write(m->priv, path, data, size);
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    struct vfs_mount *m = resolve(path);
    if (!m || !m->ops->stat) return -1;
    return m->ops->stat(m->priv, path, st);
}

int vfs_create(const char *path, uint8_t type) {
    struct vfs_mount *m = resolve(path);
    if (!m || !m->ops->create) return -1;
    return m->ops->create(m->priv, path, type);
}

int vfs_unlink(const char *path) {
    struct vfs_mount *m = resolve(path);
    if (!m || !m->ops->unlink) return -1;
    return m->ops->unlink(m->priv, path);
}

int vfs_readdir(const char *path) {
    struct vfs_mount *m = resolve(path);
    if (!m || !m->ops->readdir) return -1;
    return m->ops->readdir(m->priv, path);
}

void vfs_init(void) {
    num_mounts = 0;
    /* Mount the SMFS filesystem as root */
    vfs_mount("/", &smfs_ops, NULL);
}
