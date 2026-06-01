#include "vfs.h"
#include "fs.h"
#include "fat32.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "heap.h"
#include "signal.h"
#include "syscall.h"
#include "fsnotify.h"
#include "tmpfs.h"

#define EROFS_KERNEL 30

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
        if (wpos >= (int)sizeof(tmp) - 1) break;
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

static int smfs_symlink(void *priv, const char *target, const char *linkpath) {
    (void)priv;
    return fs_symlink(target, linkpath);
}

static int smfs_readlink(void *priv, const char *path, char *buf, int bufsize) {
    (void)priv;
    return fs_readlink(path, buf, bufsize);
}

/* ── SMTF extensions for new VFS operations ────────────────────────── */

static int smfs_fallocate(void *priv, const char *path, int mode, uint32_t offset, uint32_t len) {
    (void)priv;
    (void)mode;
    /* For SMTF, fallocate just ensures the file is large enough */
    struct vfs_stat st;
    if (fs_stat(path, &st.size, &st.type) == 0) {
        uint32_t needed = offset + len;
        if (needed > st.size) {
            /* Extend file with zeros */
            uint8_t *zero_buf = (uint8_t *)kmalloc(needed - st.size);
            if (!zero_buf) return -1;
            memset(zero_buf, 0, needed - st.size);
            int ret = fs_append(path, zero_buf, needed - st.size);
            kfree(zero_buf);
            return ret;
        }
    }
    return 0;
}

static int smfs_dedup(void *priv, const char *path1, const char *path2) {
    (void)priv;
    (void)path1;
    (void)path2;
    /* SMTF block-level dedup: compare and share blocks */
    /* For now, a simple stub that reports success */
    kprintf("[smfs] dedup: %s <-> %s\n", path1, path2);
    return 0;
}

static int smfs_resize(void *priv, uint32_t new_block_count) {
    (void)priv;
    /* For SMTF, resize adjusts the superblock's block count */
    /* In a real implementation this would grow/shrink the filesystem */
    kprintf("[smfs] resize to %u blocks\n", new_block_count);
    return 0;
}

static int smfs_journal_start(void *priv) {
    (void)priv;
    kprintf("[smfs] journal start\n");
    return 0;
}

static int smfs_journal_commit(void *priv) {
    (void)priv;
    kprintf("[smfs] journal commit\n");
    return 0;
}

static int smfs_journal_abort(void *priv) {
    (void)priv;
    kprintf("[smfs] journal abort\n");
    return 0;
}

static struct vfs_ops smfs_ops = {
    .read    = smfs_read,
    .write   = smfs_write,
    .stat    = smfs_stat,
    .create  = smfs_create,
    .unlink  = smfs_unlink,
    .readdir = smfs_readdir,
    .fallocate = smfs_fallocate,
    .dedup     = smfs_dedup,
    .resize    = smfs_resize,
    .journal_start = smfs_journal_start,
    .journal_commit = smfs_journal_commit,
    .journal_abort  = smfs_journal_abort,
    .symlink   = smfs_symlink,
    .readlink  = smfs_readlink,
};

/* ------------------------------------------------------------------
 * Mount table
 * ------------------------------------------------------------------ */

struct vfs_mount mounts[VFS_MAX_MOUNTS];
int num_mounts = 0;

/* Registered filesystem types (for /proc/filesystems) */
static struct vfs_filesystem_type fs_types[VFS_MAX_FS_TYPES];
static int num_fs_types = 0;

/* File locking: static array of 16 locks */
#define VFS_MAX_LOCKS 16
static struct file_lock file_locks[VFS_MAX_LOCKS];

/* Extended attribute storage: keyed by path hash, up to 4 per path, 16 entries total */
#define XATTR_PATH_TABLE 16
static struct {
    char path[64];
    struct xattr_entry xattrs[VFS_XATTR_PER_INODE];
    int  count;
} xattr_table[XATTR_PATH_TABLE];

/* ── ACL storage ──────────────────────────────────────────────── */
#define VFS_ACL_TABLE 16
static struct {
    char path[64];
    struct posix_acl acl;
    int in_use;
} acl_table[VFS_ACL_TABLE];

/* ── Bind mount storage ───────────────────────────────────────── */
#define VFS_MAX_BIND_MOUNTS 4
static struct {
    char source[64];
    char target[64];
    int  in_use;
} bind_mounts[VFS_MAX_BIND_MOUNTS];

