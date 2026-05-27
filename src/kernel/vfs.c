#include "vfs.h"
#include "fs.h"
#include "fat32.h"
#include "string.h"
#include "printf.h"
#include "process.h"

extern struct vfs_ops procfs_ops;
extern struct vfs_ops devfs_ops;

/*
 * Resolve a possibly-relative path against the current process's cwd.
 * If path already starts with '/', copies it verbatim.
 * Normalises "." and ".." components.
 */
static void vfs_abs_path(const char *path, char *out, int out_max) {
    char tmp[128];
    int wpos = 0;
    int i;

    if (!path || !path[0]) { out[0] = '/'; out[1] = '\0'; return; }

    /* Start with cwd for relative paths, empty for absolute */
    if (path[0] == '/') {
        wpos = 0;
        i = 0;
    } else {
        const char *cwd = "/";
        struct process *p = process_get_current();
        if (p && p->cwd[0]) cwd = p->cwd;
        int cwdl = (int)strlen(cwd);
        if (cwdl >= (int)sizeof(tmp)) cwdl = (int)sizeof(tmp) - 1;
        memcpy(tmp, cwd, cwdl);
        wpos = cwdl;
        i = 0;
    }

    /* Normalize the starting path: remove trailing slash unless it's just "/" */
    while (wpos > 1 && tmp[wpos - 1] == '/') wpos--;

    /* Tokenize the input path and process each component */
    while (path[i]) {
        /* Skip slashes */
        while (path[i] == '/') i++;
        if (!path[i]) break;

        /* Read the next component */
        int comp_start = i;
        while (path[i] && path[i] != '/') i++;
        int comp_len = i - comp_start;

        if (comp_len == 1 && path[comp_start] == '.') {
            /* "." — skip */
            continue;
        }
        if (comp_len == 2 && path[comp_start] == '.' && path[comp_start + 1] == '.') {
            /* ".." — go up one level */
            while (wpos > 0 && tmp[wpos - 1] != '/') wpos--;
            if (wpos > 0) wpos--; /* remove the slash too */
            if (wpos < 0) wpos = 0;
            continue;
        }
        /* Regular component: append */
        if (wpos > 0 && tmp[wpos - 1] != '/') {
            if (wpos >= (int)sizeof(tmp) - 1) break;
            tmp[wpos++] = '/';
        } else if (wpos == 0) {
            if (wpos >= (int)sizeof(tmp) - 1) break;
            tmp[wpos++] = '/';
        }
        if (wpos + comp_len >= (int)sizeof(tmp) - 1)
            comp_len = (int)sizeof(tmp) - 1 - wpos;
        memcpy(tmp + wpos, path + comp_start, comp_len);
        wpos += comp_len;
    }

    /* Ensure result is non-empty */
    if (wpos == 0) { tmp[wpos++] = '/'; }
    tmp[wpos] = '\0';

    /* Copy to output with size limit */
    strncpy(out, tmp, out_max - 1);
    out[out_max - 1] = '\0';
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
    uint32_t size;
    uint8_t type;
    uint16_t uid = 0, gid = 0, mode = 0;
    int r = fs_stat_ex(path, &size, &type, &uid, &gid, &mode);
    if (r < 0) return r;
    st->size  = size;
    st->type  = type;
    st->uid   = uid;
    st->gid   = gid;
    st->mode  = mode;
    st->mtime = (uint32_t)fs_stat_mtime(path);
    if ((int32_t)st->mtime < 0) st->mtime = 0;
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
        if (mlen == 0) continue;
        /* Root mount "/" matches everything */
        if (mlen == 1 && mounts[i].mountpoint[0] == '/') {
            if (1 > best_len) { best = &mounts[i]; best_len = 1; }
            continue;
        }
        if (strncmp(path, mounts[i].mountpoint, mlen) == 0) {
            /* Exact match or path continues after a separator */
            if (path[mlen] == '\0' || path[mlen] == '/') {
                if (mlen > best_len) { best = &mounts[i]; best_len = mlen; }
            }
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

int vfs_readdir_names(const char *path, char names[][64], int max) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m) return -1;
    if (strcmp(m->mountpoint, "/") == 0) {
        char smfs_names[64][FS_MAX_NAME];
        int n = fs_list_names(ap, "", smfs_names, max < 64 ? max : 64);
        for (int i = 0; i < n; i++) {
            strncpy(names[i], smfs_names[i], 63);
            names[i][63] = '\0';
        }
        return n;
    }
    if (strcmp(m->mountpoint, "/mnt") == 0 && fat32_is_mounted()) {
        const char *rel = ap;
        if (strncmp(ap, "/mnt", 4) == 0) {
            if (ap[4] == '\0') rel = "/";
            else if (ap[4] == '/') rel = ap + 4;
        }
        char fat_names[64][FAT32_MAX_NAME];
        int n = fat32_list_dir(rel, fat_names, max < 64 ? max : 64);
        for (int i = 0; i < n; i++) {
            strncpy(names[i], fat_names[i], 63);
            names[i][63] = '\0';
        }
        return n;
    }
    return -1;
}

int vfs_list_mountpoints(char mounts_out[][64], int max) {
    int n = num_mounts < max ? num_mounts : max;
    for (int i = 0; i < n; i++) {
        strncpy(mounts_out[i], mounts[i].mountpoint, 63);
        mounts_out[i][63] = '\0';
    }
    return n;
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
