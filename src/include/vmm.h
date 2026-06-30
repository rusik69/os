#ifndef VMM_H
#define VMM_H

#include "types.h"

#define VMM_FLAG_PRESENT  (1ULL << 0)
#define VMM_FLAG_WRITE    (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2)
/* Software bit 9 (available to OS) used for Copy-on-Write */
#define VMM_FLAG_COW      (1ULL << 9)

/* ── x86-64 Page Table Entry (PTE) constants ──────────────────────────
 * These correspond to the hardware page-table entry bit layout.
 * Used by the VMM, KPTI, and other low-level page-table code. */
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITE      (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_WRTHROUGH  (1ULL << 3)  /* write-through cache */
#define PTE_CACHE_DIS  (1ULL << 4)  /* cache disable */
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)  /* 2MB / 1GB page */
#define PTE_GLOBAL     (1ULL << 8)  /* global page (not flushed on CR3 reload) */
#define PTE_NX         (1ULL << 63) /* No-Execute (active when EFER.NXE=1) */

/* Physical address mask: lower 12 bits are flags, bits 12..51 are address */
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Aliases used by legacy code */
#define PAGE_PRESENT   PTE_PRESENT
#define PAGE_WRITE     PTE_WRITE
#define PAGE_USER      PTE_USER
#define PAGE_GLOBAL    PTE_GLOBAL
#define PAGE_ACCESSED  PTE_ACCESSED
#define PAGE_DIRTY     PTE_DIRTY
#define PAGE_NX        PTE_NX

/* Software-defined bits (available to OS in bits 9, 10, 11) */
/* Software bit 10 (available to OS) used for lazy/demand allocation.
 * Pages mapped with this flag share the global zero page; on first write
 * the COW handler allocates a real physical page. */
#define VMM_FLAG_LAZY     (1ULL << 10)
/* Software bit 11 (available to OS) used for execute-only page tracking.
 * A page with this flag is executable but NOT readable (software-enforced
 * since x86-64 lacks a hardware read-disable bit).  A read fault on such
 * a page triggers SIGSEGV. */
#define VMM_FLAG_EXECONLY (1ULL << 11)
/* Software bit 52 (available to OS — high address-range available bit) used for
 * mlock/munlock page locking.  Pages with this bit set have an elevated refcount
 * via pmm_ref_frame() and must not be swapped out or freed. */
#define VMM_FLAG_LOCKED   (1ULL << 52)
/* Page-level cache disable (PAT bit) for MMIO */
#define VMM_FLAG_NOCACHE  (1ULL << 4)  /* PCD = Page Cache Disable */
#define VMM_FLAG_NOEXEC   (1ULL << 63) /* No-Execute (NX bit) */

/* ── mmap/mprotect protection flags (POSIX) ────────────── */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* ── mmap mapping flags (Linux-compatible values) ──────── */
#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_GROWSDOWN       0x0100
#define MAP_DENYWRITE       0x0800
#define MAP_EXECUTABLE      0x1000
#define MAP_LOCKED          0x2000
#define MAP_NORESERVE       0x4000
#define MAP_POPULATE        0x8000
#define MAP_NONBLOCK        0x10000
#define MAP_STACK           0x20000
/* MAP_HUGETLB is defined in hugetlb.h (0x40000) */
#define MAP_SYNC            0x80000
#define MAP_FIXED_NOREPLACE 0x100000

/* mremap flags (Linux-compatible values) */
#define MREMAP_MAYMOVE      1
#define MREMAP_FIXED        2

/* MAP_FAILED return value */
#define MAP_FAILED          ((void *)(uint64_t)-1)

/* Validate mmap flags — returns 0 on success, negative errno on invalid combo */
static inline int vmm_validate_mmap_flags(uint64_t flags)
{
    /* Exactly one of MAP_SHARED or MAP_PRIVATE must be set */
    uint64_t type = flags & (MAP_SHARED | MAP_PRIVATE);
    if (type != MAP_SHARED && type != MAP_PRIVATE)
        return -EINVAL;
    /* MAP_SHARED_VALIDATE is a superset of MAP_SHARED */
    if ((flags & MAP_SHARED_VALIDATE) == MAP_SHARED_VALIDATE)
        return -EOPNOTSUPP;
    /* MAP_SHARED without a backing fd requires MAP_ANONYMOUS */
    if ((flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS))
        return -ENODEV;  /* file-backed shared not yet implemented */
    return 0;
}

