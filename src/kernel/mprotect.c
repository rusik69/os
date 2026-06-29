/*
 * mprotect.c — mprotect syscall implementation
 *
 * Changes memory protection flags for a user-space virtual address range.
 * This is a self-contained implementation that can be called from the
 * syscall dispatch table in syscall.c.
 *
 * The function:
 *   1. Validates the address range and protection flags
 *   2. Checks W^X enforcement (rejects W+X if wx_enabled == 0)
 *   3. Checks mseal (rejects changes to sealed ranges)
 *   4. Converts POSIX PROT_* to VMM page-table flags via AST
 *   5. Updates the page table entries via vmm_set_user_pages_flags()
 *   6. Flushes TLB entries for the modified range
 *
 * Security notes:
 *   - Address must be page-aligned
 *   - Range must be within user space (< USER_VADDR_MAX)
 *   - W^X policy prevents creating writable+executable mappings
 *   - Sealed (mseal) ranges are immutable
 */

#define KERNEL_INTERNAL
#include "mprotect.h"
#include "process.h"
#include "vmm.h"
#include "errno.h"
#include "printf.h"
#include "wx_enforce.h"
#include "cpu.h"        /* for local_invlpg */

/* local_invlpg — flush a single page from the TLB via INVLPG instruction */
static inline void local_invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Forward declaration of mseal_check (defined in mseal.c) */
extern int mseal_check(uint64_t addr, uint64_t length);

/* ── mprotect implementation ────────────────────────────────────────── */

int64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot) {
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4) return (int64_t)-EFAULT;

    /* ── Validate protection flags ───────────────────────────────────
     * Only PROT_READ (1), PROT_WRITE (2), PROT_EXEC (4),
     * combinations thereof, or PROT_NONE (0) are valid.
     * Bits 3-63 must be zero. */
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return (int64_t)-EINVAL;

    /* ── W^X enforcement ─────────────────────────────────────────────
     * Reject writable + executable mappings unless the sysctl allows. */
    {
        int wx_ret = wx_enforce_check_prot(prot);
        if (wx_ret < 0)
            return (int64_t)wx_ret;
    }

    /* ── Address must be page-aligned ──────────────────────────────── */
    if (addr & (PAGE_SIZE - 1))
        return (int64_t)-EINVAL;

    /* ── Round length to page boundary ─────────────────────────────── */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    /* ── Check for overflow and user-space boundary ────────────────── */
    if (addr + length < addr)
        return (int64_t)-EINVAL;
    if (addr + length > USER_VADDR_MAX)
        return (int64_t)-ENOMEM;

    /* ── Check mseal (sealed ranges are immutable) ─────────────────── */
    if (mseal_check(addr, length) == 0)
        return (int64_t)-EPERM;

    /* ── Convert POSIX PROT_* to VMM page-table flags via AST ────────
     * This automatically handles:
     *   - PROT_NONE    → guard page (no PRESENT, sets NX)
     *   - PROT_READ    → readable (PRESENT|USER, NX)
     *   - PROT_EXEC    → executable (PRESENT|USER, no NX)
     *   - PROT_EXEC without PROT_READ → execute-only tag (EXECONLY bit)
     *     for software-level enforcement in the page-fault handler. */
    uint8_t ast = vmm_prot_to_ast(prot);
    uint64_t page_flags = vmm_ast_to_vmm_flags(ast, 1, 1);

    /* ── Update page table entries ─────────────────────────────────── */
    if (vmm_set_user_pages_flags(proc->pml4, addr,
                                 length / PAGE_SIZE, page_flags) < 0)
        return (int64_t)-EFAULT;

    /* ── Flush TLB for the modified range ────────────────────────────
     * If this is the current process's page table, flush each page.
     * For non-current processes, the TLB will be flushed on context
     * switch (CR3 reload). */
    if (proc->pml4 == vmm_get_pml4()) {
        for (uint64_t v = addr; v < addr + length; v += PAGE_SIZE)
            local_invlpg(v);
    }

    return 0;
}

/* ── Stub: mprotect_check ─────────────────────────────── */
int mprotect_check(uint64_t addr, size_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    kprintf("[mprotect] mprotect_check: not yet implemented\n");
    return 0;
}
/* ── Stub: mprotect_apply ─────────────────────────────── */
int mprotect_apply(uint64_t addr, size_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    kprintf("[mprotect] mprotect_apply: not yet implemented\n");
    return 0;
}
