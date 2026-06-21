/*
 * softirq.c — SoftIRQ subsystem with ksoftirqd
 *
 * SoftIRQs are a deferred interrupt processing mechanism.  Handlers run
 * with interrupts enabled but on the interrupted CPU.  To prevent IRQ
 * livelock (where network/block softirqs are re-raised faster than they
 * can be drained), processing is throttled: after SOFTIRQ_MAX_IRQ_PROCESS
 * consecutive iterations in IRQ context, the remainder is handed off to
 * the per-CPU ksoftirqd kernel thread which runs at SCHED_IDLE priority.
 */
#include "softirq.h"
#include "printf.h"
#include "string.h"
#include "irq_regs.h"
#include "smp.h"
#include "kernel.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"

#define SOFTIRQ_MAX 16

static softirq_handler softirq_handlers[SOFTIRQ_MAX];
static uint32_t softirq_pending = 0;

/* Recursion guard: track whether we're already processing softirqs.
 * Prevents softirqs from re-entering do_softirq() indefinitely. */
static int softirq_recursion[SMP_MAX_CPUS];

/* ── ksoftirqd state ───────────────────────────────────────────────────── */

/* Per-CPU ksoftirqd process pointer.  NULL before creation or if creation
 * failed on a given CPU. */
static struct process *ksoftirqd_proc[SMP_MAX_CPUS];

/* Per-CPU flag: non-zero when ksoftirqd should wake and process softirqs. */
static volatile int ksoftirqd_needed[SMP_MAX_CPUS];

/* Per-CPU spinlock protecting the wake handshake.  Only a simple atomic
 * store/load is needed in practice, but the lock prevents the compiler
 * from optimizing away the volatile load in the tight loop. */
static spinlock_t ksoftirqd_lock[SMP_MAX_CPUS];

static int softirq_initialized = 0;

/* ── Forward declarations ──────────────────────────────────────────────── */
static void ksoftirqd_task(void *arg);

void softirq_init(void)
{
    if (softirq_initialized)
        return;

    memset(softirq_handlers, 0, sizeof(softirq_handlers));
    softirq_pending = 0;
    memset(softirq_recursion, 0, sizeof(softirq_recursion));
    memset((void *)ksoftirqd_proc, 0, sizeof(ksoftirqd_proc));
    memset((void *)ksoftirqd_needed, 0, sizeof(ksoftirqd_needed));

    for (int i = 0; i < SMP_MAX_CPUS; i++)
        spinlock_init(&ksoftirqd_lock[i]);

    softirq_initialized = 1;
    kprintf("[OK] SoftIRQ subsystem initialized\n");
}

int softirq_register(int nr, softirq_handler handler)
{
    if (nr < 0 || nr >= SOFTIRQ_MAX || !handler)
        return -1;
    softirq_handlers[nr] = handler;
    return 0;
}

void softirq_raise(int nr)
{
    if (nr >= 0 && nr < SOFTIRQ_MAX)
        __atomic_fetch_or(&softirq_pending, 1U << nr, __ATOMIC_SEQ_CST);
}

/* ── ksoftirqd management ──────────────────────────────────────────────── */

/* Wake ksoftirqd on the current CPU.  Called when the IRQ-context
 * processing threshold is exceeded.  Safe to call from IRQ context. */
void softirq_wake_ksoftirqd(void)
{
    int cpu = 0;
    struct cpu_info *info = get_cpu_info();
    if (info)
        cpu = (int)info->cpu_id;
    if (cpu >= SMP_MAX_CPUS) cpu = 0;

    /* Set the wake flag.  ksoftirqd checks this in its loop. */
    __atomic_store_n(&ksoftirqd_needed[cpu], 1, __ATOMIC_SEQ_CST);
}

/* ksoftirqd per-CPU main loop.  Runs at SCHED_IDLE priority so it only
 * executes when nothing else needs the CPU.  Once woken, it drains any
 * pending softirqs and goes back to sleep (spins with scheduler_yield). */
static void ksoftirqd_task(void *arg)
{
    (void)arg;
    int cpu = 0;
    struct cpu_info *info = get_cpu_info();
    if (info)
        cpu = (int)info->cpu_id;
    if (cpu >= SMP_MAX_CPUS) cpu = 0;

    for (;;) {
        /* Spin-wait until we're needed (with yield so we don't burn CPU). */
        while (!__atomic_load_n(&ksoftirqd_needed[cpu], __ATOMIC_SEQ_CST)) {
            scheduler_yield();
        }

        /* Clear the wake flag before processing so that softirqs raised
         * during processing will set it again.  Use an atomic exchange
         * to avoid missing a concurrent raise. */
        (void)__atomic_exchange_n(&ksoftirqd_needed[cpu], 0, __ATOMIC_SEQ_CST);

        /* Drain pending softirqs.  We process in a loop because new ones
         * may be raised by the handlers themselves (e.g. NET_RX re-arming
         * the NAPI poll).  Limit to a reasonable number of iterations to
         * prevent ksoftirqd from monopolising the CPU entirely. */
        for (int iter = 0; iter < 8; iter++) {
            uint32_t pending = __atomic_exchange_n(&softirq_pending, 0,
                                                    __ATOMIC_SEQ_CST);
            if (!pending)
                break;

            for (int i = 0; pending && i < SOFTIRQ_MAX; i++) {
                if (pending & (1U << i)) {
                    pending &= ~(1U << i);
                    if (softirq_handlers[i]) {
                        /* Validate handler pointer is not obviously bogus */
                        if ((uint64_t)softirq_handlers[i] < 0xFFFF800000000000ULL ||
                            (uint64_t)softirq_handlers[i] > 0xFFFFFFFFFFFFFFFFULL) {
                            kprintf("[!!] ksoftirqd: CPU %d bogus handler %d at 0x%llx\n",
                                    cpu, i,
                                    (unsigned long long)(uint64_t)softirq_handlers[i]);
                            continue;
                        }
                        softirq_handlers[i]();
                    }
                }
            }
        }

        /* Yield after processing so other tasks get a chance to run. */
        scheduler_yield();
    }
}

