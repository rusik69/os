/*
 * src/fs/xattr.c — Extended attributes with full namespace ops (S149)
 *
 * Implements:
 *   - listxattr, getxattr, setxattr, removexattr for all namespaces
 *   - system, security, trusted, user namespace prefixes
 *   - Wired to VFS inode operations via vfs_setxattr/vfs_getxattr/...
 *
 * Namespace access controls:
 *   - system.*      : kernel/internal (root only)
 *   - security.*    : security modules (IMA, EVM, IPE)
 *   - trusted.*     : restricted to root
 *   - user.*        : accessible by any process
 */

#define KERNEL_INTERNAL
#include "xattr.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "heap.h"
#include "errno.h"

/* Extended attributes stored in a simple global table (path-indexed) */
#define XATTR_MAX_FILES 64
#define XATTR_MAX_ENTRIES_PER_FILE 8
#define XATTR_BUCKET_SIZE 4
#define XATTR_NUM_BUCKETS (XATTR_MAX_FILES / XATTR_BUCKET_SIZE)

struct xattr_file {
    char path[128];
    struct xattr_entry entries[XATTR_MAX_ENTRIES_PER_FILE];
    int count;
    int in_use;
};

static struct xattr_file xattr_table[XATTR_MAX_FILES];
static int xattr_initialized = 0;

/* Simple string hash for path-based lookup */
static uint32_t xattr_path_hash(const char *path)
{
    uint32_t h = 5381;
    int c;
    while ((c = (unsigned char)*path++))
        h = ((h << 5) + h) + c;
    return h;
}

/* ── Namespace validation (S149) ──────────────────────────────────── */

int xattr_validate_namespace(const char *name)
{
    if (!name || !name[0])
        return -EINVAL;

    /* Check for known namespace prefixes */
    if (strncmp(name, "system.", 7) == 0)
        return 0;
    if (strncmp(name, "security.", 9) == 0)
        return 0;
    if (strncmp(name, "trusted.", 8) == 0)
        return 0;
    if (strncmp(name, "user.", 5) == 0)
        return 0;

    return -EINVAL;  /* Unknown namespace */
}

/* Check if the current process has access to a given namespace */
static int xattr_check_namespace_access(const char *name)
{
    struct process *p = process_get_current();
    int is_root = (p && p->uid == 0);

    if (strncmp(name, "system.", 7) == 0)
        return is_root ? 0 : -EACCES;
    if (strncmp(name, "security.", 9) == 0)
        return is_root ? 0 : -EACCES;
    if (strncmp(name, "trusted.", 8) == 0)
        return is_root ? 0 : -EACCES;
    if (strncmp(name, "user.", 5) == 0)
        return 0;

    return -EINVAL;
}

/* ── Internal helpers ──────────────────────────────────────────────── */

void xattr_init(void) {
    if (xattr_initialized) return;
    memset(xattr_table, 0, sizeof(xattr_table));
    xattr_initialized = 1;
    kprintf("[OK] xattr initialized (system/security/trusted/user namespaces)\n");
}

static struct xattr_file *xattr_find_file(const char *path) {
    uint32_t h = xattr_path_hash(path);
    uint32_t base = (h % XATTR_NUM_BUCKETS) * XATTR_BUCKET_SIZE;
    for (uint32_t i = 0; i < XATTR_BUCKET_SIZE; i++) {
        uint32_t idx = base + i;
        if (idx >= XATTR_MAX_FILES) break;
        if (xattr_table[idx].in_use && strcmp(xattr_table[idx].path, path) == 0)
            return &xattr_table[idx];
    }
    return NULL;
}

static struct xattr_file *xattr_get_or_create(const char *path) {
    struct xattr_file *xf = xattr_find_file(path);
    if (xf) return xf;

    uint32_t h = xattr_path_hash(path);
    uint32_t base = (h % XATTR_NUM_BUCKETS) * XATTR_BUCKET_SIZE;
    for (uint32_t i = 0; i < XATTR_BUCKET_SIZE; i++) {
        uint32_t idx = base + i;
        if (idx >= XATTR_MAX_FILES) break;
        if (!xattr_table[idx].in_use) {
            strncpy(xattr_table[idx].path, path, sizeof(xattr_table[idx].path) - 1);
            xattr_table[idx].path[sizeof(xattr_table[idx].path) - 1] = '\0';
            xattr_table[idx].count = 0;
            memset(xattr_table[idx].entries, 0, sizeof(xattr_table[idx].entries));
            xattr_table[idx].in_use = 1;
            return &xattr_table[idx];
        }
    }
    return NULL; /* Bucket full — no space */
}

/* ── setxattr (S149) ──────────────────────────────────────────────── */

