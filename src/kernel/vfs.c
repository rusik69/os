#include "vfs.h"
#include "fs.h"
#include "fat32.h"
#include "string.h"
#include "printf.h"
#include "page_cache.h"
#include "process.h"
#include "heap.h"
#include "signal.h"
#include "syscall.h"
#include "fsnotify.h"
#include "tmpfs.h"
#include "bufcache.h"
#include "file_lock.h"
#include "dcache.h"
#include "spinlock.h"
#include "export.h"
#include "quota.h"
#include "landlock.h"
#include "timer.h"
#include "process.h"
#include "mnt_namespace.h"

#define EROFS_KERNEL 30

/*
 * ── Dentry Cache (Item 43) — path resolution cache with LRU shrink ──────
 *
 * Fixed-size array caching vfs_stat results keyed by absolute path.
 * LRU eviction on insert when full; dcache_shrink() is called from
 * the OOM path to reclaim entries under memory pressure.
 * All operations are spinlock-protected for SMP safety.
 */

static struct dcache_entry dcache[DCACHE_SIZE];
static spinlock_t dcache_lock = SPINLOCK_INIT;
static uint32_t dcache_global_tick = 1;  /* monotonic tick for LRU aging */

void dcache_init(void)
{
    memset(dcache, 0, sizeof(dcache));
    dcache_global_tick = 1;
}

static int dcache_match(const struct dcache_entry *e, const char *path)
{
    return e->in_use && (strcmp(e->path, path) == 0);
}

struct dcache_entry *dcache_lookup(const char *path)
{
    if (!path || !path[0])
        return NULL;

    struct dcache_entry *best = NULL;
    spinlock_acquire(&dcache_lock);

    for (int i = 0; i < DCACHE_SIZE; i++) {
        if (dcache_match(&dcache[i], path)) {
            dcache[i].last_tick = dcache_global_tick++;
            best = &dcache[i];
            break;
        }
    }

    spinlock_release(&dcache_lock);
    return best;
}

void dcache_add(const char *path, void *mount,
                uint8_t type, uint32_t size,
                uint16_t uid, uint16_t gid, uint16_t mode,
                uint32_t mtime, uint32_t atime, uint32_t nlink,
                uint32_t ino)
{
    if (!path || !path[0])
        return;

    spinlock_acquire(&dcache_lock);

    int target = -1;
    int empty  = -1;
    int lru_idx = 0;
    uint32_t lru_tick = dcache[0].last_tick;

    for (int i = 0; i < DCACHE_SIZE; i++) {
        if (dcache_match(&dcache[i], path)) {
            target = i;
            break;
        }
        if (!dcache[i].in_use) {
            empty = i;
        }
        if (dcache[i].in_use && dcache[i].last_tick < lru_tick) {
            lru_tick = dcache[i].last_tick;
            lru_idx  = i;
        }
    }

    if (target < 0) {
        target = (empty >= 0) ? empty : lru_idx;
    }

    struct dcache_entry *e = &dcache[target];
    strncpy(e->path, path, DCACHE_PATH_LEN - 1);
    e->path[DCACHE_PATH_LEN - 1] = '\0';
    e->mount   = mount;
    e->type    = type;
    e->size    = size;
    e->uid     = uid;
    e->gid     = gid;
    e->mode    = mode;
    e->mtime   = mtime;
    e->atime   = atime;
    e->nlink   = nlink;
    e->ino     = ino;
    e->last_tick = dcache_global_tick++;
    e->in_use  = 1;

    spinlock_release(&dcache_lock);
}

void dcache_remove(const char *path)
{
    if (!path || !path[0])
        return;

    spinlock_acquire(&dcache_lock);
    for (int i = 0; i < DCACHE_SIZE; i++) {
        if (dcache_match(&dcache[i], path)) {
            memset(&dcache[i], 0, sizeof(dcache[i]));
            break;
        }
    }
    spinlock_release(&dcache_lock);
}

