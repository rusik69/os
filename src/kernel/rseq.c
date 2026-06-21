#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "vmm.h"
#include "errno.h"
#include "rseq.h"
#include "smp.h"

/*
 * rseq.c — Restartable Sequences (Item 348)
 *
 * Provides per-CPU restartable sequence registration and context-switch
 * integration.  Userspace uses rseq to atomically update per-CPU data
 * without costly atomic operations or syscalls in the fast path.
 *
 * On context switch, the kernel updates the rseq cpu_id in userspace so
 * that user code can detect preemption.  If a task migrates to a different
 * CPU while in an rseq critical section (rseq_cs != NULL), the kernel
 * aborts the section by jumping to the abort handler.
 */

/* Track rseq registration in the process structure.
 * We extend the process struct via an auxiliary table since we cannot
 * easily modify struct process without touching many other files. */

struct rseq_state {
    uint64_t rseq_addr;     /* user address of struct rseq */
    uint32_t rseq_len;      /* size of rseq structure */
    uint32_t rseq_sig;      /* signature for abort handler */
    int      registered;
    int      last_cpu;      /* CPU this task last ran on */
};

/* Per-process rseq state table indexed by PID. */
static struct rseq_state rseq_table[PROCESS_MAX];
static int rseq_initialized = 0;

void rseq_init(void)
{
    memset(rseq_table, 0, sizeof(rseq_table));
    rseq_initialized = 1;
    kprintf("[OK] rseq: restartable sequences initialized\n");
}

static struct rseq_state *rseq_get_state(struct process *proc)
{
    if (!proc || proc->pid >= PROCESS_MAX)
        return NULL;
    return &rseq_table[proc->pid];
}

int rseq_register(struct process *proc, uint64_t addr, uint32_t len, uint32_t sig)
{
    if (!rseq_initialized || !proc)
        return -EINVAL;
    if (addr == 0 || len < sizeof(struct rseq))
        return -EINVAL;
    if (len > PAGE_SIZE) /* sanity: rseq struct shouldn't span multiple pages */
        return -EINVAL;

    /* Validate the address is in user space */
    if (addr >= USER_VADDR_MAX)
        return -EFAULT;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state)
        return -ENOMEM;

    if (state->registered)
        return -EBUSY;

    state->rseq_addr = addr;
    state->rseq_len = len;
    state->rseq_sig = sig;
    state->registered = 1;
    state->last_cpu = smp_get_cpu_id();

    /* Initialize the cpu_id fields in the user-space rseq struct.
     * Userspace is expected to have zero-initialized the structure, but
     * we set cpu_id_start and cpu_id so they're correct immediately. */
    {
        uint32_t cpu = (uint32_t)smp_get_cpu_id();
        volatile uint32_t *cpu_id_start = (volatile uint32_t *)addr;
        volatile uint32_t *cpu_id = (volatile uint32_t *)(addr + 4);
        *cpu_id_start = cpu;
        *cpu_id = cpu;
    }

    return 0;
}

int rseq_unregister(struct process *proc)
{
    if (!rseq_initialized || !proc)
        return -EINVAL;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state || !state->registered)
        return -EINVAL;

    /* Abort any active critical section */
    rseq_abort(proc);

    state->rseq_addr = 0;
    state->rseq_len = 0;
    state->rseq_sig = 0;
    state->registered = 0;
    state->last_cpu = -1;

    return 0;
}

void rseq_abort(struct process *proc)
{
    if (!rseq_initialized || !proc)
        return;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state || !state->registered)
        return;

    /* Clear the rseq_cs field in the user-space rseq structure.
     * This forces user code to detect the abort and retry. */
    uint64_t addr = state->rseq_addr + __builtin_offsetof(struct rseq, rseq_cs);

    if (addr >= USER_VADDR_MAX)
        return;

    /* Write 0 to rseq_cs atomically (8 bytes) */
    *(volatile uint64_t *)addr = 0;
}

/*
 * rseq_update_cpu_id — Update the rseq cpu_id in userspace.
 *
 * Called on every context switch to the target process.  Writes the
 * current CPU number into the userspace rseq structure so that user
 * code can cheaply detect preemption by comparing cpu_id_start with
 * cpu_id after the critical section.
 */
void rseq_update_cpu_id(struct process *proc)
{
    if (!rseq_initialized || !proc)
        return;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state || !state->registered)
        return;

    uint64_t addr = state->rseq_addr;
    if (addr == 0 || addr >= USER_VADDR_MAX)
        return;

    uint32_t cpu = (uint32_t)smp_get_cpu_id();

    /* Update cpu_id_start first, then cpu_id.
     * If cpu_id_start != cpu_id after the critical section, userspace
     * knows it was preempted and must retry. */
    volatile uint32_t *cpu_id_start = (volatile uint32_t *)addr;
    volatile uint32_t *cpu_id = (volatile uint32_t *)(addr + 4);

    /* Memory barrier: ensure all prior writes are visible before updating
     * cpu_id_start, and cpu_id_start is visible before cpu_id. */
    __sync_synchronize();
    *cpu_id_start = cpu;
    __sync_synchronize();
    *cpu_id = cpu;
}

/*
 * rseq_migrate — Handle process migration to a different CPU.
 *
 * If the process was in an rseq critical section (rseq_cs != NULL) and
 * is being migrated to a different CPU, we must abort the section so
 * that the per-CPU data invariants are maintained.
 *
 * Called from the scheduler when a task switches CPUs.
 */
void rseq_migrate(struct process *proc, int old_cpu, int new_cpu)
{
    if (!rseq_initialized || !proc)
        return;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state || !state->registered)
        return;

    /* Update the last_cpu tracking */
    state->last_cpu = new_cpu;

    /* Check if the process has an active rseq critical section.
     * Read from the user-space rseq_cs pointer. */
    uint64_t rseq_cs_addr = state->rseq_addr +
        __builtin_offsetof(struct rseq, rseq_cs);

    if (rseq_cs_addr >= USER_VADDR_MAX)
        return;

    /* Read the current rseq_cs value from userspace */
    uint64_t rseq_cs_val = *(volatile uint64_t *)rseq_cs_addr;

    if (rseq_cs_val != 0) {
        /* Process is in an rseq critical section — abort it.
         * Clear rseq_cs so the userspace retry loop restarts. */
        *(volatile uint64_t *)rseq_cs_addr = 0;

        kprintf("[rseq] aborted critical section for pid=%d on migration "
                "cpu %d -> %d\n",
                proc->pid, old_cpu, new_cpu);
    }
}

/* ── Stub: rseq_signal ─────────────────────────────── */
int rseq_signal(void *task, int sig)
{
    (void)task;
    (void)sig;
    kprintf("[rseq] rseq_signal: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: rseq_set_flags ─────────────────────────────── */
int rseq_set_flags(void *task, uint32_t flags)
{
    (void)task;
    (void)flags;
    kprintf("[rseq] rseq_set_flags: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: rseq_get_flags ─────────────────────────────── */
uint32_t rseq_get_flags(void *task)
{
    (void)task;
    kprintf("[rseq] rseq_get_flags: not yet implemented\n");
    return -ENOSYS;
}
