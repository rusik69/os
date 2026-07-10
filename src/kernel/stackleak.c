/* stackleak.c — Kernel stack eraser
 *
 * Poisons kernel stack on syscall exit to prevent information leaks.
 * Inspired by the Linux kernel's STACKLEAK feature (grsecurity/PaX).
 *
 * On each syscall exit (or interrupt return to user-space), we fill
 * the used portion of the kernel stack with a known poison value.
 * This prevents leaking stack contents across syscalls.
 */

#include "types.h"
#include "printf.h"
#include "scheduler.h"
#include "process.h"
#include "cpu.h"
#include "string.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define STACKLEAK_POISON_VALUE  0xDEADBEEFDEADBEEFull
#define STACKLEAK_STACK_ORDER   2       /* size = (1U << order) * 4KB */
#define STACKLEAK_STACK_SIZE    (4096 * (1U << STACKLEAK_STACK_ORDER))

/* ── State ─────────────────────────────────────────────────────────── */

static int stackleak_enabled = 1;   /* can be disabled via sysctl */
static uint64_t stackleak_poison_count = 0;

/* ── Core poison routine ───────────────────────────────────────────── */

static void stackleak_poison_stack(void)
{
    if (!stackleak_enabled) return;

    struct process *p = process_get_current();
    if (!p) return;

    uint64_t *stack_base = (uint64_t *)p->kernel_stack;
    if (!stack_base) return;

    /*
     * Read the current stack pointer.  The kernel stack grows downward
     * from stack_top toward kernel_stack (base).  Everything at addresses
     * below current RSP is stale data from earlier call frames; everything
     * at or above RSP is actively live and MUST NOT be poisoned.
     *
     * We poison only the stale region: [kernel_stack, rsp - margin).
     * The margin protects this function's own stack frame and any
     * registers the compiler saved on entry.
     */
    uint64_t current_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(current_rsp));

    const uint64_t margin = 256;  /* safety headroom */

    /* Bail if the stack pointer is too close to the base to poison
     * safely — there must be at least margin bytes between them. */
    if (current_rsp <= p->kernel_stack + margin)
        return;

    uint64_t poison_end = current_rsp - margin;
    size_t poison_bytes = poison_end - p->kernel_stack;
    size_t poison_words = poison_bytes / sizeof(uint64_t);

    for (size_t i = 0; i < poison_words; i++) {
        stack_base[i] = STACKLEAK_POISON_VALUE;
    }

    stackleak_poison_count++;
}

/* ── Called from syscall entry ────────────────────────────────────── */

static void stackleak_syscall_entry(void)
{
    /* Record current stack pointer for later poisoning bounds.
     * Called at the very start of syscall entry (before any kernel stack use). */
    struct process *p = process_get_current();
    if (!p) return;

    /* In a full implementation, store the low watermark in per-process state.
     * Here we just ensure the poison function has the right context. */
    (void)p;
}

/* ── Called from syscall exit ─────────────────────────────────────── */

static void stackleak_syscall_exit(void)
{
    stackleak_poison_stack();
}

/* ── Sysctl handler ────────────────────────────────────────────────── */

static int stackleak_set_enabled(int val)
{
    int old = stackleak_enabled;
    stackleak_enabled = val ? 1 : 0;
    return old;
}

static int stackleak_get_enabled(void)
{
    return stackleak_enabled;
}

static uint64_t stackleak_get_poison_count(void)
{
    return stackleak_poison_count;
}

/* ── Initialization ────────────────────────────────────────────────── */

static void __init stackleak_init(void)
{
    kprintf("[OK] STACKLEAK: kernel stack eraser initialized (poison=0x%016llx)\n",
            (unsigned long long)STACKLEAK_POISON_VALUE);
}

/* ── Stub: stackleak_check ─────────────────────────────── */
static int stackleak_check(void *task)
{
    (void)task;
    kprintf("[stackleak] stackleak_check: not yet implemented\n");
    return 0;
}
/* ── Stub: stackleak_erase ─────────────────────────────── */
static int stackleak_erase(void *task)
{
    (void)task;
    kprintf("[stackleak] stackleak_erase: not yet implemented\n");
    return 0;
}