void dcache_remove_mount(void *mount)
{
    spinlock_acquire(&dcache_lock);
    for (int i = 0; i < DCACHE_SIZE; i++) {
        if (dcache[i].in_use && dcache[i].mount == mount) {
            memset(&dcache[i], 0, sizeof(dcache[i]));
        }
    }
    spinlock_release(&dcache_lock);
}

int dcache_shrink(int target_count)
{
    if (target_count <= 0)
        return 0;

    int evicted = 0;
    spinlock_acquire(&dcache_lock);

    for (int round = 0; round < target_count; round++) {
        int lru_idx = -1;
        uint32_t lru_tick = (uint32_t)-1;

        for (int i = 0; i < DCACHE_SIZE; i++) {
            if (dcache[i].in_use && dcache[i].last_tick < lru_tick) {
                lru_tick = dcache[i].last_tick;
                lru_idx  = i;
            }
        }

        if (lru_idx < 0)
            break;

        memset(&dcache[lru_idx], 0, sizeof(dcache[lru_idx]));
        evicted++;
    }

    spinlock_release(&dcache_lock);
    return evicted;
}

int dcache_evict_one(void)
{
    return dcache_shrink(1);
}

int dcache_fill_count(void)
{
    int count = 0;
    spinlock_acquire(&dcache_lock);
    for (int i = 0; i < DCACHE_SIZE; i++) {
        if (dcache[i].in_use)
            count++;
    }
    spinlock_release(&dcache_lock);
    return count;
}

int dcache_capacity(void)
{
    return DCACHE_SIZE;
}