/* Create ksoftirqd threads on all online CPUs.  Called after the scheduler
 * is initialized and SMP is online. */
void create_ksoftirqd(void)
{
    static char ksoftirqd_name[SMP_MAX_CPUS][24];
    int cpu_count = smp_get_cpu_count();
    if (cpu_count < 1) cpu_count = 1;
    if (cpu_count > SMP_MAX_CPUS) cpu_count = SMP_MAX_CPUS;

    for (int cpu = 0; cpu < cpu_count; cpu++) {
        if (ksoftirqd_proc[cpu])
            continue;  /* already created */

        /* Build a static name buffer that outlives this function.
         * process_create stores the name pointer directly, so we cannot
         * use a stack-local buffer (it would dangle after we return). */
        snprintf(ksoftirqd_name[cpu], sizeof(ksoftirqd_name[cpu]),
                 "ksoftirqd/%d", cpu);

        struct process *p = kthread_create_on_cpu(ksoftirqd_task, NULL,
                                                   ksoftirqd_name[cpu], cpu);
        if (!p) {
            p = kthread_create(ksoftirqd_task, NULL, ksoftirqd_name[cpu]);
        }

        if (p) {
            /* Move to the SCHED_IDLE queue level.  scheduler_add inside
             * process_create already put us on level 1 (SCHED_OTHER).
             * Remove, set policy, and re-add so we land on level 3. */
            scheduler_remove(p);
            p->sched_policy = SCHED_IDLE;
            scheduler_add(p);
            ksoftirqd_proc[cpu] = p;
            kprintf("[OK] ksoftirqd/%d created (PID %lu, SCHED_IDLE)\n",
                    cpu, (unsigned long)p->pid);
        } else {
            kprintf("[!!] ksoftirqd/%d: failed to create thread\n", cpu);
        }
    }
}

/* ── SoftIRQ processing ────────────────────────────────────────────────── */

void do_softirq(void)
{
    if (!softirq_initialized)
        return;

    int cpu = 0;
    {
        struct cpu_info *info = get_cpu_info();
        if (info)
            cpu = (int)info->cpu_id;
        if (cpu >= SMP_MAX_CPUS) cpu = 0;
    }

    /* ── Recursion guard ──────────────────────────────────────────── */
    if (softirq_recursion[cpu] > 0) {
        /* Already in softirq processing on this CPU — return immediately.
         * The outer invocation will handle any newly-pending softirqs. */
        return;
    }
    softirq_recursion[cpu]++;

    /* ── Stack integrity check (if IRQ stack is available) ────────── */
    irq_stack_check();

    /* ── Process pending softirqs with threshold ──────────────────── */
    /* We process up to SOFTIRQ_MAX_IRQ_PROCESS batches in IRQ context.
     * If more work remains, we set the ksoftirqd wake flag so the
     * per-CPU thread picks up the slack.  This prevents IRQ livelock. */
    int irq_batches = 0;
    int deferred = 0;

    for (;;) {
        uint32_t pending = __atomic_exchange_n(&softirq_pending, 0,
                                                __ATOMIC_SEQ_CST);
        if (!pending)
            break;

        /* Check if we've exceeded the IRQ-context processing budget */
        if (irq_batches >= SOFTIRQ_MAX_IRQ_PROCESS) {
            /* Put the pending bits back and flag ksoftirqd */
            __atomic_fetch_or(&softirq_pending, pending, __ATOMIC_SEQ_CST);
            deferred = 1;
            break;
        }
        irq_batches++;

        for (int i = 0; pending && i < SOFTIRQ_MAX; i++) {
            if (pending & (1U << i)) {
                pending &= ~(1U << i);
                if (softirq_handlers[i]) {
                    if ((uint64_t)softirq_handlers[i] < 0xFFFF800000000000ULL ||
                        (uint64_t)softirq_handlers[i] > 0xFFFFFFFFFFFFFFFFULL) {
                        kprintf("[!!] do_softirq: CPU %d bogus handler %d at 0x%llx\n",
                                cpu, i,
                                (unsigned long long)(uint64_t)softirq_handlers[i]);
                        continue;
                    }
                    softirq_handlers[i]();
                }
            }
        }
    }

    if (deferred) {
        /* Wake ksoftirqd to process the remaining softirqs.
         * This is intentionally outside the IRQ-context fast path:
         * we've already done our budget of work; the rest is handled
         * by the ksoftirqd thread at SCHED_IDLE priority. */
        softirq_wake_ksoftirqd();
    }

    softirq_recursion[cpu]--;
}

/* ── softirq_handle: Handle pending softirqs in process context ────── */
int softirq_handle(void)
{
    /* Forward to the existing do_softirq implementation */
    do_softirq();
    return 0;
}
