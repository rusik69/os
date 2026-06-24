// SPDX-License-Identifier: GPL-2.0-only
/*
 * scs.c — Shadow Call Stack
 *
 * Provides a separate shadow stack for return addresses to protect
 * against ROP (Return-Oriented Programming) attacks.
 * Uses a per-CPU shadow stack region for kernel code.
 */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "pmm.h"
#include "smp.h"

#define SCS_SHIFT 10           /* 1024 entries per shadow stack */
#define SCS_SIZE  (1U << SCS_SHIFT)
#define SCS_ENTRY_SIZE sizeof(uintptr_t)
#define SCS_TOTAL (SCS_SIZE * SCS_ENTRY_SIZE)

/* Per-CPU shadow stack pointers */
static uintptr_t *scs_sp[SMP_MAX_CPUS];
static uintptr_t *scs_base[SMP_MAX_CPUS];
static int scs_initialized = 0;

/* Allocate shadow stack for a CPU */
static int scs_setup_cpu(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return -1;

    /* Allocate one page for shadow stack */
    uint64_t frame = pmm_alloc_frame();
    if (!frame)
        return -1;

    void *page = PHYS_TO_VIRT(frame << 12);
    scs_base[cpu] = (uintptr_t *)page;
    scs_sp[cpu] = scs_base[cpu] + SCS_SIZE; /* grows downward */

    return 0;
}

/* Push a return address onto the shadow stack */
int __attribute__((used)) scs_push(uintptr_t return_addr)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS || !scs_sp[cpu])
        return -1;

    /* Check for overflow (stack grows downward) */
    if (scs_sp[cpu] <= scs_base[cpu])
        return -1;

    scs_sp[cpu]--;
    *scs_sp[cpu] = return_addr;
    return 0;
}

/* Pop a return address from the shadow stack */
uintptr_t __attribute__((used)) scs_pop(void)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS || !scs_sp[cpu])
        return 0;

    /* Check for underflow */
    if (scs_sp[cpu] >= scs_base[cpu] + SCS_SIZE)
        return 0;

    uintptr_t addr = *scs_sp[cpu];
    scs_sp[cpu]++;
    return addr;
}

/* Verify a return address matches the shadow stack */
int scs_check_return(uintptr_t expected)
{
    uintptr_t actual = scs_pop();
    if (actual != expected) {
        struct process *cur = process_get_current();
        kprintf("[SCS] Return address mismatch! expected=0x%llx actual=0x%llx "
                "pid=%u %s\n",
                (unsigned long long)expected, (unsigned long long)actual,
                cur ? (unsigned int)cur->pid : 0,
                cur && cur->name ? cur->name : "?");
        return -1;
    }
    return 0;
}

/* Get current shadow stack pointer (for context switch) */
uintptr_t *scs_get_sp(void)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return NULL;
    return scs_sp[cpu];
}

/* Set shadow stack pointer (for context switch) */
void scs_set_sp(uintptr_t *sp)
{
    int cpu = smp_get_cpu_id();
    if (cpu >= 0 && cpu < SMP_MAX_CPUS)
        scs_sp[cpu] = sp;
}

void __init scs_init(void)
{
    if (scs_initialized)
        return;

    int ncpus = smp_get_cpu_count();
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;

    for (int i = 0; i < ncpus; i++) {
        if (scs_setup_cpu(i) < 0) {
            kprintf("[SCS] Failed to setup shadow stack for CPU %d\n", i);
            continue;
        }
    }

    scs_initialized = 1;
    kprintf("[OK] SCS — Shadow Call Stack (%d CPUs, %d entries each)\n",
            smp_get_cpu_count(), SCS_SIZE);
}

/* ── Stub: scs_alloc ─────────────────────────────── */
void* scs_alloc(void *task)
{
    (void)task;
    kprintf("[scs] scs_alloc: not yet implemented\n");
    return 0;
}
/* ── Stub: scs_free ─────────────────────────────── */
int scs_free(void *task)
{
    (void)task;
    kprintf("[scs] scs_free: not yet implemented\n");
    return 0;
}
/* ── Stub: scs_switch ─────────────────────────────── */
int scs_switch(void *task, void *new_scs)
{
    (void)task;
    (void)new_scs;
    kprintf("[scs] scs_switch: not yet implemented\n");
    return 0;
}