/* ── End of dentry cache ──────────────────────────────────────── */

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
    st->atime = 0;
    st->nlink = 1;
    /* Populate inode number from the underlying filesystem */
    int ino = fs_get_ino(path);
    st->ino = (ino >= 0) ? (uint32_t)ino : 0;
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
    uint32_t fs_size = 0;
    uint8_t  fs_type = 0;
    if (fs_stat(path, &fs_size, &fs_type) == 0) {
        uint32_t needed = offset + len;
        if (needed > fs_size) {
            /* Extend file with zeros */
            uint8_t *zero_buf = (uint8_t *)kmalloc(needed - fs_size);
            if (!zero_buf) return -1;
            memset(zero_buf, 0, needed - fs_size);
            int ret = fs_append(path, zero_buf, needed - fs_size);
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

/* ── SMFS set_time: set file access and modification times ──────── */

static int smfs_set_time(void *priv, const char *path,
                          uint64_t atime_sec, uint64_t atime_nsec,
                          uint64_t mtime_sec, uint64_t mtime_nsec)
{
    (void)priv;
    /* SMFS stores timestamps as uint32_t seconds (truncating nanoseconds).
     * If either value is UTIME_OMIT (passed through from caller), skip it. */
    if (atime_nsec != UTIME_OMIT) {
        /* SMFS doesn't have a dedicated atime field; we map atime writes
         * to mtime since the on-disk inode only has one timestamp field. */
        fs_set_mtime(path, (uint32_t)atime_sec);
    }
    if (mtime_nsec != UTIME_OMIT) {
        fs_set_mtime(path, (uint32_t)mtime_sec);
    }
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
    .set_time  = smfs_set_time,
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
/* no more internal file_locks array — delegated to file_lock.c */

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

/*
 * vfs_force_readonly — remount a filesystem read-only on fatal error.
 *
 * Called by a filesystem driver when it detects unrecoverable corruption
 * (e.g. invalid superblock, corrupted inode bitmap, disk I/O failure on
 * a critical structure).  The VFS write path already checks the MS_RDONLY
 * flag on the mount and returns -EROFS, preventing further data loss.
 *
 * @path    Any path within the filesystem to force read-only.
 * @reason  Human-readable description of the corruption (logged once).
 *
 * Returns 0 on success, -1 if the path has no matching mount.
 */
int vfs_force_readonly(const char *path, const char *reason)
{
    struct vfs_mount *m = resolve(path);
    if (!m)
        return -1;

    if (m->flags & MS_RDONLY)
        return 0;  /* already read-only — no-op */

    m->flags |= MS_RDONLY;
    kprintf("[!!] VFS: FORCED READ-ONLY on '%s': %s\n",
            m->mountpoint, reason ? reason : "unknown error");
    return 0;
}
EXPORT_SYMBOL(vfs_force_readonly);

/* Find the best-matching mount for a path.
 * First checks the current process's mount namespace (if any),
 * then falls back to the global mount table. */
static struct vfs_mount *resolve(const char *path) {
    struct vfs_mount *best = NULL;
    size_t best_len = 0;

    /* Check mount namespace first */
    struct mnt_namespace *ns = mnt_ns_current();
    if (ns) {
        struct vfs_mount *ns_best = mnt_ns_resolve(ns, path);
        if (ns_best)
            return ns_best;
    }

    /* Fall back to global mount table */
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

    /* Check Landlock read-file permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_READ_FILE) < 0)
            return -EACCES;
    }

    /* Check mandatory locks before read */
    {
        int lock_ret = file_lock_check_mandatory(ap, 0);
        if (lock_ret < 0)
            return lock_ret; /* -EAGAIN if blocked by mandatory write lock */
    }

    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->read) return -1;
    int r = m->ops->read(m->priv, ap, buf, max, out_size);
    /* If the read fails with an I/O or corruption error, auto-remount
     * read-only to prevent further damage.  Reads into corrupted areas
     * can propagate bad data; stopping writes protects data integrity. */
    if (r < 0 && (r == -EIO || r == -EFSCORRUPTED)) {
        if (!(m->flags & MS_RDONLY)) {
            m->flags |= MS_RDONLY;
            kprintf("[!!] VFS: read on '%s' failed with %d; "
                    "auto-remounted read-only\n", m->mountpoint, r);
        }
    }
    if (r == 0) {
        vfs_update_atime(ap);
        fsnotify_notify(ap, FS_ACCESS);
    }
    return r;
}

int vfs_write(const char *path, const void *data, uint32_t size) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));
    uint32_t existing_size = 0;

    /* Check Landlock write-file permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_WRITE_FILE) < 0)
            return -EACCES;
    }

    /* Check mandatory locks before write */
    {
        int lock_ret = file_lock_check_mandatory(ap, 1);
        if (lock_ret < 0)
            return lock_ret; /* -EAGAIN if blocked by mandatory read or write lock */
    }

    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->write) return -1;

    /* Check read-only mount */
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;

    /* Enforce RLIMIT_FSIZE: check if write would exceed file size limit */
    struct process *proc = process_get_current();
    if (proc) {
        /* Get the existing file size (if any) and check the new total */
        struct vfs_stat st;
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

    /* Enforce filesystem quotas: check block quota before write */
    {
        struct process *qproc = process_get_current();
        uint16_t uid = qproc ? (uint16_t)qproc->uid : 0;
        uint32_t blocks_needed = bytes_to_blocks(size);
        int qret = vfs_check_quota_blocks(uid, blocks_needed);
        if (qret < 0) {
            kprintf("QUOTA: write denied for UID %u (needs %u blocks)\n",
                    (unsigned int)uid, (unsigned int)blocks_needed);
            return -EDQUOT;
        }
    }

    int r = m->ops->write(m->priv, ap, data, size);
    /* If the filesystem returns an I/O or corruption error, automatically
     * remount read-only to prevent further data loss.  The filesystem
     * driver should also call vfs_force_readonly() directly when it
     * detects the underlying corruption, but this VFS-level fallback
     * catches cases where the error surfaces through the write path. */
    if (r < 0 && (r == -EIO || r == -EFSCORRUPTED || r == -EROFS_KERNEL)) {
        if (!(m->flags & MS_RDONLY)) {
            m->flags |= MS_RDONLY;
            kprintf("[!!] VFS: write to '%s' failed with %d; "
                    "auto-remounted read-only\n", m->mountpoint, r);
        }
    }
    if (r == 0) {
        fsnotify_notify(ap, FS_MODIFY);
        /* File metadata (size, mtime) changed — invalidate cache */
        dcache_remove(ap);

        /* Update block quota usage after successful write */
        struct process *qproc = process_get_current();
        if (qproc) {
            uint32_t new_size;
            struct vfs_stat st2;
            if (vfs_stat(ap, &st2) == 0)
                new_size = st2.size;
            else
                new_size = existing_size + size;
            uint32_t new_blocks = bytes_to_blocks(new_size);
            uint32_t old_blocks = bytes_to_blocks(existing_size);
            int32_t delta = (int32_t)new_blocks - (int32_t)old_blocks;
            if (delta != 0)
                vfs_update_quota_blocks((uint16_t)qproc->uid, delta);
        }
    }
    return r;
}

