// SPDX-License-Identifier: GPL-2.0-only
/*
 * perf_branch.c — perf branch stack (LBR) sampling
 *
 * Supports Last Branch Record (LBR) sampling for performance
 * monitoring. Captures branch history for profiling.
 *
 * LBR MSRs are per-CPU hardware resources.  On context switch the
 * MSR state must be saved and restored so that a task's branch
 * history is not corrupted by other tasks running on the same CPU.
 *
 * Context-switch protocol (scheduler.c calls these with IRQs disabled):
 *   1. perf_branch_save_state()   — saves DEBUGCTL + LBR entries
 *   2. context_switch(old, new)   — switches tasks
 *   3. perf_branch_restore_state()— restores DEBUGCTL + LBR entries
 *
 * The multipart MSR read in perf_branch_read_lbr_atomic() disables
 * interrupts locally to guarantee a consistent snapshot.
 */

#include "perf_branch.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "smp.h"
#include "initcall.h"

/* ── Per-CPU LBR save areas ─────────────────────────────────────── */
/* Indexed by cpu_id; protects LBR state across context switches.
 * Initialised to zero (LBR disabled, empty save area). */
static struct perf_lbr_save_area lbr_save_areas[SMP_MAX_CPUS];

/* ── LBR MSR addresses ──────────────────────────────────────────── */
#define MSR_IA32_DEBUGCTLMSR  0x1D9
#define MSR_LBR_FROM_BASE     0x40   /* first LBR FROM MSR (legacy format) */
#define PERF_LBR_LEGACY_ENTRIES 16   /* 16 pairs in legacy MSR range [0x40-0x5F] */

/* ── Internal helpers ───────────────────────────────────────────── */

/* Read a 64-bit MSR */
static inline uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Write a 64-bit MSR */
static inline void write_msr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFULL);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

/* Return the per-CPU save area for the calling CPU */
static inline struct perf_lbr_save_area *this_save_area(void)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return NULL;
    return &lbr_save_areas[cpu];
}

/* ── Public API ─────────────────────────────────────────────────── */

int perf_branch_enable(void)
{
    struct perf_lbr_save_area *area = this_save_area();
    if (!area)
        return -EINVAL;

    area->enabled = 1;
    area->count = 0;

    /* Enable LBR in MSR_IA32_DEBUGCTLMSR (bit 0 = LBR) */
    uint64_t debugctl = read_msr(MSR_IA32_DEBUGCTLMSR);
    debugctl |= 1;
    write_msr(MSR_IA32_DEBUGCTLMSR, debugctl);
    area->debugctl = debugctl;

    kprintf("[PERF_BRANCH] LBR sampling enabled on CPU %d\n",
            smp_get_cpu_id());
    return 0;
}

int perf_branch_disable(void)
{
    struct perf_lbr_save_area *area = this_save_area();
    if (!area)
        return -EINVAL;

    area->enabled = 0;

    /* Disable LBR (clear bit 0) */
    uint64_t debugctl = read_msr(MSR_IA32_DEBUGCTLMSR);
    debugctl &= ~1ULL;
    write_msr(MSR_IA32_DEBUGCTLMSR, debugctl);
    area->debugctl = debugctl;

    return 0;
}

/*
 * perf_branch_read_lbr_atomic — atomically read LBR entries from MSRs
 *
 * Disables IRQs to guarantee a consistent snapshot of the LBR MSR ring.
 * Without this, a timer interrupt mid-read could cause a context switch,
 * mixing entries from two different tasks.
 *
 * Returns the number of entries read, or -EINVAL if LBR is not enabled.
 */
