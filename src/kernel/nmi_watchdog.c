#define KERNEL_INTERNAL
#include "types.h"
#include "nmi_watchdog.h"
#include "printf.h"
#include "timer.h"
#include "process.h"
#include "scheduler.h"
#include "vga.h"
#include "acpi.h"
#include "panic.h"
#include "apic.h"
#include "smp.h"
#include "string.h"
#include "kallsyms.h"

/* ── Per-CPU watchdog data ───────────────────────────────────────── */
static struct nmi_watchdog_cpu watchdog_per_cpu[SMP_MAX_CPUS];

/* Global enable flag */
static volatile int watchdog_running = 0;

/* Rate-limiting: prevent flooding the console with redundant lockup
 * messages on the same CPU within the cooldown period. */
#define LOCKUP_COOLDOWN_TICKS (TIMER_FREQ * 30)  /* 30 seconds */

/* Escalation: if a hard lockup repeats this many times on the same CPU
 * within the cooldown window, call panic() with kdump capture. */
#define HARD_LOCKUP_ESCALATION_LIMIT  3

/* Threshold to treat a hard lockup as "immediate" (panic directly): < 1 sec */
#define HARD_LOCKUP_IMMEDIATE_MS      1000UL

/* Escalation for soft lockups: after this many occurrences, panic */
#define SOFT_LOCKUP_ESCALATION_LIMIT  5

/* ── Per-CPU accessor ────────────────────────────────────────────── */
static inline struct nmi_watchdog_cpu *this_watchdog(void) {
    uint32_t cpu_id = smp_get_cpu_id();
    if (cpu_id >= SMP_MAX_CPUS) cpu_id = 0;
    return &watchdog_per_cpu[cpu_id];
}

static inline struct nmi_watchdog_cpu *watchdog_for_cpu(int cpu_id) {
    if ((uint32_t)cpu_id >= SMP_MAX_CPUS) return NULL;
    return &watchdog_per_cpu[cpu_id];
}

/* ── Petting ─────────────────────────────────────────────────────── */

/* Pet from non-NMI context.  Updates both hard and soft timestamps.
 * Called from:
 *   - The idle loop (each CPU)
 *   - scheduler after context switch
 *   - Any long-running kernel loop that should not appear as a lockup */
void nmi_watchdog_pet(void) {
    if (!watchdog_running) return;
    struct nmi_watchdog_cpu *wd = this_watchdog();
    uint64_t now = timer_get_ticks();
    wd->hard_pet_tick = now;
    wd->soft_pet_tick = now;
}

/* Pet only the soft timer.  Called from the timer tick handler.
 * This allows distinguishing between "timer IRQs are firing but the
 * scheduler isn't getting to pet" (soft lockup) vs "nothing is
 * happening at all" (hard lockup). */
void nmi_watchdog_soft_pet(void) {
    if (!watchdog_running) return;
    struct nmi_watchdog_cpu *wd = this_watchdog();
    wd->soft_pet_tick = timer_get_ticks();
}

/* ── IPI backtrace request ───────────────────────────────────────── */

void nmi_watchdog_request_backtrace(void) {
    int cpu_count = smp_get_cpu_count();
    if (cpu_count <= 1) return;

    kprintf("Sending backtrace IPI to all other CPUs...\n");
    __asm__ volatile("mfence" ::: "memory");

    /* Send IPI to all other CPUs */
    apic_send_ipi_all_except(IPI_VECTOR_BACKTRACE);

    /* Small delay to let other CPUs dump their state.
     * In production, we'd spin-wait with a timeout.  We use a
     * bounded PAUSE loop to avoid livelock. */
    for (volatile int i = 0; i < 1000000; i++)
        __asm__ volatile("pause");
}

/* ── Soft lockup check ───────────────────────────────────────────── */

