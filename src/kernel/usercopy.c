// SPDX-License-Identifier: GPL-2.0-only
/*
 * usercopy.c — Hardened copy_from_user/copy_to_user bounds checking
 *
 * Implements:
 * - Size limit: blocks transfers > 4096 bytes
 * - Struct whitelisting: provide arrays of safe struct sizes
 * - Bounds validation against user address ranges
 */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "vmm.h"
#include "errno.h"

#define USERCOPY_MAX_SIZE 4096
#define USERCOPY_WHITELIST_MAX 32

/* Whitelist of allowed struct sizes for user copies */
static size_t usercopy_whitelist[USERCOPY_WHITELIST_MAX];
static int usercopy_whitelist_count;

/* Register a struct size as whitelisted for user copies */
int usercopy_whitelist_add(size_t struct_size)
{
    if (usercopy_whitelist_count >= USERCOPY_WHITELIST_MAX)
        return -ENOMEM;

    /* Check for duplicates */
    for (int i = 0; i < usercopy_whitelist_count; i++) {
        if (usercopy_whitelist[i] == struct_size)
            return 0;
    }

    usercopy_whitelist[usercopy_whitelist_count++] = struct_size;
    return 0;
}

/* Check if a size is whitelisted */
static int usercopy_size_whitelisted(size_t size)
{
    for (int i = 0; i < usercopy_whitelist_count; i++) {
        if (usercopy_whitelist[i] == size)
            return 1;
    }
    return 0;
}

/* Validate user address range */
static int usercopy_validate_range(uintptr_t addr, size_t size, int is_user)
{
    /* Must be user address if user copy */
    if (is_user) {
        if (addr >= 0xFFFF800000000000ULL)
            return -EFAULT;
        if (addr + size < addr) /* overflow */
            return -EFAULT;
        if (addr + size > 0x00007FFFFFFFFFFFULL)
            return -EFAULT;
    }

    return 0;
}

/* Hardened copy_from_user with bounds checking */
int hardened_copy_from_user(void *dst, uintptr_t src, size_t size)
{
    /* Size limit enforcement */
    if (size > USERCOPY_MAX_SIZE) {
        kprintf("[USERCOPY] copy_from_user size too large: %zu > %d\n",
                size, USERCOPY_MAX_SIZE);
        return -EINVAL;
    }

    /* Validate source range */
    int ret = usercopy_validate_range(src, size, 1);
    if (ret < 0) {
        kprintf("[USERCOPY] copy_from_user invalid src range: 0x%llx + %zu\n",
                (unsigned long long)src, size);
        return ret;
    }

    /* For non-whitelisted sizes, print warning but still allow */
    if (!usercopy_size_whitelisted(size) && size > 0) {
        /* Only warn once per boot */
        static int warned;
        if (!warned) {
            kprintf("[USERCOPY] copy_from_user size %zu not whitelisted\n", size);
            warned = 1;
        }
    }

    /* Perform the copy */
    memcpy(dst, (const void *)src, size);
    return 0;
}

/* Hardened copy_to_user with bounds checking */
int hardened_copy_to_user(uintptr_t dst, const void *src, size_t size)
{
    /* Size limit enforcement */
    if (size > USERCOPY_MAX_SIZE) {
        kprintf("[USERCOPY] copy_to_user size too large: %zu > %d\n",
                size, USERCOPY_MAX_SIZE);
        return -EINVAL;
    }

    /* Validate destination range */
    int ret = usercopy_validate_range(dst, size, 1);
    if (ret < 0) {
        kprintf("[USERCOPY] copy_to_user invalid dst range: 0x%llx + %zu\n",
                (unsigned long long)dst, size);
        return ret;
    }

    /* Perform the copy */
    memcpy((void *)dst, src, size);
    return 0;
}

/* Init */
void usercopy_init(void)
{
    /* Pre-register common struct sizes */
    usercopy_whitelist_add(sizeof(uint64_t));
    usercopy_whitelist_add(16); /* struct inode, etc. */
    usercopy_whitelist_add(32);
    usercopy_whitelist_add(64);
    usercopy_whitelist_add(128);
    usercopy_whitelist_add(256);
    usercopy_whitelist_add(512);
    usercopy_whitelist_add(1024);

    kprintf("[OK] Usercopy hardened — max size %d, %d whitelisted sizes\n",
            USERCOPY_MAX_SIZE, usercopy_whitelist_count);
}
