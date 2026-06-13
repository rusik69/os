/* uaccess.c — safe user-space memory access routines
 *
 * These functions provide the kernel with a safe way to read/write
 * user-space memory, respecting SMAP (Supervisor Mode Access Prevention).
 *
 * Each function:
 *   1. If called from a user process, validates the target user address
 *      range via vmm_user_range_ok() (or vmm_user_string_ok())
 *   2. Temporarily disables SMAP via STAC
 *   3. Performs the memory access
 *   4. Re-enables SMAP via CLAC
 *
 * If called from a kernel-mode context (e.g. the kernel-mode shell),
 * validation is skipped and the copy is performed directly — the
 * "user" pointer is actually a kernel pointer in that case.
 */

#include "uaccess.h"
#include "cpu.h"
#include "process.h"
#include "string.h"
#include "vmm.h"
#include "errno.h"

/* Helper: returns 1 if the current process is a userspace process */
static inline int current_is_user(void) {
    struct process *p = process_get_current();
    return p && p->is_user;
}

int copy_from_user(void *dst, uint64_t src_user, size_t n) {
    if (n == 0) return 0;
    if (current_is_user()) {
        struct process *p = process_get_current();
        if (!p || !p->pml4 || !vmm_user_range_ok(p->pml4, src_user, n, 0))
            return -EFAULT;
        stac();
        memcpy(dst, (const void *)(uintptr_t)src_user, n);
        clac();
    } else {
        /* Kernel-mode caller (e.g. shell) — pointer is already kernel-mapped */
        memcpy(dst, (const void *)(uintptr_t)src_user, n);
    }
    return 0;
}

int copy_to_user(uint64_t dst_user, const void *src, size_t n) {
    if (n == 0) return 0;
    if (current_is_user()) {
        struct process *p = process_get_current();
        if (!p || !p->pml4 || !vmm_user_range_ok(p->pml4, dst_user, n, 1))
            return -EFAULT;
        stac();
        memcpy((void *)(uintptr_t)dst_user, src, n);
        clac();
    } else {
        /* Kernel-mode caller — pointer is already kernel-mapped */
        memcpy((void *)(uintptr_t)dst_user, src, n);
    }
    return 0;
}

long strncpy_from_user(char *dst, uint64_t src_user, size_t max_len) {
    size_t i;

    if (max_len == 0) return -EFAULT;
    if (current_is_user()) {
        struct process *p = process_get_current();
        if (!p || !p->pml4 || !vmm_user_string_ok(p->pml4, src_user, max_len))
            return -EFAULT;
        stac();
        for (i = 0; i < max_len - 1; i++) {
            char c = ((volatile const char *)(uintptr_t)src_user)[i];
            dst[i] = c;
            if (c == '\0') break;
        }
        dst[i] = '\0';
        clac();
    } else {
        /* Kernel-mode caller */
        for (i = 0; i < max_len - 1; i++) {
            char c = ((const char *)(uintptr_t)src_user)[i];
            dst[i] = c;
            if (c == '\0') break;
        }
        dst[i] = '\0';
    }
    return (long)(i + 1);
}

long strlen_user(uint64_t src_user, size_t max_len) {
    size_t i;

    if (max_len == 0) return -EFAULT;
    if (current_is_user()) {
        struct process *p = process_get_current();
        if (!p || !p->pml4 || !vmm_user_string_ok(p->pml4, src_user, max_len))
            return -EFAULT;
        stac();
        for (i = 0; i < max_len; i++) {
            if (((volatile const char *)(uintptr_t)src_user)[i] == '\0') {
                clac();
                return (long)(i + 1);
            }
        }
        clac();
    } else {
        for (i = 0; i < max_len; i++) {
            if (((const char *)(uintptr_t)src_user)[i] == '\0')
                return (long)(i + 1);
        }
    }
    return (long)max_len;
}

int get_user_byte(uint64_t addr, uint8_t *out) {
    if (current_is_user()) {
        struct process *p = process_get_current();
        if (!p || !p->pml4 || !vmm_user_range_ok(p->pml4, addr, 1, 0))
            return -EFAULT;
        stac();
        *out = *(volatile uint8_t *)(uintptr_t)addr;
        clac();
    } else {
        *out = *(const uint8_t *)(uintptr_t)addr;
    }
    return 0;
}

int put_user_byte(uint64_t addr, uint8_t val) {
    if (current_is_user()) {
        struct process *p = process_get_current();
        if (!p || !p->pml4 || !vmm_user_range_ok(p->pml4, addr, 1, 1))
            return -EFAULT;
        stac();
        *(volatile uint8_t *)(uintptr_t)addr = val;
        clac();
    } else {
        *(uint8_t *)(uintptr_t)addr = val;
    }
    return 0;
}

int memset_user(uint64_t dst_user, uint8_t val, size_t n) {
    if (n == 0) return 0;
    if (current_is_user()) {
        struct process *p = process_get_current();
        if (!p || !p->pml4 || !vmm_user_range_ok(p->pml4, dst_user, n, 1))
            return -EFAULT;
        stac();
        memset((void *)(uintptr_t)dst_user, val, n);
        clac();
    } else {
        memset((void *)(uintptr_t)dst_user, val, n);
    }
    return 0;
}
