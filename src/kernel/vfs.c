#include "vfs.h"
#include "fs.h"
#include "string.h"
#include "printf.h"
#include "process.h"

extern struct vfs_ops procfs_ops;
extern struct vfs_ops devfs_ops;

/*
 * Resolve a possibly-relative path against the current process's cwd.
 * If path already starts with '/', copies it verbatim.
 * Also normalises ".." components (one level only).
 */
static void vfs_abs_path(const char *path, char *out, int out_max) {
    if (!path || !path[0]) { out[0] = '/'; out[1] = '\0'; return; }
    if (path[0] == '/') {
        strncpy(out, path, out_max - 1); out[out_max - 1] = '\0'; return;
    }
    /* Relative: prepend cwd */
    const char *cwd = "/";
    struct process *p = process_get_current();
    if (p && p->cwd[0]) cwd = p->cwd;
    int cl = (int)strlen(cwd);
    int pl = (int)strlen(path);
    /* Handle special components */
    if (pl == 1 && path[0] == '.') { strncpy(out, cwd, out_max-1); out[out_max-1]='\0'; return; }
    if (pl == 2 && path[0]=='.' && path[1]=='.') {
        /* Go up one level */
        strncpy(out, cwd, out_max-1); out[out_max-1]='\0';
        int len = (int)strlen(out);
        while (len > 1 && out[len-1] != '/') len--;
        if (len > 1) len--; /* remove trailing slash too */
        if (len < 1) len = 1;
        out[len] = '\0'; return;
    }
    /* Combine cwd + path */
    if (cl + 1 + pl >= out_max) { out[0] = '/'; out[1] = '\0'; return; }
    memcpy(out, cwd, cl);
    if (out[cl-1] != '/') out[cl++] = '/';
    memcpy(out + cl, path, pl + 1);
}

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
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->read) return -1;
    return m->ops->read(m->priv, ap, buf, max, out_size);
}

int vfs_write(const char *path, const void *data, uint32_t size) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->write) return -1;
    return m->ops->write(m->priv, ap, data, size);
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->stat) return -1;
    return m->ops->stat(m->priv, ap, st);
}

int vfs_create(const char *path, uint8_t type) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->create) return -1;
    return m->ops->create(m->priv, ap, type);
}

int vfs_unlink(const char *path) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->unlink) return -1;
    return m->ops->unlink(m->priv, ap);
}

int vfs_readdir(const char *path) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->readdir) return -1;
    return m->ops->readdir(m->priv, ap);
}

void vfs_init(void) {
    num_mounts = 0;
    /* Mount the SMFS filesystem as root */
    vfs_mount("/", &smfs_ops, NULL);
    /* Mount the /proc virtual filesystem */
    vfs_mount("/proc", &procfs_ops, NULL);
    /* Mount the /dev device filesystem */
    vfs_mount("/dev", &devfs_ops, NULL);
}
