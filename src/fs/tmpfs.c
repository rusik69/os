#define KERNEL_INTERNAL
#include "types.h"
#include "tmpfs.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "fs.h" /* for FS_MODE_FILE, FS_MODE_DIR etc */
#include "errno.h"

static struct tmpfs_inode inodes[TMPFS_MAX_INODES];
static int tmpfs_mounted = 0;

/* ── Per-mount size accounting ──────────────────────────────────────── */
static uint64_t tmpfs_size_limit  = TMPFS_SIZE_UNLIMITED; /* 0 = unlimited */
static uint64_t tmpfs_used_bytes  = 0;                     /* total data bytes */

/* ── helpers ───────────────────────────────────────────────────── */

static int alloc_inode(void) {
    for (int i = 1; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) {
            inodes[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static void free_inode(int idx) {
    if (idx < 0 || idx >= TMPFS_MAX_INODES) return;
    /* Subtract freed data from the used-bytes counter */
    if (inodes[idx].data && inodes[idx].size > 0) {
        if (tmpfs_used_bytes >= inodes[idx].size)
            tmpfs_used_bytes -= inodes[idx].size;
        else
            tmpfs_used_bytes = 0;
    }
    if (inodes[idx].data) kfree(inodes[idx].data);
    inodes[idx].in_use = 0;
    inodes[idx].data = NULL;
    inodes[idx].size = 0;
}

static int find_inode(const char *path) {
    if (!path || path[0] != '/') return -1;
    if (path[1] == '\0') return 0; /* root dir */
    const char *name = path + 1;
    /* skip trailing slash */
    int len = (int)strlen(name);
    if (len > 0 && name[len-1] == '/') len--;
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) continue;
        if ((int)strlen(inodes[i].name) == len &&
            memcmp(inodes[i].name, name, (size_t)len) == 0)
            return i;
    }
    return -1;
}

static int find_inode_in_dir(int dir_idx, const char *name) {
    if (dir_idx < 0 || dir_idx >= TMPFS_MAX_INODES) return -1;
    if (!inodes[dir_idx].in_use || inodes[dir_idx].type != TMPFS_TYPE_DIR)
        return -1;
    int len = (int)strlen(name);
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) continue;
        if (inodes[i].parent != (uint32_t)dir_idx) continue;
        if ((int)strlen(inodes[i].name) == len &&
            memcmp(inodes[i].name, name, (size_t)len) == 0)
            return i;
    }
    return -1;
}

/* ── VFS operations ────────────────────────────────────────────── */

static int tmpfs_read(void *priv, const char *path, void *buf, uint32_t max, uint32_t *out) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_FILE)
        return -1;
    uint32_t copy = inodes[idx].size < max ? inodes[idx].size : max;
    if (copy > 0 && inodes[idx].data)
        memcpy(buf, inodes[idx].data, copy);
    *out = copy;
    return 0;
}

static int tmpfs_write(void *priv, const char *path, const void *buf, uint32_t size) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_FILE)
        return -1;

    /* ── Size-limit enforcement ──────────────────────────────────── */
    uint32_t old_size = inodes[idx].size;
    if (tmpfs_size_limit != TMPFS_SIZE_UNLIMITED && size > old_size) {
        uint64_t delta = (uint64_t)(size - old_size);
        if (tmpfs_used_bytes + delta > tmpfs_size_limit) {
            return -ENOSPC; /* write would exceed per-mount size limit */
        }
    }

    /* Reallocate buffer if needed */
    if (inodes[idx].size < size || !inodes[idx].data) {
        uint8_t *new = kmalloc(size < 128 ? 128 : size);
        if (!new) return -1;
        if (inodes[idx].data) kfree(inodes[idx].data);
        inodes[idx].data = new;
    }
    memcpy(inodes[idx].data, buf, size);
    inodes[idx].size = size;

    /* Update the total used-bytes counter */
    if (size > old_size)
        tmpfs_used_bytes += (uint64_t)(size - old_size);
    else if (size < old_size)
        tmpfs_used_bytes -= (uint64_t)(old_size - size);

    return 0;
}

