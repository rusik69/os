#define KERNEL_INTERNAL
#include "xattr.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "heap.h"

/* Extended attributes stored in a simple global table (path-indexed) */
#define XATTR_MAX_FILES 64

struct xattr_file {
    char path[128];
    struct xattr_entry entries[XATTR_MAX_ENTRIES];
    int count;
    int in_use;
};

static struct xattr_file xattr_table[XATTR_MAX_FILES];
static int xattr_initialized = 0;

void xattr_init(void) {
    if (xattr_initialized) return;
    memset(xattr_table, 0, sizeof(xattr_table));
    xattr_initialized = 1;
    kprintf("[OK] xattr initialized\n");
}

static struct xattr_file *xattr_find_file(const char *path) {
    for (int i = 0; i < XATTR_MAX_FILES; i++) {
        if (xattr_table[i].in_use && strcmp(xattr_table[i].path, path) == 0)
            return &xattr_table[i];
    }
    return NULL;
}

static struct xattr_file *xattr_get_or_create(const char *path) {
    struct xattr_file *xf = xattr_find_file(path);
    if (xf) return xf;
    for (int i = 0; i < XATTR_MAX_FILES; i++) {
        if (!xattr_table[i].in_use) {
            strncpy(xattr_table[i].path, path, sizeof(xattr_table[i].path) - 1);
            xattr_table[i].path[sizeof(xattr_table[i].path) - 1] = '\0';
            xattr_table[i].count = 0;
            memset(xattr_table[i].entries, 0, sizeof(xattr_table[i].entries));
            xattr_table[i].in_use = 1;
            return &xattr_table[i];
        }
    }
    return NULL;
}

int xattr_set(const char *path, const char *name, const void *value, size_t size) {
    if (!path || !name || !value) return -EINVAL;
    struct xattr_file *xf = xattr_get_or_create(path);
    if (!xf) return -ENOMEM;

    /* Check if already exists — update */
    for (int i = 0; i < XATTR_MAX_ENTRIES; i++) {
        if (xf->entries[i].in_use && strcmp(xf->entries[i].name, name) == 0) {
            size_t copy_sz = size < XATTR_VALUE_MAX ? size : XATTR_VALUE_MAX;
            memcpy(xf->entries[i].value, value, copy_sz);
            xf->entries[i].value_size = copy_sz;
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < XATTR_MAX_ENTRIES; i++) {
        if (!xf->entries[i].in_use) {
            strncpy(xf->entries[i].name, name, XATTR_NAME_MAX - 1);
            xf->entries[i].name[XATTR_NAME_MAX - 1] = '\0';
            size_t copy_sz = size < XATTR_VALUE_MAX ? size : XATTR_VALUE_MAX;
            memcpy(xf->entries[i].value, value, copy_sz);
            xf->entries[i].value_size = copy_sz;
            xf->entries[i].in_use = 1;
            xf->count++;
            return 0;
        }
    }
    return -ENOSPC;
}

int xattr_get(const char *path, const char *name, void *value, size_t size) {
    if (!path || !name || !value) return -EINVAL;
    struct xattr_file *xf = xattr_find_file(path);
    if (!xf) return -ENOENT;

    for (int i = 0; i < XATTR_MAX_ENTRIES; i++) {
        if (xf->entries[i].in_use && strcmp(xf->entries[i].name, name) == 0) {
            size_t copy_sz = size < xf->entries[i].value_size ? size : xf->entries[i].value_size;
            memcpy(value, xf->entries[i].value, copy_sz);
            return (int)copy_sz;
        }
    }
    return -ENOENT;
}

int xattr_list(const char *path, char *buf, size_t size) {
    if (!path || !buf) return -EINVAL;
    struct xattr_file *xf = xattr_find_file(path);
    if (!xf) return -ENOENT;

    size_t written = 0;
    for (int i = 0; i < XATTR_MAX_ENTRIES; i++) {
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

int xattr_remove(const char *path, const char *name) {
    if (!path || !name) return -EINVAL;
    struct xattr_file *xf = xattr_find_file(path);
    if (!xf) return -ENOENT;

    for (int i = 0; i < XATTR_MAX_ENTRIES; i++) {
        if (xf->entries[i].in_use && strcmp(xf->entries[i].name, name) == 0) {
            memset(&xf->entries[i], 0, sizeof(struct xattr_entry));
            xf->count--;
            return 0;
        }
    }
    return -ENOENT;
}

/* VFS-level wrappers */
int vfs_setxattr(const char *path, const char *name, const void *value, int size) {
    return xattr_set(path, name, value, (size_t)size);
}

int vfs_getxattr(const char *path, const char *name, void *value, int size) {
    return xattr_get(path, name, value, (size_t)size);
}

int vfs_listxattr(const char *path, char *buf, int size) {
    return xattr_list(path, buf, (size_t)size);
}