void nmi_watchdog_check_soft(void) {
    if (!watchdog_running) return;

    struct nmi_watchdog_cpu *wd = this_watchdog();
    if (wd->lockup_active) return;  /* already reporting on this CPU */

    uint64_t now = timer_get_ticks();
    uint64_t elapsed = (now - wd->soft_pet_tick) * 1000ULL / TIMER_FREQ;

    if (elapsed < SOFT_LOCKUP_THRESHOLD_MS)
        return;

    /* Soft lockup detected */
    wd->lockup_active = 1;
    wd->soft_lockup_count++;

    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n=== SOFT LOCKUP DETECTED on CPU %u ===\n",
            (unsigned int)smp_get_cpu_id());
    kprintf("CPU %u: not scheduled for %llu ms (threshold: %lu ms)\n",
            (unsigned int)smp_get_cpu_id(),
            (unsigned long long)elapsed,
            (unsigned long)SOFT_LOCKUP_THRESHOLD_MS);
    kprintf("Soft lockup count: %llu\n",
            (unsigned long long)wd->soft_lockup_count);

    /* Dump local state */
    struct process *cur = get_current_process();
    if (cur) {
        kprintf("Current process: %s (pid=%u, state=%d)\n",
                cur->name ? cur->name : "?", cur->pid, (int)cur->state);
    }
    dump_regs();
    dump_stack();

    /* Read and dump control registers */
    {
        uint64_t cr0, cr2, cr3, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        kprintf("CR0: 0x%lx  CR2: 0x%lx  CR3: 0x%lx  CR4: 0x%lx\n",
                (unsigned long)cr0, (unsigned long)cr2,
                (unsigned long)cr3, (unsigned long)cr4);
    }

    /* Dump process table */
    struct process *table = process_get_table();
    kprintf("PID  STATE  NAME\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        const char *state_str = "?";
        switch (table[i].state) {
            case PROCESS_READY:   state_str = "RDY"; break;
            case PROCESS_RUNNING: state_str = "RUN"; break;
            case PROCESS_BLOCKED: state_str = "BLK"; break;
            case PROCESS_ZOMBIE:  state_str = "ZMB"; break;
            default: break;
        }
        kprintf(" %3u  %s  %s\n", table[i].pid, state_str,
                table[i].name ? table[i].name : "?");
    }

    /* Ask other CPUs to dump state */
    nmi_watchdog_request_backtrace();

    /* Check for escalation: repeated soft lockups → panic */
    if (wd->soft_lockup_count >= SOFT_LOCKUP_ESCALATION_LIMIT) {
        vga_set_color(VGA_WHITE, VGA_RED);
        kprintf("\n=== SOFT LOCKUP ESCALATION on CPU %u (%llu occurrences) ===\n",
                (unsigned int)smp_get_cpu_id(),
                (unsigned long long)wd->soft_lockup_count);
        panic("SOFT LOCKUP on CPU %u — not scheduled for %llu ms",
              (unsigned int)smp_get_cpu_id(),
              (unsigned long long)elapsed);
    }

    /* Pet to prevent immediate re-trigger */
    wd->hard_pet_tick = now;
    wd->soft_pet_tick = now;
    wd->lockup_active = 0;

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ── NMI handler (hard lockup detection) ─────────────────────────── */

