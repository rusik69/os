#ifndef SMP_H
#define SMP_H

#include "types.h"
#include "process.h"
#include "scheduler.h"
#include "gdt.h"

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

    uint8_t _pad[64];            /* cache line padding */
} __attribute__((aligned(64)));

/* Per-CPU accessors using GS segment */
static inline struct cpu_info *get_cpu_info(void) {
    struct cpu_info *info;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(info));
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

/* Per-CPU info array (BSP writes AP info before SIPI) */
extern struct cpu_info cpu_info_array[SMP_MAX_CPUS];
extern int smp_cpu_count;

/* AP trampoline entry point (defined in ap_trampoline.asm) */
extern void ap_trampoline(void);
extern void ap_entry_64(void);

#endif
