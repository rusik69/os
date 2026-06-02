#define KERNEL_INTERNAL
#include "types.h"
#include "tmpfs.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "fs.h" /* for FS_MODE_FILE, FS_MODE_DIR etc */

static struct tmpfs_inode inodes[TMPFS_MAX_INODES];
static int tmpfs_mounted = 0;

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

static int tmpfs_read(const char *path, void *buf, uint32_t max, uint32_t *out) {
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_FILE)
        return -1;
    uint32_t copy = inodes[idx].size < max ? inodes[idx].size : max;
    if (copy > 0 && inodes[idx].data)
        memcpy(buf, inodes[idx].data, copy);
    *out = copy;
    return 0;
}

static int tmpfs_write(const char *path, const void *buf, uint32_t size) {
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_FILE)
        return -1;
    /* Reallocate buffer if needed */
    if (inodes[idx].size < size || !inodes[idx].data) {
        uint8_t *new = kmalloc(size < 128 ? 128 : size);
        if (!new) return -1;
        if (inodes[idx].data) kfree(inodes[idx].data);
        inodes[idx].data = new;
    }
    memcpy(inodes[idx].data, buf, size);
    inodes[idx].size = size;
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

static int tmpfs_create(const char *path, uint8_t type) {
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

static int tmpfs_unlink(const char *path) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    free_inode(idx);
    return 0;
}

static int tmpfs_stat(const char *path, struct vfs_stat *st) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    st->size = inodes[idx].size;
    st->type = (inodes[idx].type == TMPFS_TYPE_DIR) ? 2 : 1;
    st->uid = inodes[idx].uid;
    st->gid = inodes[idx].gid;
    st->mode = inodes[idx].mode;
    return 0;
}

static int tmpfs_readdir(const char *path) {
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != TMPFS_TYPE_DIR) return -1;
    kprintf("tmpfs: %s\n", path);
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        if (!inodes[i].in_use) continue;
        if (inodes[i].parent != (uint32_t)idx) continue;
        const char *t = (inodes[i].type == TMPFS_TYPE_DIR) ? "D" : "F";
        kprintf("  [%s] %s (%u bytes)\n", t, inodes[i].name, inodes[i].size);
    }
    return 0;
}

static int tmpfs_readdir_names(const char *path, char names[][64], int max) {
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

static int tmpfs_truncate(const char *path, uint32_t len) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (len == 0 && inodes[idx].data) {
        kfree(inodes[idx].data);
        inodes[idx].data = NULL;
        inodes[idx].size = 0;
    } else if (len < inodes[idx].size) {
        inodes[idx].size = len;
    }
    return 0;
}

/* ── Symlink support ──────────────────────────────────────────── */

static int tmpfs_symlink(const char *target, const char *linkpath) {
    /* Create the link inode via the generic create helper */
    if (tmpfs_create(linkpath, FS_TYPE_LINK) < 0)
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
        tmpfs_unlink(linkpath);
        return -1;
    }
    memcpy(inodes[idx].data, target, target_len + 1);
    inodes[idx].size = (uint32_t)target_len;
    return 0;
}

static int tmpfs_readlink(const char *path, char *buf, int bufsize) {
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

static int tmpfs_mknod(const char *path, uint16_t mode, uint16_t dev_major, uint16_t dev_minor) {
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
    inodes[idx].type = TMPFS_TYPE_FILE;  /* non-file, non-dir: treated as special */
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
};

int tmpfs_mount(void) {
    if (tmpfs_mounted) return -1;
    /* Clear all inodes */
    for (int i = 0; i < TMPFS_MAX_INODES; i++) {
        inodes[i].in_use = 0;
        inodes[i].data = NULL;
    }
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