static int tmpfs_mkdir(const char *path) {
    if (find_inode(path) >= 0) return -1; /* exists */
    /* parent must exist */
    /* extract parent dir and basename */
    char dir[TMPFS_MAX_NAME*2], name[TMPFS_MAX_NAME];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1; /* only root */
    int dirlen = (int)(slash - path);
    if (dirlen > (int)sizeof(dir)-1) return -1;
    memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
    if (dirlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    int parent = find_inode(dir);
    if (parent < 0) return -1;
    if (inodes[parent].type != TMPFS_TYPE_DIR) return -1;

    int len = (int)strlen(slash + 1);
    if (len > TMPFS_MAX_NAME - 1) return -1;
    memcpy(name, slash + 1, (size_t)len + 1);
    if (find_inode_in_dir(parent, name) >= 0) return -1;

    int idx = alloc_inode();
    if (idx < 0) return -1;
    inodes[idx].type = TMPFS_TYPE_DIR;
    inodes[idx].parent = (uint32_t)parent;
    memcpy(inodes[idx].name, name, (size_t)len + 1);
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    inodes[idx].uid = 0; inodes[idx].gid = 0;
    inodes[idx].mode = FS_MODE_DIR;
    return 0;
}

static int tmpfs_create(void *priv, const char *path, uint8_t type) {
    (void)priv;
    if (find_inode(path) >= 0) return -1;
    if (type == FS_TYPE_DIR) return tmpfs_mkdir(path);
    if (type != FS_TYPE_FILE && type != FS_TYPE_LINK) return -1;

    char dir[TMPFS_MAX_NAME*2], name[TMPFS_MAX_NAME];
    const char *slash = strrchr(path, '/');
    if (!slash) return -1;
    int dirlen = (int)(slash - path);
    if (dirlen > (int)sizeof(dir)-1) return -1;
    memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
    if (dirlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    int parent = find_inode(dir);
    if (parent < 0) return -1;

    int len = (int)strlen(slash + 1);
    if (len > TMPFS_MAX_NAME - 1) return -1;
    memcpy(name, slash + 1, (size_t)len + 1);

    int idx = alloc_inode();
    if (idx < 0) return -1;
    inodes[idx].type = (type == FS_TYPE_LINK) ? TMPFS_TYPE_LINK : TMPFS_TYPE_FILE;
    inodes[idx].parent = (uint32_t)parent;
    memcpy(inodes[idx].name, name, (size_t)len + 1);
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    inodes[idx].uid = 0; inodes[idx].gid = 0;
    inodes[idx].mode = (type == FS_TYPE_LINK) ? 0777 : FS_MODE_FILE;
    memset(&inodes[idx].dev, 0, sizeof(inodes[idx].dev));
    return 0;
}

static int tmpfs_unlink(void *priv, const char *path) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0) return -1;
    free_inode(idx);
    return 0;
}

static int tmpfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0) return -1;
    st->size = inodes[idx].size;
    switch (inodes[idx].type) {
        case TMPFS_TYPE_DIR:  st->type = VFS_TYPE_DIR;  break;
        case TMPFS_TYPE_LINK: st->type = VFS_TYPE_LINK; break;
        case TMPFS_TYPE_CHR:  st->type = VFS_TYPE_CHR;  break;
        case TMPFS_TYPE_BLK:  st->type = VFS_TYPE_BLK;  break;
        default:              st->type = VFS_TYPE_FILE;  break;
    }
    st->uid = inodes[idx].uid;
    st->gid = inodes[idx].gid;
    st->mode = inodes[idx].mode;
    st->dev_major = inodes[idx].dev.major;
    st->dev_minor = inodes[idx].dev.minor;
    st->ino = (uint32_t)idx;
    st->nlink = 1;
    return 0;
}

static int tmpfs_readdir(void *priv, const char *path) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_DIR) return -1;
    kprintf("tmpfs: %s\n", path);
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) continue;
        if (inodes[i].parent != (uint32_t)idx) continue;
        const char *t;
        switch (inodes[i].type) {
            case TMPFS_TYPE_DIR:  t = "D"; break;
            case TMPFS_TYPE_LINK: t = "L"; break;
            case TMPFS_TYPE_CHR:  t = "C"; break;
            case TMPFS_TYPE_BLK:  t = "B"; break;
            default:              t = "F"; break;
        }
        if (inodes[i].type == TMPFS_TYPE_CHR || inodes[i].type == TMPFS_TYPE_BLK) {
            kprintf("  [%s] %s (%d,%d)\n", t, inodes[i].name,
                    inodes[i].dev.major, inodes[i].dev.minor);
        } else {
            kprintf("  [%s] %s (%u bytes)\n", t, inodes[i].name, inodes[i].size);
        }
    }
    return 0;
}