int vfs_mount(const char *mountpoint, struct vfs_ops *ops, void *priv) {
    return vfs_mount_ex(mountpoint, ops, priv, 0);
}

/* Find the best-matching mount for a path */
static struct vfs_mount *resolve(const char *path);

int vfs_mount_ex(const char *mountpoint, struct vfs_ops *ops, void *priv, int flags) {
    if (num_mounts >= VFS_MAX_MOUNTS) return -1;
    size_t mlen = strlen(mountpoint);
    if (mlen >= 64) mlen = 63;
    memcpy(mounts[num_mounts].mountpoint, mountpoint, mlen);
    mounts[num_mounts].mountpoint[mlen] = '\0';
    mounts[num_mounts].ops  = ops;
    mounts[num_mounts].priv = priv;
    mounts[num_mounts].flags = flags;
    mounts[num_mounts].is_bind = 0;
    mounts[num_mounts].bind_source[0] = '\0';

    /* Handle MS_BIND: duplicate the source entry and redirect VFS ops */
    if (flags & MS_BIND) {
        /* Find the source mount */
        struct vfs_mount *src = resolve(mountpoint);
        if (src) {
            mounts[num_mounts].ops = src->ops;
            mounts[num_mounts].priv = src->priv;
            mounts[num_mounts].is_bind = 1;
            strncpy(mounts[num_mounts].bind_source, src->mountpoint, 63);
            mounts[num_mounts].bind_source[63] = '\0';
        }
    }

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

int vfs_register_filesystem(const char *name, struct vfs_ops *ops) {
    if (num_fs_types >= VFS_MAX_FS_TYPES) return -1;
    strncpy(fs_types[num_fs_types].name, name, 31);
    fs_types[num_fs_types].name[31] = '\0';
    fs_types[num_fs_types].ops = ops;
    fs_types[num_fs_types].registered = 1;
    num_fs_types++;
    return 0;
}

int vfs_list_filesystems(char names[][32], int max) {
    int n = num_fs_types < max ? num_fs_types : max;
    for (int i = 0; i < n; i++) {
        strncpy(names[i], fs_types[i].name, 31);
        names[i][31] = '\0';
    }
    return n;
}

/* ------------------------------------------------------------------
 * Public VFS API
 * ------------------------------------------------------------------ */

int vfs_read(const char *path, void *buf, uint32_t max, uint32_t *out_size) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->read) return -1;
    int r = m->ops->read(m->priv, ap, buf, max, out_size);
    if (r == 0) {
        vfs_update_atime(ap);
        fsnotify_notify(ap, FS_ACCESS);
    }
    return r;
}

int vfs_write(const char *path, const void *data, uint32_t size) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->write) return -1;

    /* Check read-only mount */
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;

    /* Enforce RLIMIT_FSIZE: check if write would exceed file size limit */
    struct process *proc = process_get_current();
    if (proc) {
        /* Get the existing file size (if any) and check the new total */
        struct vfs_stat st;
        uint32_t existing_size = 0;
        if (vfs_stat(ap, &st) == 0) {
            existing_size = st.size;
        }
        uint64_t new_total = (uint64_t)existing_size + size;
        if (new_total > proc->rlim_cur[RLIMIT_FSIZE]) {
            kprintf("RLIMIT_FSIZE: write would exceed %llu bytes\n",
                    (unsigned long long)proc->rlim_cur[RLIMIT_FSIZE]);
            /* Send SIGXFSZ */
            signal_send(proc->pid, SIGXFSZ);
            return -1;
        }
    }

    int r = m->ops->write(m->priv, ap, data, size);
    if (r == 0) {
        fsnotify_notify(ap, FS_MODIFY);
    }
    return r;
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
    /* Check read-only mount */
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;
    int r = m->ops->create(m->priv, ap, type);
    if (r == 0) {
        fsnotify_notify(ap, FS_CREATE);
    }
    return r;
}

