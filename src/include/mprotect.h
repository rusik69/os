#ifndef MPROTECT_H
#define MPROTECT_H

#include "types.h"

/*
 * mprotect — Change memory protection of a mapped region
 *
 * Implements the POSIX mprotect(2) syscall for user-space memory.
 * Changes the page-level permission flags for the virtual address
 * range [addr, addr+len) for the calling process.
 *
 * The implementation:
 *   1. Validates the address range (page-aligned, within user space)
 *   2. Checks W^X enforcement (wx_enforce_check_prot)
 *   3. Checks mseal status (sealed ranges are immutable)
 *   4. Walks the process page tables and updates PTE permissions
 *      via vmm_set_user_pages_flags()
 *   5. Flushes TLB entries for the modified range
 */

/**
 * sys_mprotect - Change memory protection of a mapped region.
 *
 * @addr:   Start address (must be page-aligned)
 * @len:    Length in bytes (will be rounded up to page boundary)
 * @prot:   New protection flags (PROT_NONE, PROT_READ, PROT_WRITE,
 *          PROT_EXEC, or any combination)
 *
 * Returns: 0 on success, -1 (with errno in negative) on error.
 *
 * Errors:
 *   -EINVAL  Invalid protection flags or non-page-aligned address
 *   -ENOMEM  Range exceeds USER_VADDR_MAX or out of memory
 *   -EACCES  W+X requested but W^X is enforced
 *   -EPERM   Range contains sealed (mseal) mappings
 *   -EFAULT  Address range is not fully mapped
 */
int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot);

#endif /* MPROTECT_H */