static int tmpfs_readdir_names(void *priv, const char *path, char names[][64], int max) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_DIR) return 0;
    int count = 0;
    for (int i = 0; i < TMPFS_MAX_INODES && count < max; i++) {
        if (!inodes[i].in_use) continue;
        if (inodes[i].parent != (uint32_t)idx) continue;
        int len = (int)strlen(inodes[i].name);
        int copylen = len < 63 ? len : 63;
        memcpy(names[count], inodes[i].name, (size_t)copylen);
        names[count][copylen] = '\0';
        count++;
    }
    return count;
}

static int tmpfs_truncate(void *priv, const char *path, uint32_t len) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0) return -1;

    uint32_t old_size = inodes[idx].size;

    if (len == 0 && inodes[idx].data) {
        kfree(inodes[idx].data);
        inodes[idx].data = NULL;
        inodes[idx].size = 0;
    } else if (len < inodes[idx].size) {
        inodes[idx].size = len;
    }

    /* Update used-bytes counter for shrinkage */
    if (inodes[idx].size < old_size) {
        uint64_t freed = (uint64_t)(old_size - inodes[idx].size);
        if (tmpfs_used_bytes >= freed)
            tmpfs_used_bytes -= freed;
        else
            tmpfs_used_bytes = 0;
    }

    return 0;
}

/* ── Symlink support ──────────────────────────────────────────── */

static int tmpfs_symlink(void *priv, const char *target, const char *linkpath) {
    (void)priv;
    /* Create the link inode via the generic create helper */
    if (tmpfs_create(priv, linkpath, FS_TYPE_LINK) < 0)
        return -1;

    int idx = find_inode(linkpath);
    if (idx < 0) return -1;

    /* Store the target path in the inode's data buffer */
    size_t target_len = strlen(target);
    if (target_len == 0) {
        inodes[idx].data = NULL;
        inodes[idx].size = 0;
        return 0;
    }

    inodes[idx].data = kmalloc(target_len + 1);
    if (!inodes[idx].data) {
        tmpfs_unlink(priv, linkpath);
        return -1;
    }
    memcpy(inodes[idx].data, target, target_len + 1);
    inodes[idx].size = (uint32_t)target_len;
    return 0;
}

static int tmpfs_readlink(void *priv, const char *path, char *buf, int bufsize) {
    (void)priv;
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_LINK)
        return -1;
    if (bufsize <= 0) return -1;

    if (!inodes[idx].data || inodes[idx].size == 0) {
        buf[0] = '\0';
        return 0;
    }

    int copy_len = (int)inodes[idx].size;
    if (copy_len >= bufsize)
        copy_len = bufsize - 1;
    memcpy(buf, inodes[idx].data, (size_t)copy_len);
    buf[copy_len] = '\0';
    return copy_len;
}

/* ── mknod support ────────────────────────────────────────────── */

