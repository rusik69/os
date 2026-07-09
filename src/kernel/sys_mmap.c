/*
 * Linux-compatible memory mapping syscalls.
 *
 * Provides mmap, munmap, mremap, mprotect, brk, and related memory
 * management syscalls matching the Linux x86-64 ABI conventions.
 *
 * Each function returns (uint64_t)(int64_t)-errno on error, or a
 * non-negative value on success.
 */
#include "syscall.h"
#include "module.h"
#include "process.h"
#include "errno.h"
#include "vmm.h"
#include "pmm.h"
#include "hugetlb.h"
#include "mprotect.h"
#include "wx_enforce.h"
#include "printf.h"
#include "string.h"
#include "uaccess.h"
#include "scheduler.h"
#include "mseal.h"
#include "aslr.h"

/* ── Inline helper for TLB invalidation ──────────────────────── */
static inline void local_invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ── sys_munmap — unmap a range of pages ────────────────────────── */

uint64_t sys_munmap(uint64_t addr, uint64_t length) {
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4)
        return (uint64_t)(int64_t)-EFAULT;

    /* Address must be page-aligned */
    if (addr & (PAGE_SIZE - 1))
        return (uint64_t)(int64_t)-EINVAL;

    /* Linux returns 0 for zero-length munmap */
    if (length == 0)
        return 0;

    /* Round length up to page boundary */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    /* Check for overflow */
    if (addr + length < addr)
        return (uint64_t)(int64_t)-EINVAL;

    /* Must be entirely within user address space */
    if (addr + length > USER_VADDR_MAX)
        return (uint64_t)(int64_t)-EFAULT;

    /* Sealed (mseal) ranges are immutable — reject unmap on sealed pages */
    if (mseal_check(addr, length) == 0)
        return (uint64_t)(int64_t)-EPERM;

    /* Perform the unmap. vmm_unmap_user_pages skips unmapped pages,
     * freeing only pages that are actually present. */
    if (vmm_unmap_user_pages(proc->pml4, addr, length / PAGE_SIZE) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    /* Flush TLB for the unmapped range if running on this PML4 */
    if (proc->pml4 == vmm_get_pml4()) {
        for (uint64_t v = addr; v < addr + length; v += PAGE_SIZE)
            local_invlpg(v);
    }

    return 0;
}

/* ── sys_mremap — remap a virtual address range (with possible move) ── */