/*
 * vfs_append — Append data to the end of a file.
 *
 * Reads the existing file content, concatenates the new data, and writes
 * the combined result back.  This is used by the O_APPEND open flag.
 *
 * For small files this read-modify-write approach is acceptable; for
 * production use with large files a filesystem-level append or pwrite
 * operation would be more efficient.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
int vfs_append(const char *path, const void *data, uint32_t size)
{
    if (!path || !data || size == 0)
        return -EINVAL;

    /* Read the existing file content if any */
    uint32_t old_size = 0;
    void *old_buf = NULL;

    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0 && st.size > 0) {
        old_size = st.size;
        old_buf = kmalloc(old_size);
        if (!old_buf)
            return -ENOMEM;

        if (vfs_read(path, old_buf, old_size, NULL) < 0) {
            kfree(old_buf);
            return -EIO;
        }
    }

    /* Allocate a combined buffer */
    uint32_t new_total = old_size + size;
    void *combined = kmalloc(new_total);
    if (!combined) {
        kfree(old_buf);
        return -ENOMEM;
    }

    /* Copy old content followed by new data */
    if (old_size > 0 && old_buf) {
        __builtin_memcpy(combined, old_buf, old_size);
    }
    __builtin_memcpy((uint8_t *)combined + old_size, data, size);

    kfree(old_buf);

    /* Write the combined content back */
    int ret = vfs_write(path, combined, new_total);
    kfree(combined);

    if (ret < 0)
        return ret;

    return 0;
}

/*
 * vfs_create — create a new file or directory.
 * @path:  absolute path for the new entry.
 * @type:  1 = file, 2 = directory.
 * the byte range to page cache blocks and prefetches them from the
 * backing store.  For memory-backed filesystems (tmpfs, procfs, etc.)
 * this is a no-op since there is no backing store to prefetch from.
 *
 * Returns 0 on success, or negative on error.
 */
int vfs_readahead(const char *path, uint32_t offset, uint32_t count) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    /* Check Landlock read-file permission (same as vfs_read) */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_READ_FILE) < 0)
            return -EACCES;
    }

    /* Check mandatory locks before readahead */
    {
        int lock_ret = file_lock_check_mandatory(ap, 0);
        if (lock_ret < 0)
            return lock_ret;
    }

    struct vfs_mount *m = resolve(ap);
    if (!m) return -1;

    /* For block-device-backed filesystems, delegate to the legacy
     * filesystem's readahead which integrates with the page cache.
     * The SMFS (Simple Memory File System) on "/" is backed by
     * the legacy ATA block filesystem (fs.c). */
    if (m->ops == &smfs_ops) {
        return fs_readahead(ap, offset, count);
    }

    /* For other filesystems (tmpfs, procfs, sysfs, etc.), readahead
     * is a no-op — they have no backing store to prefetch from. */
    return 0;
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    /* Check Landlock read-file permission (reading metadata) */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_READ_FILE) < 0)
            return -EACCES;
    }

    /* Try the dentry cache first */
    struct dcache_entry *de = dcache_lookup(ap);
    if (de) {
        st->size  = de->size;
        st->type  = de->type;
        st->uid   = de->uid;
        st->gid   = de->gid;
        st->mode  = de->mode;
        st->mtime = de->mtime;
        st->atime = de->atime;
        st->nlink = de->nlink;
        st->ino   = de->ino;
        return 0;
    }

    /* Cache miss — query the real filesystem */
    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->stat) return -1;
    int r = m->ops->stat(m->priv, ap, st);
    if (r == 0) {
        /* Cache the result for future lookups */
        dcache_add(ap, (void *)m, st->type, st->size,
                   st->uid, st->gid, st->mode,
                   st->mtime, st->atime, st->nlink,
                   st->ino);
    }
    return r;
}

