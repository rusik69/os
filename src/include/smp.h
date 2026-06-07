#ifndef SMP_H
#define SMP_H

#include "types.h"
#include "process.h"
#include "scheduler.h"
#include "gdt.h"
#include "cpuidle.h"

/* Forward declaration for RPS per-CPU backlog pointer */
struct rps_backlog;

/* Maximum CPUs supported */
#define SMP_MAX_CPUS 16

/* Per-CPU information structure; accessed via GS.base */
struct cpu_info {
    uint32_t cpu_id;
    uint32_t apic_id;
    uint64_t stack_top;          /* top of per-CPU kernel stack */
    uint64_t kernel_stack;       /* bottom of per-CPU kernel stack */
    struct process *current_process;
    struct process *idle_process;

    /* Per-CPU scheduler queues (4 priority levels) */
    struct process *queue_head[SCHED_LEVELS];
    struct process *queue_tail[SCHED_LEVELS];
    int scheduler_enabled;
    uint64_t idle_ticks;

    /* Per-CPU TSS for IST stacks */
    struct tss tss;

    /* BSP sets up AP trampoline info here before SIPI */
    uint64_t ready_flag;         /* AP sets to 1 when fully initialized */

    /* CPU idle state data (cpuidle) */
    struct cpuidle_cpu idle_data;

    /* CFS minimum vruntime on this CPU's runqueue (for sleeper fairness) */
    uint64_t cfs_min_vruntime;

    /* ── Preemptible kernel state ────────────────────────────────── */
    int preempt_count;           /* > 0 = preemption disabled (nested) */
    volatile int need_resched;   /* non-zero = schedule() requested */

    /* ── RPS/RFS: Per-CPU packet backlog ─────────────────────────── */
    struct rps_backlog *rps_backlog;  /* allocated separately */

    uint8_t _pad[40];            /* cache line padding */
} __attribute__((aligned(64)));

/* Per-CPU accessors using GS segment */
static inline struct cpu_info *get_cpu_info(void) {
    struct cpu_info *info;
    /* GS.base = &cpu_info_array[cpu_id]. Read the base MSR directly
     * rather than the value at GS:0 (which would be the struct's first
     * 8 bytes, i.e. cpu_id + apic_id, not a useful pointer). */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    uint64_t base = ((uint64_t)hi << 32) | lo;
    info = (struct cpu_info *)base;
    return info;
}

static inline uint32_t get_cpu_id(void) {
    return get_cpu_info()->cpu_id;
}

static inline struct process *get_current_process(void) {
    return get_cpu_info()->current_process;
}

static inline void set_current_process(struct process *proc) {
    get_cpu_info()->current_process = proc;
}

/* Set GS.base to point to a per-CPU info struct */
void smp_set_gs_base(struct cpu_info *info);

/* Initialize per-CPU data for the BSP (CPU 0) */
void smp_init_bsp(void);

/* Boot all APs (application processors) */
int smp_boot_aps(void);

/* Check how many CPUs are online */
int smp_get_cpu_count(void);

/* ── CPU hotplug ──────────────────────────────────────────────────── */

/*
 * Gracefully take CPU @cpu_id offline.
 * Migrates all runnable tasks to other CPUs.
 * Returns 0 on success, negative error code on failure.
 * CPU 0 (BSP) cannot be offlined.
 */
int smp_cpu_disable(int cpu_id);

/*
 * Bring CPU @cpu_id back online.
 * The CPU must have been previously disabled via smp_cpu_disable().
 * Returns 0 on success, negative error code on failure.
 */
int smp_cpu_enable(int cpu_id);

/* Per-CPU info array (BSP writes AP info before SIPI) */
extern struct cpu_info cpu_info_array[SMP_MAX_CPUS];
extern int smp_cpu_count;

/* Get current CPU ID (inline for speed) */
static inline int smp_get_cpu_id(void) {
    struct cpu_info *info = get_cpu_info();
    return info ? info->cpu_id : 0;
}

/* SMP TLB shootdown: invalidate addresses on all CPUs */
void smp_tlb_shootdown(const uint64_t *addrs, int nr);

/* AP trampoline entry point (defined in ap_trampoline.asm) */
extern void ap_trampoline(void);
extern void ap_entry_64(void);

#endif