int vfs_unlink(const char *path) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->unlink) return -1;
    /* Check read-only mount */
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;
    int r = m->ops->unlink(m->priv, ap);
    if (r == 0) {
        fsnotify_notify(ap, FS_DELETE);
    }
    return r;
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
        char (*smfs_names)[FS_MAX_NAME] = kmalloc((size_t)64 * FS_MAX_NAME);
        if (!smfs_names) return -1;
        int n = fs_list_names(ap, "", smfs_names, max < 64 ? max : 64);
        for (int i = 0; i < n; i++) {
            strncpy(names[i], smfs_names[i], 63);
            names[i][63] = '\0';
        }
        kfree(smfs_names);
        return n;
    }
    if (strcmp(m->mountpoint, "/mnt") == 0 && fat32_is_mounted()) {
        const char *rel = ap;
        if (strncmp(ap, "/mnt", 4) == 0) {
            if (ap[4] == '\0') rel = "/";
            else if (ap[4] == '/') rel = ap + 4;
        }
        char (*fat_names)[FAT32_MAX_NAME] = kmalloc((size_t)64 * FAT32_MAX_NAME);
        if (!fat_names) return -1;
        int n = fat32_list_dir(rel, fat_names, max < 64 ? max : 64);
        for (int i = 0; i < n; i++) {
            strncpy(names[i], fat_names[i], 63);
            names[i][63] = '\0';
        }
        kfree(fat_names);
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

/* ── File locking ──────────────────────────────────────────────── */

int vfs_setlk(const char *path, struct file_lock *flk, int wait) {
    (void)wait; /* blocking not yet supported */
    if (!path || !flk) return -1;

    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    if (flk->l_type == F_UNLCK) {
        /* Find and remove the lock */
        for (int i = 0; i < VFS_MAX_LOCKS; i++) {
            if (file_locks[i].used &&
                strcmp(file_locks[i].path_storage, ap) == 0 &&
                file_locks[i].l_pid == flk->l_pid) {
                file_locks[i].used = 0;
                return 0;
            }
        }
        return 0; /* nothing to unlock */
    }

    /* Check for conflicting lock */
    for (int i = 0; i < VFS_MAX_LOCKS; i++) {
        if (!file_locks[i].used) continue;
        if (strcmp(file_locks[i].path_storage, ap) != 0) continue;
        if (file_locks[i].l_pid == flk->l_pid) continue;

        /* Check lock range overlap */
        int64_t l1_start = file_locks[i].l_start;
        int64_t l1_end = (file_locks[i].l_len == 0) ? INT64_MAX :
                          file_locks[i].l_start + file_locks[i].l_len;
        int64_t l2_start = flk->l_start;
        int64_t l2_end = (flk->l_len == 0) ? INT64_MAX :
                          flk->l_start + flk->l_len;

        if (l1_start < l2_end && l2_start < l1_end) {
            /* Conflict: if non-blocking, return -EAGAIN */
            if (!wait) return -EAGAIN;
            return -EAGAIN; /* blocking not implemented yet */
        }
    }

    /* Find free slot and set the lock */
    for (int i = 0; i < VFS_MAX_LOCKS; i++) {
        if (!file_locks[i].used) {
            strncpy(file_locks[i].path_storage, ap, 63);
            file_locks[i].path_storage[63] = '\0';
            file_locks[i].l_type   = flk->l_type;
            file_locks[i].l_whence = flk->l_whence;
            file_locks[i].l_start  = flk->l_start;
            file_locks[i].l_len    = flk->l_len;
            file_locks[i].l_pid    = flk->l_pid;
            file_locks[i].used     = 1;
            return 0;
        }
    }
    return -ENOLCK;
}

int vfs_getlk(const char *path, struct file_lock *flk) {
    if (!path || !flk) return -1;
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    for (int i = 0; i < VFS_MAX_LOCKS; i++) {
        if (!file_locks[i].used) continue;
        if (strcmp(file_locks[i].path_storage, ap) != 0) continue;

        /* Check if this lock would conflict */
        int64_t l1_start = file_locks[i].l_start;
        int64_t l1_end = (file_locks[i].l_len == 0) ? INT64_MAX :
                          file_locks[i].l_start + file_locks[i].l_len;
        int64_t l2_start = flk->l_start;
        int64_t l2_end = (flk->l_len == 0) ? INT64_MAX :
                          flk->l_start + flk->l_len;

        if (l1_start < l2_end && l2_start < l1_end) {
            flk->l_type   = file_locks[i].l_type;
            flk->l_whence = file_locks[i].l_whence;
            flk->l_start  = file_locks[i].l_start;
            flk->l_len    = file_locks[i].l_len;
            flk->l_pid    = file_locks[i].l_pid;
            return 0;
        }
    }
    /* No conflicting lock found */
    flk->l_type = F_UNLCK;
    return 0;
}

/* ── Extended attributes ───────────────────────────────────────── */

/* Simple hash from path */
static int xattr_path_hash(const char *path) {
    int h = 0;
    while (*path) {
        h = (h * 31 + (unsigned char)*path) % XATTR_PATH_TABLE;
        path++;
    }
    return h;
}

int vfs_setxattr(const char *path, const char *name, const void *value, int size) {
    if (!path || !name || !value || size > VFS_XATTR_VALUE_MAX) return -EINVAL;
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    int idx = xattr_path_hash(ap);

    /* Find or create entry for this path */
    int found = -1;
    for (int i = 0; i < XATTR_PATH_TABLE; i++) {
        int ti = (idx + i) % XATTR_PATH_TABLE;
        if (xattr_table[ti].count == 0 || strcmp(xattr_table[ti].path, ap) == 0) {
            found = ti;
            break;
        }
    }
    if (found < 0) return -ENOSPC;

    if (xattr_table[found].count == 0) {
        strncpy(xattr_table[found].path, ap, 63);
        xattr_table[found].path[63] = '\0';
    }

    /* Find existing attr with same name or free slot */
    for (int i = 0; i < VFS_XATTR_PER_INODE; i++) {
        if (!xattr_table[found].xattrs[i].in_use) continue;
        if (strcmp(xattr_table[found].xattrs[i].name, name) == 0) {
            /* Update existing */
            memcpy(xattr_table[found].xattrs[i].value, value, (size_t)size);
            xattr_table[found].xattrs[i].size = size;
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < VFS_XATTR_PER_INODE; i++) {
        if (!xattr_table[found].xattrs[i].in_use) {
            strncpy(xattr_table[found].xattrs[i].name, name, VFS_XATTR_NAME_MAX - 1);
            xattr_table[found].xattrs[i].name[VFS_XATTR_NAME_MAX - 1] = '\0';
            memcpy(xattr_table[found].xattrs[i].value, value, (size_t)size);
            xattr_table[found].xattrs[i].size = size;
            xattr_table[found].xattrs[i].in_use = 1;
            xattr_table[found].count++;
            return 0;
        }
    }
    return -ENOSPC;
}

int vfs_getxattr(const char *path, const char *name, void *value, int size) {
    if (!path || !name) return -EINVAL;
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    int idx = xattr_path_hash(ap);

    for (int i = 0; i < XATTR_PATH_TABLE; i++) {
        int ti = (idx + i) % XATTR_PATH_TABLE;
        if (xattr_table[ti].count == 0) continue;
        if (strcmp(xattr_table[ti].path, ap) != 0) continue;

        for (int j = 0; j < VFS_XATTR_PER_INODE; j++) {
            if (!xattr_table[ti].xattrs[j].in_use) continue;
            if (strcmp(xattr_table[ti].xattrs[j].name, name) == 0) {
                int copy_size = (size < xattr_table[ti].xattrs[j].size) ?
                                 size : xattr_table[ti].xattrs[j].size;
                if (value && copy_size > 0) {
                    memcpy(value, xattr_table[ti].xattrs[j].value, (size_t)copy_size);
                }
                return xattr_table[ti].xattrs[j].size;
            }
        }
        return -ENODATA;
    }
    return -ENODATA;
}

int vfs_listxattr(const char *path, char *buf, int size) {
    if (!path) return -EINVAL;
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    int idx = xattr_path_hash(ap);

    for (int i = 0; i < XATTR_PATH_TABLE; i++) {
        int ti = (idx + i) % XATTR_PATH_TABLE;
        if (xattr_table[ti].count == 0) continue;
        if (strcmp(xattr_table[ti].path, ap) != 0) continue;

        int pos = 0;
        for (int j = 0; j < VFS_XATTR_PER_INODE; j++) {
            if (!xattr_table[ti].xattrs[j].in_use) continue;
            int nlen = (int)strlen(xattr_table[ti].xattrs[j].name);
            if (pos + nlen + 1 <= size) {
                if (buf) {
                    memcpy(buf + pos, xattr_table[ti].xattrs[j].name, (size_t)nlen);
                    buf[pos + nlen] = '\0';
                }
                pos += nlen + 1;
            } else {
                return -ERANGE;
            }
        }
        return pos;
    }
    return 0; /* no xattrs */
}

/* ── Bind mount support ────────────────────────────────────────── */

int vfs_bind_mount(const char *src, const char *target) {
    if (!src || !target) return -1;
    char ap_src[128]; vfs_abs_path(src, ap_src, sizeof(ap_src));
    char ap_tgt[128]; vfs_abs_path(target, ap_tgt, sizeof(ap_tgt));

    /* Find free slot */
    for (int i = 0; i < VFS_MAX_BIND_MOUNTS; i++) {
        if (!bind_mounts[i].in_use) {
            strncpy(bind_mounts[i].source, ap_src, 63);
            bind_mounts[i].source[63] = '\0';
            strncpy(bind_mounts[i].target, ap_tgt, 63);
            bind_mounts[i].target[63] = '\0';
            bind_mounts[i].in_use = 1;
            return 0;
        }
    }
    return -1;
}

int vfs_is_bind_mount(const char *path) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    for (int i = 0; i < VFS_MAX_BIND_MOUNTS; i++) {
        if (bind_mounts[i].in_use && strcmp(bind_mounts[i].target, ap) == 0)
            return 1;
    }
    return 0;
}

const char *vfs_bind_source(const char *path) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    for (int i = 0; i < VFS_MAX_BIND_MOUNTS; i++) {
        if (bind_mounts[i].in_use && strcmp(bind_mounts[i].target, ap) == 0)
            return bind_mounts[i].source;
    }
    return NULL;
}

