#define KERNEL_INTERNAL
#include "rseq.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "vmm.h"
#include "errno.h"
#include "types.h"
#include "smp.h"
#include "idt.h"
#include "cpu.h"

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

void __init rseq_init(void)
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

/*
 * rseq_user_access — validated, SMAP-safe copy to/from a process's
 * user-space memory.
 *
 * All rseq user pointers must go through this helper (never a raw
 * dereference): the kernel runs with SMAP enabled on capable CPUs, so a
 * raw access faults with #PF, and without validation a user-supplied
 * address can point at an unmapped page (kernel oops) or at
 * kernel-mapped low memory (arbitrary kernel memory corruption on
 * SMAP-less CPUs).  The generic copy_from_user/copy_to_user cannot be
 * used here because they resolve the target process via
 * process_get_current() — wrong in rseq_migrate(), where the scheduler
 * has already set current to the *incoming* task while the outgoing
 * task's page tables are still active — and because they unconditionally
 * re-enable interrupts (sti), which would enable interrupts in the
 * middle of schedule()'s IF=0 critical section.
 *
 * proc is the process whose address space addr belongs to.  The caller
 * must guarantee proc's page tables are (or can be made) active; the
 * helper switches CR3 to proc's pml4 if needed, exactly like the uaccess
 * helpers do, and preserves the interrupt state it was called with.
 *
 * Returns 0 on success, -EFAULT on any validation or copy failure.
 */
static int rseq_user_access(struct process *proc, uint64_t addr,
                            void *buf, size_t n, int to_user)
{
    if (!proc || !proc->pml4)
        return -EFAULT;
    if (addr == 0)
        return -EFAULT;
    if (addr + n < addr || addr + n > USER_VADDR_MAX)
        return -EFAULT;
    /* Reject unmapped, non-user, and (for writes) non-writable pages. */
    if (!vmm_user_range_ok(proc->pml4, addr, n, to_user ? 1 : 0))
        return -EFAULT;

    /* Save IF so this works both from syscall context (IF=1) and from
     * schedule() (IF=0, timer IRQ).  Never enable interrupts in the
     * middle of a context switch. */
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    uint64_t old_cr3 = read_cr3();
    uint64_t user_cr3 = VIRT_TO_PHYS((uint64_t)proc->pml4);
    int switched = ((old_cr3 & PTE_ADDR_MASK) != user_cr3);
    if (switched)
        write_cr3(user_cr3);

    stac();
    if (to_user)
        memcpy((void *)(uintptr_t)addr, buf, n);
    else
        memcpy(buf, (const void *)(uintptr_t)addr, n);
    clac();

    if (switched)
        write_cr3(old_cr3);
    if (flags & 0x200) /* X86_EFLAGS_IF */
        __asm__ volatile("sti");
    return 0;
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

    /* Initialize the cpu_id fields in the user-space rseq struct.
     * Userspace is expected to have zero-initialized the structure, but
     * we set cpu_id_start and cpu_id so they're correct immediately.
     * Done BEFORE committing the registration so a failed user write
     * (unmapped page, non-user address) leaves no half-registered
     * state behind. */
    {
        uint32_t cpu = (uint32_t)smp_get_cpu_id();
        if (rseq_user_access(proc, addr, &cpu, sizeof(cpu), 1) < 0)
            return -EFAULT;
        if (rseq_user_access(proc, addr + 4, &cpu, sizeof(cpu), 1) < 0)
            return -EFAULT;
    }

    state->rseq_addr = addr;
    state->rseq_len = len;
    state->rseq_sig = sig;
    state->registered = 1;
    state->last_cpu = smp_get_cpu_id();

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

    /* Write 0 to rseq_cs atomically (8 bytes) via validated user access:
     * the page may have been unmapped since registration — skip instead
     * of faulting the kernel. */
    uint64_t zero = 0;
    rseq_user_access(proc, addr, &zero, sizeof(zero), 1);
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
     * knows it was preempted and must retry.  Both writes go through
     * validated user access so an unmapped rseq page (munmap'd by the
     * task) can never fault the scheduler. */
    uint32_t val = cpu;
    __sync_synchronize();
    if (rseq_user_access(proc, addr, &val, sizeof(val), 1) < 0)
        return;
    __sync_synchronize();
    rseq_user_access(proc, addr + 4, &val, sizeof(val), 1);
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
     * Read the user-space rseq_cs pointer via validated user access —
     * the task may have unmapped the rseq page since registration,
     * which must not fault the scheduler. */
    uint64_t rseq_cs_addr = state->rseq_addr +
        __builtin_offsetof(struct rseq, rseq_cs);

    if (rseq_cs_addr >= USER_VADDR_MAX)
        return;

    uint64_t rseq_cs_val = 0;
    if (rseq_user_access(proc, rseq_cs_addr, &rseq_cs_val,
                         sizeof(rseq_cs_val), 0) < 0)
        return;

    if (rseq_cs_val != 0) {
        /* Process is in an rseq critical section — abort it.
         * Clear rseq_cs so the userspace retry loop restarts. */
        uint64_t zero = 0;
        rseq_user_access(proc, rseq_cs_addr, &zero, sizeof(zero), 1);

        /* ── Redirect instruction pointer to abort handler ──────────
         *
         * Per the rseq ABI, when a critical section is aborted the
         * kernel must redirect userspace execution to the abort
         * handler (abort_ip).  Without this, the task resumes at the
         * interrupted instruction inside the critical section and
         * executes the body using per-CPU data computed for the old
         * CPU, corrupting the old CPU's per-CPU data.
         *
         * We read the rseq_cs descriptor from userspace and compare
         * the saved RIP (from the interrupt frame saved when the
         * timer fired) against [start_ip, start_ip + post_commit_offset).
         * If RIP falls within that range, we set it to abort_ip so
         * userspace retries from the abort handler on the correct CPU. */
        struct interrupt_frame *frame = get_cpu_info()->current_frame;
        if (frame && (frame->cs & 3)) {
            /* Read the rseq_cs descriptor from userspace via validated
             * access: rseq_cs_val is fully user-controlled and may point
             * at an unmapped or non-user address.
             * The current task's page tables are still active because
             * schedule() calls us before switching to next's CR3. */
            struct rseq_cs cs;
            if (rseq_user_access(proc, rseq_cs_val, &cs, sizeof(cs), 0) == 0) {
                uint64_t start_ip = cs.start_ip;
                uint64_t end_ip   = cs.start_ip + cs.post_commit_offset;
                uint64_t abort_ip = cs.abort_ip;

                if (frame->rip >= start_ip && frame->rip < end_ip &&
                    abort_ip < USER_VADDR_MAX) {
                    frame->rip = abort_ip;
                }
            }
        }

        kprintf("[rseq] aborted critical section for pid=%d on migration "
                "cpu %d -> %d\n",
                proc->pid, old_cpu, new_cpu);
    }
}

/* ── Stub: rseq_signal ─────────────────────────────── */
static int rseq_signal(void *task, int sig)
{
    (void)task;
    (void)sig;
    kprintf("[rseq] rseq_signal: not yet implemented\n");
    return 0;
}
/* ── Stub: rseq_set_flags ─────────────────────────────── */
static int rseq_set_flags(void *task, uint32_t flags)
{
    (void)task;
    (void)flags;
    kprintf("[rseq] rseq_set_flags: not yet implemented\n");
    return 0;
}
/* ── Stub: rseq_get_flags ─────────────────────────────── */
static uint32_t rseq_get_flags(void *task)
{
    (void)task;
    kprintf("[rseq] rseq_get_flags: not yet implemented\n");
    return 0;
}
