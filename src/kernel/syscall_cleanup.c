/*
 * syscall_cleanup.c — Kernel stack zeroing on return to userspace
 *
 * FINER (Item 180): Clear kernel stack data before returning to
 * user mode to prevent information disclosure through residual
 * kernel stack contents.
 *
 * On every syscall entry via the `syscall` instruction (ring-3 →
 * ring-0 transition), the assembly entry point (syscall_entry in
 * syscall_asm.asm) saves all user-visible registers on the kernel
 * stack and records the stack pointer (entry_rsp).  After the C
 * syscall dispatcher returns, the assembly path calls the function
 * below to zero the portion of the kernel stack that was used
 * during the syscall, BEFORE restoring the saved registers and
 * returning to userspace via sysret.
 *
 * The zeroed region is everything between the kernel stack base
 * (proc->kernel_stack) and a conservative point just below the
 * saved register frame, leaving enough headroom for this function's
 * own stack frame so we don't accidentally clobber our own return
 * path.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "process.h"
#include "string.h"

/* ── Stack zeroing helper ──────────────────────────────────────────── */

void zero_kernel_stack_uapi(uint64_t entry_rsp)
{
    struct process *proc = process_get_current();
    if (!proc)
        return;

    uint64_t stack_base = proc->kernel_stack;
    uint64_t stack_top  = proc->stack_top;

    /* Sanity checks: entry_rsp must be within the kernel stack bounds
     * and above (higher address than) the stack base. */
    if (entry_rsp == 0 ||
        entry_rsp <= stack_base ||
        entry_rsp >= stack_top + 4096) {
        return;
    }

    /*
     * Zero everything from the stack base up to entry_rsp, minus a
     * safety margin that accounts for:
     *   1. The return address pushed by the `call` instruction that
     *      called this function (8 bytes below entry_rsp).
     *   2. This function's own stack frame (register saves, locals).
     *
     * A 128-byte margin is conservative — our frame is ~40-48 bytes
     * on a typical build (saves: rbp 8, rbx 8, r12 8, r13 8; plus
     * local vars).  With 128 bytes we are well within safety.
     */
#define STACK_ZERO_SAFE_MARGIN 128

    uint64_t zero_end = entry_rsp;
    if (zero_end <= stack_base + STACK_ZERO_SAFE_MARGIN)
        return; /* not enough room to zero safely */

    zero_end -= STACK_ZERO_SAFE_MARGIN;

    size_t count = (size_t)(zero_end - stack_base);
    if (count > 0) {
        /* Use volatile store to prevent the compiler from optimising
         * away the zeroing as "dead store". */
        volatile char *p = (volatile char *)stack_base;
        for (size_t i = 0; i < count; i++)
            p[i] = 0x00;
    }
}