int vfs_create(const char *path, uint8_t type) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    /* Check Landlock write-file permission (creating a new file writes to parent dir) */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_WRITE_FILE) < 0)
            return -EACCES;
    }

    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->create) return -1;
    /* Check read-only mount */
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;

    /* Enforce filesystem quotas: check inode quota before create */
    {
        struct process *proc = process_get_current();
        if (proc) {
            int qret = vfs_check_quota_inodes((uint16_t)proc->uid);
            if (qret < 0) {
                kprintf("QUOTA: inode creation denied for UID %u\n",
                        (unsigned int)proc->uid);
                return -EDQUOT;
            }
        }
    }

    int r = m->ops->create(m->priv, ap, type);
    if (r == 0) {
        fsnotify_notify(ap, FS_CREATE);
        /* Invalidate the parent directory's cache entry */
        dcache_remove(ap);
        char parent[128];
        strncpy(parent, ap, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            dcache_remove(parent);
        } else if (slash == parent) {
            dcache_remove("/");
        }

        /* Update inode quota after successful creation */
        struct process *proc = process_get_current();
        if (proc)
            vfs_update_quota_inodes((uint16_t)proc->uid, 1);
    }
    return r;
}

int vfs_unlink(const char *path) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    /* Check Landlock write-file permission (removing a file writes to parent dir) */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_WRITE_FILE) < 0)
            return -EACCES;
    }

    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->unlink) return -1;
    /* Check read-only mount */
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;

    /* Save stat before unlink for quota adjustment */
    struct vfs_stat pre_st;
    int have_pre_stat = (vfs_stat(ap, &pre_st) == 0) ? 1 : 0;

    int r = m->ops->unlink(m->priv, ap);
    if (r == 0) {
        fsnotify_notify(ap, FS_DELETE);
        dcache_remove(ap);
        char parent[128];
        strncpy(parent, ap, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            dcache_remove(parent);
        } else if (slash == parent) {
            dcache_remove("/");
        }

        /* Update quota after successful unlink */
        struct process *proc = process_get_current();
        if (proc) {
            vfs_update_quota_inodes((uint16_t)proc->uid, -1);
            if (have_pre_stat) {
                uint32_t blocks_freed = bytes_to_blocks(pre_st.size);
                if (blocks_freed > 0)
                    vfs_update_quota_blocks((uint16_t)proc->uid,
                                            -(int32_t)blocks_freed);
            }
        }
    }
    return r;
}

int vfs_readdir(const char *path) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    /* Check Landlock read-dir permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_READ_DIR) < 0)
            return -EACCES;
    }

    struct vfs_mount *m = resolve(ap);
    if (!m || !m->ops->readdir) return -1;
    return m->ops->readdir(m->priv, ap);
}

int vfs_readdir_names(const char *path, char names[][64], int max) {
    char ap[128]; vfs_abs_path(path, ap, sizeof(ap));

    /* Check Landlock read-dir permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_READ_DIR) < 0)
            return -EACCES;
    }

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
    /* Try process namespace first */
    struct mnt_namespace *ns = mnt_ns_current();
    if (ns) {
        return mnt_ns_list_mounts(ns, mounts_out, max);
    }

    /* Fall back to global mount table */
    int n = num_mounts < max ? num_mounts : max;
    for (int i = 0; i < n; i++) {
        strncpy(mounts_out[i], mounts[i].mountpoint, 63);
        mounts_out[i][63] = '\0';
    }
    return n;
}