/* ── Address Space Type (AST) — abstract page permissions ──
 *
 * AST decouples the logical page type from the hardware PTE bits.
 * On x86-64, PRESENT always implies readable, so we cannot truly
 * enforce execute-only in hardware.  We approximate it by:
 *   - Setting the page as PRESENT|USER and clearing NX (executable)
 *   - Tagging the PTE with VMM_FLAG_EXECONLY (software bit 11)
 *   - In the page-fault handler, delivering SIGSEGV if a read/write
 *     access (not an instruction fetch) hits an EXECONLY page.
 *
 * Guard pages (PROT_NONE = AST_NONE) get VMM_FLAG_NOEXEC only
 * (no PRESENT bit), which causes any access to fault as not-present.
 */
#define AST_NONE     0x00  /* No access (guard page)          */
#define AST_READ     0x01  /* Readable                        */
#define AST_WRITE    0x02  /* Writable                        */
#define AST_EXEC     0x04  /* Executable (without read)       */
#define AST_RW       (AST_READ | AST_WRITE)     /* Read-write   */
#define AST_RX       (AST_READ | AST_EXEC)      /* Read-exec    */
#define AST_RWX      (AST_READ | AST_WRITE | AST_EXEC)  /* All */

/* Convert POSIX PROT_* flags to AST flags.
 * PROT_NONE (0) → AST_NONE; otherwise the presence of PROT_READ,
 * PROT_WRITE, PROT_EXEC maps directly. */
static inline uint8_t vmm_prot_to_ast(uint64_t prot) {
    if (prot == 0) return AST_NONE;
    uint8_t ast = 0;
    if (prot & PROT_READ)  ast |= AST_READ;
    if (prot & PROT_WRITE) ast |= AST_WRITE;
    if (prot & PROT_EXEC)  ast |= AST_EXEC;
    return ast;
}

/* Convert AST flags to VMM_FLAG_* combination for page-table mapping.
 * @param ast     AST permission flags
 * @param user    Non-zero if the mapping is for user-space
 * @param noexec  Non-zero if NX is globally enabled (set NX on non-exec pages)
 * @return VMM_FLAG_* bitmask suitable for vmm_map_user_page etc.
 */
static inline uint64_t vmm_ast_to_vmm_flags(uint8_t ast, int user, int noexec) {
    uint64_t flags = 0;
    if (ast == AST_NONE) {
        /* Guard page: not present, mark NX so any access faults */
        if (noexec) flags |= VMM_FLAG_NOEXEC;
        return flags;
    }
    /* Present implies readable on x86-64 */
    flags |= VMM_FLAG_PRESENT;
    if (user) flags |= VMM_FLAG_USER;
    if (ast & AST_WRITE) flags |= VMM_FLAG_WRITE;
    /* Execute-only: page is present (hence readable in hardware),
     * but we tag it for software enforcement in the PF handler. */
    if (ast & AST_EXEC) {
        /* No NX bit — allow execution */
        if ((ast & AST_READ) == 0) {
            /* Execute-only: tag for software enforcement */
            flags |= VMM_FLAG_EXECONLY;
        }
    } else {
        /* Not executable — set NX */
        if (noexec) flags |= VMM_FLAG_NOEXEC;
    }
    return flags;
}

/* User virtual address space limit */
#define USER_VADDR_MAX 0x0000800000000000ULL

void vmm_init(void);
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_set_range_uncacheable(uint64_t virt, uint64_t size);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_physaddr(uint64_t virt);
int vmm_virt_to_phys(uint64_t virt, uint64_t *phys);
uint64_t *vmm_get_pml4(void);

/* Map/unmap physical memory in the kernel's high-half VMA space. */
void *vmm_map_phys(uint64_t phys, uint64_t size, uint64_t flags);
void  vmm_unmap_phys(void *vaddr, uint64_t size);

/* Per-process user address space */
uint64_t *vmm_create_user_pml4(void);
uint64_t *vmm_clone_user_pml4(uint64_t *src); /* deep-copy user half */
int vmm_map_user_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_user_page(uint64_t *pml4, uint64_t virt);
void vmm_switch_pml4(uint64_t *pml4);
void vmm_destroy_user_pml4(uint64_t *pml4);

