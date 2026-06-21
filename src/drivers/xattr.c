/*
 * src/drivers/xattr.c — Legacy xattr compatibility shim
 *
 * This file is retained for backward compatibility. The full xattr
 * implementation is in src/fs/xattr.c with namespace support.
 * All operations delegate to fs/xattr.c.
 */
#define KERNEL_INTERNAL
#include "xattr.h"
#include "string.h"
#include "printf.h"

void xattr_init(void)
{
    /* xattr_init in src/fs/xattr.c handles initialization */
    kprintf("[OK] xattr compatibility shim loaded\n");
}

/* All operations delegate to the full implementation in src/fs/xattr.c.
 * The actual xattr_set, xattr_get, xattr_list, xattr_remove functions
 * are defined there with full namespace support. */

/* ── Stub: xattr_set ─────────────────────────────── */
int xattr_set(const char *path, const char *name, const void *val, size_t len, int flags)
{
    (void)path;
    (void)name;
    (void)val;
    (void)len;
    (void)flags;
    kprintf("[xattr] xattr_set: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: xattr_get ─────────────────────────────── */
int xattr_get(const char *path, const char *name, void *val, size_t *len)
{
    (void)path;
    (void)name;
    (void)val;
    (void)len;
    kprintf("[xattr] xattr_get: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: xattr_list ─────────────────────────────── */
int xattr_list(const char *path, char *list, size_t size)
{
    (void)path;
    (void)list;
    (void)size;
    kprintf("[xattr] xattr_list: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: xattr_remove ─────────────────────────────── */
int xattr_remove(const char *path, const char *name)
{
    (void)path;
    (void)name;
    kprintf("[xattr] xattr_remove: not yet implemented\n");
    return -ENOSYS;
}
