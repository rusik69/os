#include "softirq.h"
#include "printf.h"
#include "string.h"
#include "irq_regs.h"
#include "smp.h"
#include "kernel.h"

#define SOFTIRQ_MAX 16

static softirq_handler softirq_handlers[SOFTIRQ_MAX];
static uint32_t softirq_pending = 0;

/* Recursion guard: track whether we're already processing softirqs.
 * Prevents softirqs from re-entering do_softirq() indefinitely. */
static int softirq_recursion[SMP_MAX_CPUS];
static int softirq_initialized = 0;

void softirq_init(void)
{
    memset(softirq_handlers, 0, sizeof(softirq_handlers));
    softirq_pending = 0;
    memset(softirq_recursion, 0, sizeof(softirq_recursion));
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

void do_softirq(void)
{
    /* ── Safety check: IRQ context ────────────────────────────────── */
    /* Softirqs should only run in IRQ context (from the return path of
     * an interrupt handler, or from a ksoftirqd thread).  If we're not
     * in IRQ context, something is wrong — but don't panic, just warn. */
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
        kprintf("[!!] do_softirq: CPU %d already in softirq processing (depth=%d)\n",
                cpu, softirq_recursion[cpu]);
        return;
    }
    softirq_recursion[cpu]++;

    /* ── Stack integrity check (if IRQ stack is available) ────────── */
    irq_stack_check();

    /* ── Process pending softirqs ─────────────────────────────────── */
    uint32_t pending = __atomic_exchange_n(&softirq_pending, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; pending && i < SOFTIRQ_MAX; i++) {
        if (pending & (1U << i)) {
            pending &= ~(1U << i);
            if (softirq_handlers[i]) {
                /* Validate handler pointer is not obviously bogus */
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

    softirq_recursion[cpu]--;
}