/* ── File locking ──────────────────────────────────────────────── */

int vfs_setlk(const char *path, struct file_lock *flk, int wait)
{
    return file_lock_set(path, flk, wait);
}

int vfs_getlk(const char *path, struct file_lock *flk)
{
    return file_lock_get(path, flk);
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

/* ── Set file times (utimensat / futimens) ────────────────────── */

/* Internal helper: resolve path, call filesystem's set_time, update dcache. */
static int vfs_do_set_time(const char *abs_path, const struct timespec times[2])
{
    struct vfs_mount *m = resolve(abs_path);
    if (!m) return -ENOENT;

    if (!m->ops->set_time)
        return -EOPNOTSUPP;  /* filesystem doesn't support timestamp changes */

    /* Resolve times[] to actual values.
     * NULL or UTIME_NOW → current time (boot seconds + nsec).
     * UTIME_OMIT → leave unchanged (pass caller's value and let FS handle it).
     */
    uint64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec;
    uint64_t now_sec  = timer_get_ticks() / TIMER_FREQ;
    uint64_t now_nsec = (timer_get_ticks() % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);

    if (times == NULL) {
        /* Both to current time */
        atime_sec = now_sec;  atime_nsec = now_nsec;
        mtime_sec = now_sec;  mtime_nsec = now_nsec;
    } else {
        /* Process times[0] (atime) */
        if (times[0].tv_nsec == UTIME_NOW || (times[0].tv_sec == 0 && times[0].tv_nsec == UTIME_NOW)) {
            atime_sec = now_sec;  atime_nsec = now_nsec;
        } else if (times[0].tv_nsec == UTIME_OMIT) {
            atime_sec = 0;  atime_nsec = UTIME_OMIT;
        } else {
            atime_sec = times[0].tv_sec;
            atime_nsec = times[0].tv_nsec;
        }
        /* Process times[1] (mtime) */
        if (times[1].tv_nsec == UTIME_NOW || (times[1].tv_sec == 0 && times[1].tv_nsec == UTIME_NOW)) {
            mtime_sec = now_sec;  mtime_nsec = now_nsec;
        } else if (times[1].tv_nsec == UTIME_OMIT) {
            mtime_sec = 0;  mtime_nsec = UTIME_OMIT;
        } else {
            mtime_sec = times[1].tv_sec;
            mtime_nsec = times[1].tv_nsec;
        }
    }

    int ret = m->ops->set_time(m->priv, abs_path,
                               atime_sec, atime_nsec,
                               mtime_sec, mtime_nsec);
    if (ret == 0) {
        /* Invalidate dentry cache for the path so next stat re-reads metadata */
        dcache_remove(abs_path);
    }
    return ret;
}

int vfs_set_time(const char *path, const struct timespec times[2])
{
    if (!path || !path[0]) return -EINVAL;
    char ap[128];
    vfs_abs_path(path, ap, sizeof(ap));
    return vfs_do_set_time(ap, times);
}

int vfs_fset_time(int fd, const struct timespec times[2])
{
    struct process *proc = process_get_current();
    if (!proc) return -EPERM;
    if (fd < 0 || fd >= PROCESS_FD_MAX) return -EBADF;
    struct process_fd *pfd = &proc->fd_table[fd];
    if (!pfd->used) return -EBADF;
    return vfs_do_set_time(pfd->path, times);
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

    /* Check Landlock write-file permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_WRITE_FILE) < 0)
            return -EACCES;
    }

    struct vfs_mount *m = resolve(ap);
    if (!m) return -1;

    /* Get the old size before truncate for quota adjustment */
    struct vfs_stat old_st;
    int have_old = (vfs_stat(ap, &old_st) == 0) ? 1 : 0;

    if (m->ops->truncate) {
        int r = m->ops->truncate(m->priv, ap, len);
        if (r == 0) {
            /* Update block quota after truncate */
            struct process *proc = process_get_current();
            if (proc && have_old) {
                uint32_t old_blocks = bytes_to_blocks(old_st.size);
                uint32_t new_blocks = bytes_to_blocks(len);
                int32_t delta = (int32_t)new_blocks - (int32_t)old_blocks;
                if (delta != 0)
                    vfs_update_quota_blocks((uint16_t)proc->uid, delta);
            }
        }
        return r;
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

    /* Check Landlock write-file permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_WRITE_FILE) < 0)
            return -EACCES;
    }

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

    /* Check Landlock read-file permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_READ_FILE) < 0)
            return -EACCES;
    }

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

/* ── vfs_mknod ────────────────────────────────────────────────── */
int vfs_mknod(const char *path, uint16_t mode, uint16_t dev_major, uint16_t dev_minor) {
    if (!path) return -EINVAL;
    char ap[128];
    vfs_abs_path(path, ap, sizeof(ap));

    /* Check Landlock write-file permission */
    {
        struct process *proc = process_get_current();
        if (proc && landlock_check_path(proc, ap, LANDLOCK_ACCESS_FS_WRITE_FILE) < 0)
            return -EACCES;
    }

    struct vfs_mount *m = resolve(ap);
    if (!m) return -ENOENT;
    if (m->flags & MS_RDONLY) return -EROFS_KERNEL;
    if (m->ops->mknod)
        return m->ops->mknod(m->priv, ap, mode, dev_major, dev_minor);
    return -EOPNOTSUPP;
}

/* ── vfs_flush: flush cached writes for a filesystem identified by path ── */

int vfs_flush(const char *path) {
    if (!path) return -EINVAL;
    char ap[128];
    vfs_abs_path(path, ap, sizeof(ap));
    struct vfs_mount *m = resolve(ap);
    if (!m) return -ENOENT;

    int ret = 0;

    /* Call the filesystem-specific flush op if present */
    if (m->ops->flush) {
        ret = m->ops->flush(m->priv);
    }

    /* Flush the buffer cache (block device cache) to backing store */
    bufcache_flush();

    /* Flush the page cache (file data cache) dirty pages to disk */
    page_cache_flush();

    return ret;
}

/* ── vfs_sync_all: sync all mounted filesystems ───────────────────── */

int vfs_sync_all(void) {
    int ret = 0;
    for (int i = 0; i < num_mounts; i++) {
        if (mounts[i].ops->flush) {
            int r = mounts[i].ops->flush(mounts[i].priv);
            if (r < 0) ret = r;
        }
    }
    /* Flush the global buffer cache */
    bufcache_flush();

    /* Flush the page cache (file data cache) dirty pages to disk */
    page_cache_flush();

    return ret;
}

void vfs_init(void) {
    num_mounts = 0;
    dcache_init();
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
    memset(mounts, 0, sizeof(mounts));
    /* Initialize xattr */
    memset(xattr_table, 0, sizeof(xattr_table));

    /* Create the root mount namespace (Item 112) */
    mnt_ns_create_root();
}

/* ── root mount index tracking ───────────────────────────────────── */

/* Return the index of the root mount (mountpoint == "/") or -1 */
static int root_mount_index(void) {
    for (int i = 0; i < num_mounts; i++) {
        if (mounts[i].mountpoint[0] == '/' && mounts[i].mountpoint[1] == '\0')
            return i;
    }
    return -1;
}

/* ── pivot_root — swap root mount (Item 118) ─────────────────────────
 *
 * pivot_root(new_root, put_old):
 *   Makes new_root the process's root filesystem.
 *   Moves the current root to put_old (which must be under new_root).
 *
 * Both new_root and put_old must be directories.
 * put_old must be underneath new_root.
 * new_root must be a mount point.
 *
 * After pivot_root:
 *   - The root mount entry serves new_root's filesystem at "/".
 *   - The new_root mount entry serves the old root at "put_old".
 */
int vfs_pivot_root(const char *new_root, const char *put_old) {
    if (!new_root || !put_old || !new_root[0] || !put_old[0]) return -EINVAL;

    int root_idx = root_mount_index();
    if (root_idx < 0) return -ENOENT;

    /* Find the mount entry for new_root */
    int new_idx = -1;
    for (int i = 0; i < num_mounts; i++) {
        if (mounts[i].mountpoint[0] && strcmp(mounts[i].mountpoint, new_root) == 0) {
            new_idx = i;
            break;
        }
    }
    if (new_idx < 0)
        return -EINVAL;

    /* new_root must not be the root itself */
    if (new_idx == root_idx) return -EINVAL;

    /* Verify put_old is under new_root */
    size_t nr_len = strlen(new_root);
    if (strncmp(put_old, new_root, nr_len) != 0) return -EINVAL;
    if (put_old[nr_len] != '/') return -EINVAL;

    /* Ensure put_old exists as a directory */
    struct vfs_stat st;
    int ret = vfs_stat(put_old, &st);
    if (ret < 0) {
        /* Try to create put_old as a directory */
        ret = vfs_create(put_old, 2); /* type 2 = directory */
        if (ret < 0) return -ENOTDIR;
    } else if (st.type != 2) {
        return -ENOTDIR;
    }

    /* Swap ops/priv between root and new_root entries.
     *
     * After pivot_root:
     *   - Root entry (mountpoint="/") gets new_root's filesystem → process
     *     now sees new_root as root.
     *   - new_root entry gets old root's filesystem at put_old → old root
     *     is accessible there.
     */
    struct vfs_ops *tmp_ops   = mounts[root_idx].ops;
    void           *tmp_priv  = mounts[root_idx].priv;
    int             tmp_flags = mounts[root_idx].flags;

    mounts[root_idx].ops   = mounts[new_idx].ops;
    mounts[root_idx].priv  = mounts[new_idx].priv;
    mounts[root_idx].flags = mounts[new_idx].flags;

    mounts[new_idx].ops   = tmp_ops;
    mounts[new_idx].priv  = tmp_priv;
    mounts[new_idx].flags = tmp_flags;

    /* Update the mountpoint of the old-root-turned-new-root entry to put_old */
    strncpy(mounts[new_idx].mountpoint, put_old, 63);
    mounts[new_idx].mountpoint[63] = '\0';

    kprintf("[VFS] pivot_root: new_root='%s' -> '/', put_old='%s'\\n", new_root, put_old);
    return 0;
}

/* ── Exported symbols for loadable kernel modules ─────────────────── */
EXPORT_SYMBOL(vfs_register_filesystem);
EXPORT_SYMBOL(vfs_mount);
EXPORT_SYMBOL(vfs_mount_ex);
EXPORT_SYMBOL(vfs_read);
EXPORT_SYMBOL(vfs_write);
EXPORT_SYMBOL(vfs_stat);
EXPORT_SYMBOL(vfs_create);
EXPORT_SYMBOL(vfs_unlink);
EXPORT_SYMBOL(vfs_readdir_names);
EXPORT_SYMBOL(vfs_truncate);
EXPORT_SYMBOL(vfs_link);
EXPORT_SYMBOL(vfs_symlink);
EXPORT_SYMBOL(vfs_readlink);
EXPORT_SYMBOL(vfs_mknod);
EXPORT_SYMBOL(vfs_flush);
EXPORT_SYMBOL(vfs_sync_all);
EXPORT_SYMBOL(vfs_statfs);
EXPORT_SYMBOL(vfs_bind_mount);