int xattr_set(const char *path, const char *name, const void *value, size_t size) {
    if (!path || !name || !value) return -EINVAL;

    int ns_ret = xattr_validate_namespace(name);
    if (ns_ret < 0)
        return -EINVAL;

    ns_ret = xattr_check_namespace_access(name);
    if (ns_ret < 0)
        return ns_ret;

    struct xattr_file *xf = xattr_get_or_create(path);
    if (!xf) return -ENOMEM;

    /* Check if already exists — update */
    for (int i = 0; i < XATTR_MAX_ENTRIES_PER_FILE; i++) {
        if (xf->entries[i].in_use && strcmp(xf->entries[i].name, name) == 0) {
            size_t copy_sz = size < VFS_XATTR_VALUE_MAX ? size : VFS_XATTR_VALUE_MAX;
            memcpy(xf->entries[i].value, value, copy_sz);
            xf->entries[i].size = (int)copy_sz;
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < XATTR_MAX_ENTRIES_PER_FILE; i++) {
        if (!xf->entries[i].in_use) {
            strncpy(xf->entries[i].name, name, VFS_XATTR_NAME_MAX - 1);
            xf->entries[i].name[VFS_XATTR_NAME_MAX - 1] = '\0';
            size_t copy_sz = size < VFS_XATTR_VALUE_MAX ? size : VFS_XATTR_VALUE_MAX;
            memcpy(xf->entries[i].value, value, copy_sz);
            xf->entries[i].size = (int)copy_sz;
            xf->entries[i].in_use = 1;
            xf->count++;
            return 0;
        }
    }
    return -ENOSPC;
}

/* ── getxattr (S149) ──────────────────────────────────────────────── */

int xattr_get(const char *path, const char *name, void *value, size_t size) {
    if (!path || !name || !value) return -EINVAL;

    int ns_ret = xattr_validate_namespace(name);
    if (ns_ret < 0)
        return -EINVAL;

    ns_ret = xattr_check_namespace_access(name);
    if (ns_ret < 0)
        return ns_ret;

    struct xattr_file *xf = xattr_find_file(path);
    if (!xf) return -ENOENT;

    for (int i = 0; i < XATTR_MAX_ENTRIES_PER_FILE; i++) {
        if (xf->entries[i].in_use && strcmp(xf->entries[i].name, name) == 0) {
            size_t copy_sz = size < (size_t)xf->entries[i].size
                             ? size : (size_t)xf->entries[i].size;
            memcpy(value, xf->entries[i].value, copy_sz);
            return (int)copy_sz;
        }
    }
    return -ENOENT;
}

/* ── listxattr (S149) ─────────────────────────────────────────────── */

int xattr_list(const char *path, char *buf, size_t size) {
    if (!path || !buf) return -EINVAL;

    struct xattr_file *xf = xattr_find_file(path);
    if (!xf) return 0;  /* No xattrs is not an error */

    size_t written = 0;
    for (int i = 0; i < XATTR_MAX_ENTRIES_PER_FILE; i++) {
        if (xf->entries[i].in_use) {
            size_t name_len = strlen(xf->entries[i].name) + 1;
            if (written + name_len <= size) {
                memcpy(buf + written, xf->entries[i].name, name_len);
                written += name_len;
            } else {
                return -ERANGE;
            }
        }
    }
    return (int)written;
}

/* ── removexattr (S149) ───────────────────────────────────────────── */

int xattr_remove(const char *path, const char *name) {
    if (!path || !name) return -EINVAL;

    int ns_ret = xattr_validate_namespace(name);
    if (ns_ret < 0)
        return -EINVAL;

    ns_ret = xattr_check_namespace_access(name);
    if (ns_ret < 0)
        return ns_ret;

    struct xattr_file *xf = xattr_find_file(path);
    if (!xf) return -ENOENT;

    for (int i = 0; i < XATTR_MAX_ENTRIES_PER_FILE; i++) {
        if (xf->entries[i].in_use && strcmp(xf->entries[i].name, name) == 0) {
            memset(&xf->entries[i], 0, sizeof(struct xattr_entry));
            xf->count--;
            return 0;
        }
    }
    return -ENOENT;
}

/* ── VFS-level wrappers (S149) ─────────────────────────────────────── */

int vfs_setxattr(const char *path, const char *name, const void *value, int size) {
    return xattr_set(path, name, value, (size_t)size);
}

int vfs_getxattr(const char *path, const char *name, void *value, int size) {
    return xattr_get(path, name, value, (size_t)size);
}

int vfs_listxattr(const char *path, char *buf, int size) {
    return xattr_list(path, buf, (size_t)size);
}

int vfs_removexattr(const char *path, const char *name) {
    return xattr_remove(path, name);
}
#include "module.h"
module_init(xattr_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── xattr_generic_set ─────────────────────────────────── */
int xattr_generic_set(void *inode, const char *name, const void *value, size_t size, int flags)
{
    (void)inode;
    (void)flags;
    if (!name || !value) return -EINVAL;
    kprintf("[xattr] set: %s size=%zu\n", name, size);
    return 0;
}
/* ── xattr_generic_get ─────────────────────────────────── */
int xattr_generic_get(void *inode, const char *name, void *value, size_t size)
{
    (void)inode;
    (void)name;
    (void)value;
    (void)size;
    kprintf("[xattr] get: %s\n", name);
    return -ENODATA;
}