/* ── POSIX ACL ─────────────────────────────────────────────────── */

int vfs_set_acl(const char *path, struct posix_acl *acl) {
    if (!path || !acl) return -EINVAL;
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    /* Find existing or free slot */
    int found = -1;
    for (int i = 0; i < VFS_ACL_TABLE; i++) {
        if (!acl_table[i].in_use) { found = i; break; }
        if (strcmp(acl_table[i].path, ap) == 0) { found = i; break; }
    }
    if (found < 0) return -ENOSPC;

    if (!acl_table[found].in_use) {
        strncpy(acl_table[found].path, ap, 63);
        acl_table[found].path[63] = '\0';
    }
    acl_table[found].acl = *acl;
    acl_table[found].in_use = 1;
    return 0;
}

int vfs_get_acl(const char *path, struct posix_acl *acl) {
    if (!path || !acl) return -EINVAL;
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    for (int i = 0; i < VFS_ACL_TABLE; i++) {
        if (acl_table[i].in_use && strcmp(acl_table[i].path, ap) == 0) {
            *acl = acl_table[i].acl;
            return 0;
        }
    }
    return -ENODATA;
}

/* ── Access time update ─────────────────────────────────────────── */

void vfs_update_atime(const char *path) {
    (void)path;
    /* For now, we'd update the underlying filesystem's atime.
     * Most filesystems don't have a direct atime update call.
     * The VFS stat result now includes atime field.
     * We do nothing here right now since the underlying fs manages timestamps. */
}

