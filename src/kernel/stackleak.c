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

void stackleak_poison_stack(void)
{
    if (!stackleak_enabled) return;

    struct process *p = process_get_current();
    if (!p) return;

    /* Determine stack boundaries from the process descriptor.
     * Kernel stack grows downward from stack_top. */
    uint64_t *stack_base = (uint64_t *)p->kernel_stack;
    if (!stack_base) return;

    /* Poison the entire kernel stack region. In a more refined
     * implementation we would track the exact high-water mark. */
    int stack_words = STACKLEAK_STACK_SIZE / sizeof(uint64_t);
    for (int i = 0; i < stack_words; i++) {
        stack_base[i] = STACKLEAK_POISON_VALUE;
    }

    stackleak_poison_count++;
}

/* ── Called from syscall entry ────────────────────────────────────── */

void stackleak_syscall_entry(void)
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

void stackleak_syscall_exit(void)
{
    stackleak_poison_stack();
}

/* ── Sysctl handler ────────────────────────────────────────────────── */

int stackleak_set_enabled(int val)
{
    int old = stackleak_enabled;
    stackleak_enabled = val ? 1 : 0;
    return old;
}

int stackleak_get_enabled(void)
{
    return stackleak_enabled;
}

uint64_t stackleak_get_poison_count(void)
{
    return stackleak_poison_count;
}

/* ── Initialization ────────────────────────────────────────────────── */

void stackleak_init(void)
{
    kprintf("[OK] STACKLEAK: kernel stack eraser initialized (poison=0x%016llx)\n",
            (unsigned long long)STACKLEAK_POISON_VALUE);
}

/* ── Stub: stackleak_check ─────────────────────────────── */
int stackleak_check(void *task)
{
    (void)task;
    kprintf("[stackleak] stackleak_check: not yet implemented\n");
    return 0;
}
/* ── Stub: stackleak_erase ─────────────────────────────── */
int stackleak_erase(void *task)
{
    (void)task;
    kprintf("[stackleak] stackleak_erase: not yet implemented\n");
    return 0;
}