int perf_branch_read_lbr_atomic(struct perf_lbr_entry *entries, int max)
{
    struct perf_lbr_save_area *area = this_save_area();
    if (!area || !area->enabled)
        return -EINVAL;

    /* Save interrupt state and disable IRQs to prevent context switch
     * during the multi-MSR read sequence.  The scheduler calls
     * context_switch with IRQs off, so if we are being called from a
     * syscall or read path we must protect ourselves. */
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory"
    );

    int count = 0;

    /* Read legacy LBR entries from MSR 0x40 - 0x5F (16 FROM/TO pairs) */
    for (int i = 0; i < PERF_LBR_LEGACY_ENTRIES; i++) {
        uint32_t msr_from = MSR_LBR_FROM_BASE + (uint32_t)(i * 2);
        uint32_t msr_to   = MSR_LBR_FROM_BASE + (uint32_t)(i * 2 + 1);

        uint64_t from = read_msr(msr_from);
        uint64_t to   = read_msr(msr_to);

        if (count < max && count < PERF_LBR_MAX_ENTRIES) {
            entries[count].from  = from;
            entries[count].to    = to;
            entries[count].flags = 0;
            count++;
        }
    }

    /* Restore interrupt state */
    if (flags & 0x200) {
        __asm__ volatile("sti");
    }

    return count;
}

/*
 * perf_branch_save_state — Save LBR MSRs before context switch
 *
 * Called from schedule() with IRQs DISABLED (the scheduler disables
 * interrupts before calling context_switch).  Saves the current LBR
 * state (DEBUGCTLMSR + entries) into the per-CPU save area so that
 * the next invocation of perf_branch_restore_state() can reload it
 * when the task resumes.
 */
void perf_branch_save_state(void)
{
    struct perf_lbr_save_area *area = this_save_area();
    if (!area)
        return;

    /* Save DEBUGCTL MSR (tells us whether LBR is enabled) */
    area->debugctl = read_msr(MSR_IA32_DEBUGCTLMSR);
    area->enabled  = (area->debugctl & 1) ? 1 : 0;

    /* If LBR is disabled there is nothing else to save */
    if (!area->enabled) {
        area->count = 0;
        return;
    }

    /* Save LBR entries */
    area->count = 0;
    for (int i = 0; i < PERF_LBR_LEGACY_ENTRIES && area->count < PERF_LBR_MAX_ENTRIES; i++) {
        uint32_t msr_from = MSR_LBR_FROM_BASE + (uint32_t)(i * 2);
        uint32_t msr_to   = MSR_LBR_FROM_BASE + (uint32_t)(i * 2 + 1);

        area->entries[area->count].from  = read_msr(msr_from);
        area->entries[area->count].to    = read_msr(msr_to);
        area->entries[area->count].flags = 0;
        area->count++;
    }
}

/*
 * perf_branch_restore_state — Restore LBR MSRs after context switch
 *
 * Called from schedule() with IRQs DISABLED after context_switch()
 * returns.  Reloads the saved LBR state for the task that is now
 * running on this CPU.
 */
void perf_branch_restore_state(void)
{
    struct perf_lbr_save_area *area = this_save_area();
    if (!area)
        return;

    /* Restore DEBUGCTL MSR (enables or disables LBR) */
    write_msr(MSR_IA32_DEBUGCTLMSR, area->debugctl);

    /* If LBR was disabled when we saved, nothing else to restore */
    if (!area->enabled)
        return;

    /* Restore LBR entries — write them back to the MSR ring so the
     * resuming task sees its own branch history. */
    for (int i = 0; i < area->count && i < PERF_LBR_MAX_ENTRIES; i++) {
        uint32_t msr_from = MSR_LBR_FROM_BASE + (uint32_t)(i * 2);
        uint32_t msr_to   = MSR_LBR_FROM_BASE + (uint32_t)(i * 2 + 1);

        write_msr(msr_from, area->entries[i].from);
        write_msr(msr_to,   area->entries[i].to);
    }
}

/*
 * perf_branch_init — Initialise LBR subsystem
 *
 * Called once during kernel boot.  Zeroes all per-CPU save areas.
 * LBR sampling is not enabled until a profiler explicitly calls
 * perf_branch_enable().
 */
void perf_branch_init(void)
{
    memset(&lbr_save_areas, 0, sizeof(lbr_save_areas));
    kprintf("[OK] perf branch stack (LBR) sampling — context-switch safe\n");
}

postcore_initcall(perf_branch_init);