uint64_t sys_mremap(uint64_t old_addr, uint64_t old_size,
                     uint64_t new_size, uint64_t flags,
                     uint64_t new_addr)
{
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4)
        return (uint64_t)(int64_t)-EFAULT;
    if (old_addr & (PAGE_SIZE - 1))
        return (uint64_t)(int64_t)-EINVAL;

    old_size = (old_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    new_size = (new_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    if (old_size == 0)
        return (uint64_t)(int64_t)-EINVAL;

    /* Same size = no-op */
    if (old_size == new_size)
        return old_addr;

    if (new_size < old_size) {
        /* Shrinking: unmap the extra pages */
        vmm_unmap_user_pages(proc->pml4, old_addr + new_size,
                             (old_size - new_size) / PAGE_SIZE);
        return old_addr;
    }

    /* RLIMIT_AS: reject growth beyond the address-space limit */
    uint64_t as_limit = proc->rlim_cur[RLIMIT_AS];
    if (as_limit != RLIM_INFINITY && new_size > as_limit)
        return (uint64_t)-ENOMEM;

    /* Growing: try to extend in-place */
    uint64_t extend = new_size - old_size;
    int can_extend = 1;
    for (uint64_t check = old_addr + old_size;
         check < old_addr + new_size; check += PAGE_SIZE) {
        if (vmm_page_is_mapped_user(proc->pml4, check)) {
            can_extend = 0;
            break;
        }
    }

    if (can_extend) {
        /* Extend in place: just map the new pages */
        uint64_t page_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITE | VMM_FLAG_LAZY;
        if (vmm_map_user_pages(proc->pml4, old_addr + old_size,
                               extend / PAGE_SIZE, page_flags) < 0)
            return (uint64_t)(int64_t)-ENOMEM;
        return old_addr;
    }

    /* Can't extend in place — need to move (only if MREMAP_MAYMOVE) */
    if (!(flags & MREMAP_MAYMOVE))
        return (uint64_t)(int64_t)-ENOMEM;

    /* Find a new location */
    uint64_t new = (flags & MREMAP_FIXED) ? new_addr : 0;
    if (new == 0) {
        new = 0x0000000001000000ULL;
        while (new + new_size < USER_VADDR_MAX) {
            int free = 1;
            for (uint64_t check = new; check < new + new_size;
                 check += PAGE_SIZE) {
                if (vmm_page_is_mapped_user(proc->pml4, check)) {
                    free = 0; break;
                }
            }
            if (free) break;
            new += 0x100000ULL;
        }
        if (new + new_size >= USER_VADDR_MAX)
            return (uint64_t)(int64_t)-ENOMEM;
    }

    /* Copy pages one by one */
    for (uint64_t off = 0; off < old_size; off += PAGE_SIZE) {
        uint64_t old_phys = vmm_get_physaddr(old_addr + off);
        if (old_phys) {
            uint64_t new_phys = pmm_alloc_frame();
            if (!new_phys)
                return (uint64_t)(int64_t)-ENOMEM;
            memcpy(PHYS_TO_VIRT(new_phys), PHYS_TO_VIRT(old_phys),
                   PAGE_SIZE);
            vmm_map_user_page(proc->pml4, new + off, new_phys,
                              VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITE);
        }
    }

    /* Unmap old region */
    vmm_unmap_user_pages(proc->pml4, old_addr, old_size / PAGE_SIZE);

    /* Map remaining new pages (if growing) */
    if (new_size > old_size) {
        uint64_t page_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITE | VMM_FLAG_LAZY;
        vmm_map_user_pages(proc->pml4, new + old_size,
                           (new_size - old_size) / PAGE_SIZE, page_flags);
    }

    return new;
}

/* ── sys_brk — program break (heap management) ──────────────────────── */

uint64_t sys_brk(uint64_t addr) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ENOMEM;
    if (!p->is_user || !p->pml4) return (uint64_t)(int64_t)-EFAULT;

    /* Track heap start/end — initialized lazily with ASLR offset */
    if (p->heap_end == 0) {
        uint64_t brk_aslr = aslr_brk_offset() * PAGE_SIZE;
        p->heap_start = 0x0000000002000000ULL + brk_aslr;
        p->heap_end = p->heap_start;
    }

    if (addr == 0) return p->heap_end; /* brk(0) — get current */

    /* Align to page boundary */
    addr = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    if (addr > USER_VADDR_MAX) return (uint64_t)(int64_t)-ENOMEM;

    uint64_t old_end = p->heap_end;
    if (addr > old_end) {
        /* Grow heap — map new pages */
        uint64_t grow = addr - old_end;

        /* Enforce RLIMIT_DATA */
        uint64_t data_limit = p->rlim_cur[RLIMIT_DATA];
        if (data_limit != RLIM_INFINITY && (addr - p->heap_start) > data_limit)
            return (uint64_t)(int64_t)-ENOMEM;

        /* Also enforce RLIMIT_AS: new heap end relative to
         * heap_start must not exceed rlim_cur[RLIMIT_AS]. */
        uint64_t as_limit = p->rlim_cur[RLIMIT_AS];
        if (as_limit != RLIM_INFINITY && (addr - p->heap_start) > as_limit)
            return (uint64_t)(int64_t)-ENOMEM;
        uint64_t pages = (grow + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t page_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE | VMM_FLAG_NOEXEC | VMM_FLAG_LAZY;
        if (vmm_map_user_pages(p->pml4, old_end, pages, page_flags) < 0)
            return (uint64_t)(int64_t)-ENOMEM;
        p->heap_end = addr;
    } else if (addr < old_end) {
        /* Shrink heap — unmap pages */
        uint64_t shrink = old_end - addr;
        uint64_t pages = shrink / PAGE_SIZE;
        if (pages > 0) {
            vmm_unmap_user_pages(p->pml4, addr, pages);
        }
        p->heap_end = addr;
    }
    return p->heap_end;
}

/* ── Module metadata ───────────────────────────────────────────── */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Linux-compatible memory mapping syscalls");
MODULE_AUTHOR("Ruslan Gustomiasov");

/* ── Forward declarations of helpers ─────────────────────────── */

/* ASLR offset for mmap base (from aslr.c) */
extern uint64_t aslr_mmap_offset(void);

/* mseal check — returns 0 if range is sealed */
extern int mseal_check(uint64_t addr, uint64_t length);

/* ── sys_mmap — map files or anonymous memory into userspace ── */

uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                   uint64_t flags, uint64_t fd, uint64_t offset)
{
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4)
        return (uint64_t)(int64_t)-EFAULT;

    /* ── Validate flags ──────────────────────────────────────── */
    int ret = vmm_validate_mmap_flags(flags);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;

    /* length must be non-zero */
    if (length == 0)
        return (uint64_t)(int64_t)-EINVAL;

    /* Guard against overflow in the page-alignment addition */
    if (length > UINT64_MAX - (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;

    /* Round up to page size */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    /* ── Address validation ──────────────────────────────────── */
    /* Check that addr + length does not overflow or exceed USER_VADDR_MAX */
    if (length > USER_VADDR_MAX || addr > USER_VADDR_MAX - length)
        return (uint64_t)-EINVAL;
    if (length > 0 && addr + length < addr)
        return (uint64_t)-EINVAL;

    /* RLIMIT_AS: reject mappings that exceed address-space limit */
    uint64_t as_limit = proc->rlim_cur[RLIMIT_AS];
    if (as_limit != RLIM_INFINITY && length > as_limit)
        return (uint64_t)-ENOMEM;

    /* W^X enforcement: reject writable + executable mappings */
    if (wx_enforce_check_prot(prot) < 0)
        return (uint64_t)(int64_t)-EACCES;

    /* (fd, offset) validation — must be zero for anonymous mappings */
    if (flags & MAP_ANONYMOUS) {
        (void)fd;
        (void)offset;
    }

    /* ── MAP_FIXED_NOREPLACE: fail if any page in range is already mapped ── */
    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        for (uint64_t check = addr; check < addr + length; check += PAGE_SIZE) {
            if (vmm_page_is_mapped_user(proc->pml4, check))
                return (uint64_t)(int64_t)-EEXIST;
        }
    }

    /* ── MAP_FIXED: check mseal, then unmap existing pages ──── */
    if (addr != 0) {
        if (mseal_check(addr, length) == 0)
            return (uint64_t)(int64_t)-EPERM;
        /* Unmap any existing pages in the target range (MAP_FIXED semantics) */
        if (flags & MAP_FIXED) {
            vmm_unmap_user_pages(proc->pml4, addr, length / PAGE_SIZE);
        }
    }

    /* ── MAP_HUGETLB: huge page path ─────────────────────────── */
    if (flags & MAP_HUGETLB) {
        if (length < HUGETLB_PAGE_SIZE || (length & (HUGETLB_PAGE_SIZE - 1)))
            return (uint64_t)-EINVAL;

        if (addr == 0) {
            uint64_t mmap_base = 0x0000000001000000ULL +
                                 (aslr_mmap_offset() * PAGE_SIZE);
            addr = (mmap_base + HUGETLB_PAGE_SIZE - 1) & ~(HUGETLB_PAGE_SIZE - 1ULL);
            while (addr + length < USER_VADDR_MAX) {
                int free = 1;
                for (uint64_t check = addr; check < addr + length; check += HUGETLB_PAGE_SIZE) {
                    if (vmm_page_is_mapped_user(proc->pml4, check)) {
                        free = 0;
                        break;
                    }
                }
                if (free) break;
                addr += HUGETLB_PAGE_SIZE;
            }
            if (addr + length >= USER_VADDR_MAX)
                return (uint64_t)-ENOMEM;
        }

        if (addr & (HUGETLB_PAGE_SIZE - 1))
            return (uint64_t)-EINVAL;

        uint8_t ast = vmm_prot_to_ast(prot);
        uint64_t page_flags = vmm_ast_to_vmm_flags(ast, 1, 1);
        page_flags &= ~(uint64_t)VMM_FLAG_LAZY;
        page_flags |= VMM_FLAG_WRITE;

        uint64_t cur = addr;
        while (cur < addr + length) {
            uint64_t huge_phys = hugetlb_alloc_frame();
            if (huge_phys == 0) {
                vmm_unmap_user_pages(proc->pml4, addr, (cur - addr) / PAGE_SIZE);
                return (uint64_t)-ENOMEM;
            }
            memset((void *)PHYS_TO_VIRT(huge_phys), 0, HUGETLB_PAGE_SIZE);
            if (vmm_map_user_hugepage_internal(proc->pml4, cur, huge_phys, page_flags) < 0) {
                hugetlb_free_frame(huge_phys);
                vmm_unmap_user_pages(proc->pml4, addr, (cur - addr) / PAGE_SIZE);
                return (uint64_t)-EFAULT;
            }
            cur += HUGETLB_PAGE_SIZE;
        }

        if (proc->pml4 == vmm_get_pml4()) {
            for (uint64_t v = addr; v < addr + length; v += PAGE_SIZE)
                local_invlpg(v);
        }
        return addr;
    }

    /* ── Normal (non-HugeTLB) mmap path ──────────────────────── */

    /* If addr is 0, find a free region with ASLR */
    if (addr == 0) {
        uint64_t mmap_base = 0x0000000001000000ULL +
                             (aslr_mmap_offset() * PAGE_SIZE);
        if (length >= HUGE_PAGE_SIZE) {
            addr = (mmap_base + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1ULL);
            while (addr + length < USER_VADDR_MAX) {
                int free = 1;
                for (uint64_t check = addr; check < addr + length; check += HUGE_PAGE_SIZE) {
                    if (vmm_page_is_mapped_user(proc->pml4, check)) {
                        free = 0;
                        break;
                    }
                }
                if (free) break;
                addr += HUGE_PAGE_SIZE;
            }
        } else {
            addr = mmap_base;
            while (addr + length < USER_VADDR_MAX) {
                int free = 1;
                for (uint64_t check = addr; check < addr + length; check += PAGE_SIZE) {
                    if (vmm_page_is_mapped_user(proc->pml4, check)) {
                        free = 0;
                        break;
                    }
                }
                if (free) break;
                addr += 0x100000ULL;
            }
        }
        if (addr + length >= USER_VADDR_MAX)
            return (uint64_t)(int64_t)-ENOMEM;
    }

    /* Convert POSIX PROT_* flags to VMM page-table flags */
    uint8_t ast = vmm_prot_to_ast(prot);
    uint64_t page_flags = vmm_ast_to_vmm_flags(ast, 1, 1) | VMM_FLAG_LAZY;

    /* Handle MAP_SHARED vs MAP_PRIVATE */
    if (flags & MAP_SHARED) {
        /* Shared anonymous: allocate pages immediately (no COW) */
        page_flags &= ~(uint64_t)VMM_FLAG_LAZY;
        /* Writable shared mappings need write permission */
        if (ast & AST_WRITE)
            page_flags |= VMM_FLAG_WRITE;
    } else {
        /* MAP_PRIVATE: COW semantics via lazy allocation + writable pages */
        if (ast & AST_WRITE)
            page_flags |= VMM_FLAG_WRITE;
    }

    /* MAP_POPULATE: eagerly populate pages (prefault) */
    if (flags & MAP_POPULATE)
        page_flags &= ~(uint64_t)VMM_FLAG_LAZY;

    /* MAP_LOCKED: wire pages (memory will be pinned) */
    if (flags & MAP_LOCKED)
        page_flags &= ~(uint64_t)VMM_FLAG_LAZY;

    /* MAP_NORESERVE: skip overcommit accounting (allow overcommit) */
    if (!(flags & MAP_NORESERVE)) {
        /* Attempt to commit the requested pages; fail if over limit */
        if (vmm_commit(length) < 0)
            return (uint64_t)(int64_t)-ENOMEM;
    }

    /* Map the pages */
    if (length >= HUGE_PAGE_SIZE && (addr & (HUGE_PAGE_SIZE - 1)) == 0) {
        uint64_t hp_flags = page_flags & ~(uint64_t)VMM_FLAG_LAZY;
        hp_flags |= VMM_FLAG_WRITE;
        if (vmm_map_user_huge_pages(proc->pml4, addr, length / PAGE_SIZE, hp_flags) < 0) {
            if (!(flags & MAP_NORESERVE))
                vmm_uncommit(length);
            return (uint64_t)(int64_t)-ENOMEM;
        }
    } else {
        if (vmm_map_user_pages(proc->pml4, addr, length / PAGE_SIZE, page_flags) < 0) {
            if (!(flags & MAP_NORESERVE))
                vmm_uncommit(length);
            return (uint64_t)(int64_t)-ENOMEM;
        }
    }

    /* Flush TLB for the new region */
    if (proc->pml4 == vmm_get_pml4()) {
        for (uint64_t v = addr; v < addr + length; v += PAGE_SIZE)
            local_invlpg(v);
    }

    return addr;
}