/* ── Filesystem statistics ──────────────────────────────────────── */

int vfs_statfs(const char *path, struct vfs_statfs *st) {
    if (!path || !st) return -EINVAL;
    (void)path;
    /* Default: return sensible values */
    memset(st, 0, sizeof(*st));
    st->f_type    = 0x01021994;  /* EXT2 magic */
    st->f_bsize   = 4096;
    st->f_blocks  = 0;
    st->f_bfree   = 0;
    st->f_bavail  = 0;
    st->f_files   = 0;
    st->f_ffree   = 0;
    st->f_namelen = 255;
    return 0;
}

int vfs_fstatfs(int fd, struct vfs_statfs *st) {
    if (!st) return -EINVAL;
    /* Resolve fd to path */
    struct process *p = process_get_current();
    if (!p) return -EBADF;
    int i = fd - 3;
    if (i < 0 || i >= PROCESS_FD_MAX || !p->fd_table[i].used) return -EBADF;
    return vfs_statfs(p->fd_table[i].path, st);
}

int vfs_truncate(const char *path, uint32_t len) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m) return -1;
    if (m->ops->truncate) {
        return m->ops->truncate(m->priv, ap, len);
    }
    return -ENOSYS;
}

/* nlink tracking table */
#define NLINK_TABLE_SIZE 128
static struct {
    char path[128];
    uint32_t nlink;
    int in_use;
} nlink_table[NLINK_TABLE_SIZE];

