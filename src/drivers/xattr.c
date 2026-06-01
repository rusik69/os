#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"
#include "xattr.h"
#include "spinlock.h"

#define MAX_XATTR 8
static struct xattr_entry system_xattrs[MAX_XATTR];
static spinlock_t xattr_lock;

void xattr_init(void) {
    spinlock_init(&xattr_lock);
    memset(system_xattrs, 0, sizeof(system_xattrs));
}

int xattr_set(const char *path, const char *name, const void *value, size_t size) {
    if (!path || !name || !value || size > VFS_XATTR_VALUE_MAX) return -1;
    spinlock_acquire(&xattr_lock);
    int slot = -1;
    for (int i = 0; i < MAX_XATTR; i++) {
        if (system_xattrs[i].in_use && strcmp(system_xattrs[i].name, name) == 0) { slot = i; break; }
        if (!system_xattrs[i].in_use && slot < 0) slot = i;
    }
    if (slot < 0) { spinlock_release(&xattr_lock); return -1; }
    system_xattrs[slot].in_use = 1;
    strncpy(system_xattrs[slot].name, name, VFS_XATTR_NAME_MAX - 1);
    memcpy(system_xattrs[slot].value, value, size);
    system_xattrs[slot].size = (int)size;
    spinlock_release(&xattr_lock);
    return 0;
}

int xattr_get(const char *path, const char *name, void *value, size_t *size) {
    if (!path || !name || !size) return -1;
    spinlock_acquire(&xattr_lock);
    for (int i = 0; i < MAX_XATTR; i++) {
        if (system_xattrs[i].in_use && strcmp(system_xattrs[i].name, name) == 0) {
            size_t copy = *size < (size_t)system_xattrs[i].size ? *size : (size_t)system_xattrs[i].size;
            memcpy(value, system_xattrs[i].value, copy);
            *size = (size_t)system_xattrs[i].size;
            spinlock_release(&xattr_lock);
            return 0;
        }
    }
    spinlock_release(&xattr_lock);
    return -1;
}
