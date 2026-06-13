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