static uint32_t vfs_get_nlink(const char *path) {
    for (int i = 0; i < NLINK_TABLE_SIZE; i++) {
        if (nlink_table[i].in_use && strcmp(nlink_table[i].path, path) == 0)
            return nlink_table[i].nlink;
    }
    return 1; /* default: 1 link */
}

static void vfs_inc_nlink(const char *path) {
    for (int i = 0; i < NLINK_TABLE_SIZE; i++) {
        if (nlink_table[i].in_use && strcmp(nlink_table[i].path, path) == 0) {
            nlink_table[i].nlink++;
            return;
        }
    }
    /* Create new entry */
    for (int i = 0; i < NLINK_TABLE_SIZE; i++) {
        if (!nlink_table[i].in_use) {
            strncpy(nlink_table[i].path, path, 127);
            nlink_table[i].path[127] = '\0';
            nlink_table[i].nlink = 2; /* existing link + new link */
            nlink_table[i].in_use = 1;
            return;
        }
    }
}

static void vfs_dec_nlink(const char *path) {
    for (int i = 0; i < NLINK_TABLE_SIZE; i++) {
        if (nlink_table[i].in_use && strcmp(nlink_table[i].path, path) == 0) {
            if (nlink_table[i].nlink > 0) nlink_table[i].nlink--;
            return;
        }
    }
}

/* ── vfs_link: create a hard link ────────────────────────────────── */
int vfs_link(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EINVAL;
    char ap_old[128], ap_new[128];
    vfs_abs_path(oldpath, ap_old, sizeof(ap_old));
    vfs_abs_path(newpath, ap_new, sizeof(ap_new));
    
    /* Read old file data and create new entry */
    struct vfs_stat st;
    if (vfs_stat(ap_old, &st) < 0) return -ENOENT;
    if (vfs_stat(ap_new, &st) == 0) return -EEXIST;
    
    uint8_t *buf = kmalloc(st.size + 1);
    if (!buf) return -ENOMEM;
    
    uint32_t out_size = 0;
    int ret = vfs_read(ap_old, buf, st.size, &out_size);
    if (ret < 0) { kfree(buf); return ret; }
    
    ret = vfs_create(ap_new, st.type);
    if (ret < 0) { kfree(buf); return ret; }
    
    ret = vfs_write(ap_new, buf, out_size);
    kfree(buf);
    
    if (ret == 0) {
        vfs_inc_nlink(ap_old);
        vfs_inc_nlink(ap_new);
    }
    
    return ret;
}

/* ── vfs_symlink / vfs_readlink ──────────────────────────────── */
int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EINVAL;
    char ap[128];
    vfs_abs_path(linkpath, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m) return -ENOENT;
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;
    if (m->ops->symlink)
        return m->ops->symlink(m->priv, target, ap);
    /* Fallback: create a link inode and write target into it */
    int r = vfs_create(ap, 3); /* FS_TYPE_LINK */
    if (r < 0) return r;
    return vfs_write(ap, target, (uint32_t)strlen(target));
}

int vfs_readlink(const char *path, char *buf, int bufsize) {
    if (!path || !buf || bufsize <= 0) return -EINVAL;
    char ap[128];
    vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m) return -ENOENT;
    if (m->ops->readlink)
        return m->ops->readlink(m->priv, ap, buf, bufsize);
    /* Fallback: read the raw content (the target string) */
    uint32_t out_size = 0;
    int r = vfs_read(ap, buf, (uint32_t)(bufsize - 1), &out_size);
    if (r < 0) return r;
    buf[out_size] = '\0';
    return (int)out_size;
}

void vfs_init(void) {
    num_mounts = 0;
    /* Mount the SMFS filesystem as root */
    vfs_mount("/", &smfs_ops, NULL);
    /* Mount the /proc virtual filesystem */
    vfs_mount("/proc", &procfs_ops, NULL);
    /* Mount the /dev device filesystem */
    vfs_mount("/dev", &devfs_ops, NULL);
    /* Mount tmpfs as /dev/shm for POSIX shared memory & semaphores */
    tmpfs_mount();
    vfs_mount("/dev/shm", &tmpfs_vfs_ops, NULL);
    /* Initialize nlink tracking */
    memset(nlink_table, 0, sizeof(nlink_table));
    /* Initialize file locking */
    memset(file_locks, 0, sizeof(file_locks));
    /* Initialize xattr */
    memset(xattr_table, 0, sizeof(xattr_table));
}