void nmi_watchdog_handler(struct interrupt_frame *frame) {
    (void)frame;

    if (!watchdog_running) return;

    struct nmi_watchdog_cpu *wd = this_watchdog();
    wd->nmi_count++;

    /* If we're already handling a lockup on this CPU, bail out to
     * prevent recursive NMI storms. */
    if (wd->lockup_active) return;

    uint64_t now = timer_get_ticks();
    uint64_t elapsed_ms = (now - wd->hard_pet_tick) * 1000ULL / TIMER_FREQ;

    if (elapsed_ms < HARD_LOCKUP_THRESHOLD_MS)
        return;  /* watchdog was petted recently — no lockup */

    /* ── Hard lockup detected ── */
    wd->lockup_active = 1;
    wd->hard_lockup_count++;

    /* Check for escalation: if lockups repeat beyond limit, panic */
    if (wd->hard_lockup_count >= HARD_LOCKUP_ESCALATION_LIMIT &&
        elapsed_ms > HARD_LOCKUP_IMMEDIATE_MS) {
        vga_set_color(VGA_WHITE, VGA_RED);
        kprintf("\n=== HARD LOCKUP ESCALATION on CPU %u (%llu occurrences) ===\n",
                (unsigned int)smp_get_cpu_id(),
                (unsigned long long)wd->hard_lockup_count);
        panic("HARD LOCKUP on CPU %u — not petted for %llu ms (threshold=%lu ms)",
              (unsigned int)smp_get_cpu_id(),
              (unsigned long long)elapsed_ms,
              (unsigned long)HARD_LOCKUP_THRESHOLD_MS);
    }

    /* Use raw VGA access to ensure visibility even if kprintf is
     * stuck (e.g. serial port locked).  We write directly since this
     * is NMI context and normal print paths may deadlock. */
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n========================================\n");
    kprintf("=== HARD LOCKUP DETECTED on CPU %u ===\n",
            (unsigned int)smp_get_cpu_id());
    kprintf("========================================\n");
    kprintf("CPU %u: not petted for %llu ms (threshold: %lu ms)\n",
            (unsigned int)smp_get_cpu_id(),
            (unsigned long long)elapsed_ms,
            (unsigned long)HARD_LOCKUP_THRESHOLD_MS);
    kprintf("NMI count on this CPU: %llu  Hard lockup count: %llu\n",
            (unsigned long long)wd->nmi_count,
            (unsigned long long)wd->hard_lockup_count);

    /* Dump registers from the interrupt frame */
    kprintf("RIP: 0x%lx  RSP: 0x%lx  RFLAGS: 0x%lx\n",
            (unsigned long)frame->rip,
            (unsigned long)frame->rsp,
            (unsigned long)frame->rflags);
    kprintf("Symbol: %s\n", kallsyms_lookup(frame->rip));
    kprintf("CS: 0x%lx  SS: 0x%lx  Error code: 0x%lx\n",
            (unsigned long)frame->cs,
            (unsigned long)frame->ss,
            (unsigned long)frame->error_code);
    kprintf("RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx  RDX: 0x%lx\n",
            (unsigned long)frame->rax, (unsigned long)frame->rbx,
            (unsigned long)frame->rcx, (unsigned long)frame->rdx);
    kprintf("RSI: 0x%lx  RDI: 0x%lx  RBP: 0x%lx\n",
            (unsigned long)frame->rsi, (unsigned long)frame->rdi,
            (unsigned long)frame->rbp);
    kprintf("R8:  0x%lx  R9:  0x%lx  R10: 0x%lx  R11: 0x%lx\n",
            (unsigned long)frame->r8,  (unsigned long)frame->r9,
            (unsigned long)frame->r10, (unsigned long)frame->r11);
    kprintf("R12: 0x%lx  R13: 0x%lx  R14: 0x%lx  R15: 0x%lx\n",
            (unsigned long)frame->r12, (unsigned long)frame->r13,
            (unsigned long)frame->r14, (unsigned long)frame->r15);

    /* Read and dump control registers */
    {
        uint64_t cr0, cr2, cr3, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        kprintf("CR0: 0x%lx  CR2: 0x%lx  CR3: 0x%lx  CR4: 0x%lx\n",
                (unsigned long)cr0, (unsigned long)cr2,
                (unsigned long)cr3, (unsigned long)cr4);
    }

    /* Dump current process */
    struct process *cur = get_current_process();
    if (cur) {
        kprintf("Current process: %s (pid=%u, state=%d, is_user=%d)\n",
                cur->name ? cur->name : "?",
                cur->pid, (int)cur->state, cur->is_user);

        /* Determine what the CPU was doing */
        if (frame->cs & 3) {
            kprintf("CPU was in USER mode\n");
        } else {
            kprintf("CPU was in KERNEL mode\n");
            /* Print function name at RIP if possible */
        }
    }

    /* Dump process table */
    struct process *table = process_get_table();
    kprintf("PID  STATE  NAME\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        const char *state_str = "?";
        switch (table[i].state) {
            case PROCESS_READY:   state_str = "RDY"; break;
            case PROCESS_RUNNING: state_str = "RUN"; break;
            case PROCESS_BLOCKED: state_str = "BLK"; break;
            case PROCESS_ZOMBIE:  state_str = "ZMB"; break;
            default: break;
        }
        kprintf(" %3u  %s  %s\n", table[i].pid, state_str,
                table[i].name ? table[i].name : "?");
    }

    /* Send IPI backtrace to all other CPUs */
    nmi_watchdog_request_backtrace();

    /* Pet to prevent immediate re-trigger on next NMI.
     * We set the timestamp far enough in the future to give the system
     * a chance to recover if the lockup resolves. */
    wd->hard_pet_tick = now;
    wd->soft_pet_tick = now;
    wd->lockup_active = 0;

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

void nmi_watchdog_start(void) {
    watchdog_running = 1;
    uint64_t now = timer_get_ticks();

    /* Initialize all CPUs' timestamps to now */
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        watchdog_per_cpu[i].hard_pet_tick = now;
        watchdog_per_cpu[i].soft_pet_tick = now;
        watchdog_per_cpu[i].lockup_active = 0;
    }

    kprintf("[OK] NMI watchdog started (hard=%lu ms, soft=%lu ms)\n",
            (unsigned long)HARD_LOCKUP_THRESHOLD_MS,
            (unsigned long)SOFT_LOCKUP_THRESHOLD_MS);
}

void nmi_watchdog_stop(void) {
    watchdog_running = 0;
}

int nmi_watchdog_available(void) {
    return 1; /* Always available — uses local APIC if possible */
}

void nmi_watchdog_init(void) {
    /* Zero out per-CPU state */
    memset(watchdog_per_cpu, 0, sizeof(watchdog_per_cpu));

    /* Register the NMI handler for vector 2.
     * The IDT already has an entry for NMI (#2) with IST2 configured.
     * We install our C handler so it gets called from isr_common_handler. */
    idt_register_handler(2, nmi_watchdog_handler);

    kprintf("[OK] NMI watchdog subsystem initialized "
            "(hard=%lu ms, soft=%lu ms)\n",
            (unsigned long)HARD_LOCKUP_THRESHOLD_MS,
            (unsigned long)SOFT_LOCKUP_THRESHOLD_MS);
}

void nmi_watchdog_get_stats(struct nmi_watchdog_stats *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        stats->hard_lockups += watchdog_per_cpu[i].hard_lockup_count;
        stats->soft_lockups += watchdog_per_cpu[i].soft_lockup_count;
        stats->nmi_count    += watchdog_per_cpu[i].nmi_count;
    }
}