/* User-memory validation helpers for syscall boundary checks */
int vmm_user_range_ok(uint64_t *pml4, uint64_t addr, uint64_t len, int write);
int vmm_user_string_ok(uint64_t *pml4, uint64_t addr, uint64_t max_len);

/* COW fault handler: handles write fault on a COW page.
 * Returns 1 if fault was handled, 0 if not a COW fault. */
int vmm_handle_cow_fault(uint64_t *pml4, uint64_t virt);

/* mmap/munmap/mprotect helpers */
int vmm_map_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages, uint64_t flags);
int vmm_unmap_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages);
int vmm_set_user_pages_flags(uint64_t *pml4, uint64_t virt, size_t num_pages, uint64_t new_flags);
int vmm_page_is_mapped_user(uint64_t *pml4, uint64_t virt);

/* Lock/unlock user pages — wire/unwire by incrementing/decrementing
 * the physical frame refcount and marking the PTE with VMM_FLAG_LOCKED.
 * Returns 0 on success, -ENOMEM if a page needs to be resolved first
 * (COW break) and allocation fails, or -EFAULT if a page is not mapped. */
int vmm_lock_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages);
int vmm_unlock_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages);

/* Huge page (2MB) support for anonymous mappings */
#define HUGE_PAGE_SIZE      (2ULL * 1024 * 1024)
#define HUGE_PAGE_NFRAMES   512
int vmm_map_user_huge_pages(uint64_t *pml4, uint64_t virt, size_t num_4k_pages, uint64_t flags);
/* Map a single pre-allocated 2MB huge page — used by HugeTLB (MAP_HUGETLB).
 * The caller must provide a 2MB-aligned physical address from the HugeTLB
 * pool.  Returns 0 on success, -1 on failure. */
int vmm_map_user_hugepage_internal(uint64_t *pml4, uint64_t virt,
                                    uint64_t huge_phys, uint64_t flags);

/* Walk a user page table and count present pages.
 * Returns: number of present 4KB pages (huge pages are counted as 512 each).
 * If 'dirty_out' is non-NULL, receives count of writable/dirty pages.
 * If 'shared_out' is non-NULL, receives count of COW/shared pages. */
uint64_t vmm_count_user_pages(uint64_t *pml4, uint64_t *dirty_out, uint64_t *shared_out);

/* Walk a user page table range [start_virt, end_virt) and count present pages.
 * Same semantics as vmm_count_user_pages but restricted to a virtual address range. */
uint64_t vmm_count_user_pages_range(uint64_t *pml4,
                                     uint64_t start_virt, uint64_t end_virt,
                                     uint64_t *dirty_out, uint64_t *shared_out);

/* NX bit support */
void vmm_nx_init(void);
int vmm_check_nx(uint64_t *pml4, uint64_t virt, int write, int exec);

/* Execute-only page check — returns 1 if the page is present and
 * tagged with VMM_FLAG_EXECONLY (software-enforced execute-only). */
int vmm_page_is_execonly(uint64_t *pml4, uint64_t virt);

/* VM statistics counters (for /proc/vmstat) */
extern uint64_t vm_pgalloc;
extern uint64_t vm_pgfree;
extern uint64_t vm_pgfault;
extern uint64_t vm_pgmajfault;
extern uint64_t vm_pgswapin;
extern uint64_t vm_pgswapout;
extern uint64_t vm_pgin;
extern uint64_t vm_pgout;
extern uint64_t vm_hugepages;       /* number of 2MB huge pages allocated */

/* Shared zero page for demand/lazy allocation.
 * A single zero-filled physical frame shared among all lazy mappings.
 * Incremented on each lazy map; never freed. */
extern uint64_t vmm_zero_page_frame;

/* Memory overcommit tracking */
extern uint64_t vmm_committed_bytes;
#define VMM_OVERCOMMIT_LIMIT (256ULL * 1024 * 1024) /* 256 MB overcommit limit */
int vmm_get_committed(void);
int vmm_commit(uint64_t bytes);
void vmm_uncommit(uint64_t bytes);

#endif