static int tmpfs_mknod(void *priv, const char *path, uint16_t mode, uint16_t dev_major, uint16_t dev_minor) {
    (void)priv;
    if (find_inode(path) >= 0) return -1; /* already exists */

    char dir[TMPFS_MAX_NAME*2], name[TMPFS_MAX_NAME];
    const char *slash = strrchr(path, '/');
    if (!slash) return -1;
    int dirlen = (int)(slash - path);
    if (dirlen > (int)sizeof(dir)-1) return -1;
    memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
    if (dirlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    int parent = find_inode(dir);
    if (parent < 0 || inodes[parent].type != TMPFS_TYPE_DIR)
        return -1;

    int len = (int)strlen(slash + 1);
    if (len > TMPFS_MAX_NAME - 1) return -1;
    memcpy(name, slash + 1, (size_t)len + 1);
    if (find_inode_in_dir(parent, name) >= 0) return -1;

    int idx = alloc_inode();
    if (idx < 0) return -1;
    /* Determine device node type from mode bits */
    if (mode & S_IFCHR)
        inodes[idx].type = TMPFS_TYPE_CHR;
    else if (mode & S_IFBLK)
        inodes[idx].type = TMPFS_TYPE_BLK;
    else
        inodes[idx].type = TMPFS_TYPE_FILE;
    inodes[idx].parent = (uint32_t)parent;
    memcpy(inodes[idx].name, name, (size_t)len + 1);
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    inodes[idx].uid = 0; inodes[idx].gid = 0;
    inodes[idx].mode = mode;
    inodes[idx].dev.major = dev_major;
    inodes[idx].dev.minor = dev_minor;
    return 0;
}

/*
 * tmpfs_rename — Rename/move a file or directory in tmpfs.
 *
 * Finds the inode by old_path, then updates its name and parent
 * index to match new_path.  This is an O(n) flat-table operation.
 */
static int tmpfs_rename(void *priv, const char *old_path, const char *new_path)
{
    (void)priv;

    /* Find the old inode */
    int old_idx = find_inode(old_path);
    if (old_idx < 0) return -ENOENT;

    /* Ensure the new path doesn't already exist */
    if (find_inode(new_path) >= 0) return -EEXIST;

    /* Extract the new leaf name (last component after '/') */
    const char *new_name = new_path;
    const char *last_slash = strrchr(new_path, '/');
    if (last_slash) new_name = last_slash + 1;

    int name_len = (int)strlen(new_name);
    if (name_len == 0) return -EINVAL;
    if (name_len >= TMPFS_MAX_NAME) return -ENAMETOOLONG;

    /* Determine the new parent directory */
    char parent_path[128];
    int new_parent = old_idx; /* default: keep same parent */

    if (last_slash && last_slash > new_path) {
        /* Extract the directory portion of new_path */
        size_t dir_len = (size_t)(last_slash - new_path);
        if (dir_len >= sizeof(parent_path)) return -ENAMETOOLONG;
        memcpy(parent_path, new_path, dir_len);
        parent_path[dir_len] = '\0';
        new_parent = find_inode(parent_path);
        if (new_parent < 0) return -ENOENT;
    } else if (last_slash == new_path) {
        /* New path is in root directory */
        new_parent = 0;
    }

    /* Update the inode */
    memcpy(inodes[old_idx].name, new_name, (size_t)name_len);
    inodes[old_idx].name[name_len] = '\0';
    inodes[old_idx].parent = (uint32_t)new_parent;

    return 0;
}

struct vfs_ops tmpfs_vfs_ops = {
    .read        = tmpfs_read,
    .write       = tmpfs_write,
    .create      = tmpfs_create,
    .unlink      = tmpfs_unlink,
    .stat        = tmpfs_stat,
    .readdir     = tmpfs_readdir,
    .readdir_names = tmpfs_readdir_names,
    .truncate    = tmpfs_truncate,
    .symlink     = tmpfs_symlink,
    .readlink    = tmpfs_readlink,
    .mknod       = tmpfs_mknod,
    .rename      = tmpfs_rename,
};

int tmpfs_mount(void) {
    if (tmpfs_mounted) return -1;
    /* Clear all inodes */
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        inodes[i].in_use = 0;
        inodes[i].data = NULL;
    }
    /* Reset size accounting (unlimited) */
    tmpfs_size_limit = TMPFS_SIZE_UNLIMITED;
    tmpfs_used_bytes = 0;
    /* Create root directory */
    inodes[0].in_use = 1;
    inodes[0].type = TMPFS_TYPE_DIR;
    inodes[0].name[0] = '\0';
    inodes[0].parent = 0;
    inodes[0].size = 0;
    inodes[0].data = NULL;
    inodes[0].uid = 0; inodes[0].gid = 0;
    inodes[0].mode = 0755;
    tmpfs_mounted = 1;
    return 0;
}

int tmpfs_mount_with_limit(uint64_t max_bytes) {
    int ret = tmpfs_mount();
    if (ret == 0 && max_bytes > 0)
        tmpfs_size_limit = max_bytes;
    return ret;
}

int tmpfs_unmount(void) {
    if (!tmpfs_mounted) return -1;
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (inodes[i].in_use)
            free_inode(i);
    }
    tmpfs_mounted = 0;
    return 0;
}

void tmpfs_init(void) {
    tmpfs_mount();
    kprintf("[OK] tmpfs initialized\n");
}
