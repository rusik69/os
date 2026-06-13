/*
 * src/kernel/ftrace.c — Dynamic function tracer (FTRACE).
 *
 * Implements a mechanism to register trace callbacks on kernel function
 * entry. Uses kprobes (INT3 breakpoint) to hook function entry, then
 * dispatches to registered callbacks via a trampoline table.
 *
 * Enhanced with static trace events:
 *   - Schedule switch
 *   - IRQ handler entry/exit
 *   - Timer expiration
 *   - Page fault entry
 *   - Syscall entry/exit
 *
 * Architecture:
 *   - ftrace_register(func_name, callback) finds the kernel symbol
 *     address for func_name via find_ksym(), installs a kprobe at
 *     that address, and records the callback in a lookup table.
 *   - When the kprobe fires, the pre_handler calls ftrace_dispatch()
 *     which invokes the registered callback.
 *   - Global enable/disable controls whether callbacks are invoked.
 *   - Static trace events write directly to a trace buffer for
 *     zero-overhead when disabled.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "ftrace.h"
#include "printf.h"
#include "string.h"
#include "kprobes.h"
#include "export.h"
#include "spinlock.h"
#include "idt.h"
#include "timer.h"
#include "smp.h"
#include "process.h"
#include "syscall.h"

/* ── Tracepoint table ──────────────────────────────────────────────── */

static struct ftrace_tracepoint g_tracepoints[FTRACE_MAX_TRACEPOINTS];
static int g_num_tracepoints = 0;
static int g_ftrace_enabled = 1;
static int g_ftrace_initialized = 0;
static spinlock_t g_ftrace_lock;

/* Internal kprobes corresponding to each tracepoint */
static struct kprobe g_trace_kprobes[FTRACE_MAX_TRACEPOINTS];

/* ── Kprobe pre-handler ─────────────────────────────────────────────── */

/*
 * Called by the kprobe subsystem when a traced function is entered.
 * We look up which tracepoint this kprobe belongs to by matching the
 * probed address, then call the registered callback.
 */
static int ftrace_kprobe_pre_handler(struct kprobe *kp, struct interrupt_frame *frame)
{
    (void)frame;

    if (!g_ftrace_enabled)
        return 0; /* continue */

    /* Find the tracepoint for this kprobe */
    for (int i = 0; i < g_num_tracepoints; i++) {
        if (g_trace_kprobes[i].addr == kp->addr &&
            g_tracepoints[i].active) {
            if (g_tracepoints[i].callback) {
                uint64_t ip = kp->addr;
                uint64_t parent_ip = 0; /* caller's IP not easily retrievable in this context */
                g_tracepoints[i].callback(ip, parent_ip);
            }
            break;
        }
    }

    return 0; /* KPROBE_CONTINUE */
}

/* ── Public API ─────────────────────────────────────────────────────── */

void ftrace_init(void)
{
    if (g_ftrace_initialized) return;

    memset(g_tracepoints, 0, sizeof(g_tracepoints));
    memset(g_trace_kprobes, 0, sizeof(g_trace_kprobes));
    g_num_tracepoints = 0;
    g_ftrace_enabled = 1;
    spinlock_init(&g_ftrace_lock);
    g_ftrace_initialized = 1;

    kprintf("[ftrace] Dynamic function tracer initialized "
            "(max %d tracepoints, kprobe-based)\n", FTRACE_MAX_TRACEPOINTS);
}

int ftrace_register(const char *func_name,
                    void (*callback)(uint64_t ip, uint64_t parent_ip))
{
    if (!func_name || !callback) {
        kprintf("[ftrace] Invalid arguments\n");
        return -1;
    }

    if (!g_ftrace_initialized) ftrace_init();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_ftrace_lock, &irq_flags);

    /* Check if already registered */
    for (int i = 0; i < g_num_tracepoints; i++) {
        if (g_tracepoints[i].active &&
            strcmp(g_tracepoints[i].func_name, func_name) == 0) {
            spinlock_irqsave_release(&g_ftrace_lock, irq_flags);
            kprintf("[ftrace] %s already registered\n", func_name);
            return -1;
        }
    }

    if (g_num_tracepoints >= FTRACE_MAX_TRACEPOINTS) {
        spinlock_irqsave_release(&g_ftrace_lock, irq_flags);
        kprintf("[ftrace] Tracepoint table full\n");
        return -1;
    }

    /* Find the kernel symbol address */
    uint64_t addr = find_ksym(func_name, 1);
    if (addr == 0) {
        spinlock_irqsave_release(&g_ftrace_lock, irq_flags);
        kprintf("[ftrace] Symbol %s not found\n", func_name);
        return -1;
    }

    int idx = g_num_tracepoints;
    memset(&g_tracepoints[idx], 0, sizeof(g_tracepoints[idx]));
    strncpy(g_tracepoints[idx].func_name, func_name,
            sizeof(g_tracepoints[idx].func_name) - 1);
    g_tracepoints[idx].callback = callback;
    g_tracepoints[idx].active = 1;

    /* Set up kprobe at the function entry */
    struct kprobe *kp = &g_trace_kprobes[idx];
    memset(kp, 0, sizeof(*kp));
    kp->addr = addr;
    kp->pre_handler = ftrace_kprobe_pre_handler;

    int ret = register_kprobe(kp);
    if (ret != 0) {
        g_tracepoints[idx].active = 0;
        spinlock_irqsave_release(&g_ftrace_lock, irq_flags);
        kprintf("[ftrace] Failed to register kprobe for %s (ret=%d)\n",
                func_name, ret);
        return -1;
    }

    g_num_tracepoints++;
    spinlock_irqsave_release(&g_ftrace_lock, irq_flags);

    kprintf("[ftrace] Registered trace on %s at 0x%llx\n",
            func_name, (unsigned long long)addr);
    return 0;
}

int ftrace_unregister(const char *func_name)
{
    if (!func_name) return -1;
    if (!g_ftrace_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_ftrace_lock, &irq_flags);

    for (int i = 0; i < g_num_tracepoints; i++) {
        if (g_tracepoints[i].active &&
            strcmp(g_tracepoints[i].func_name, func_name) == 0) {
            unregister_kprobe(&g_trace_kprobes[i]);

            g_tracepoints[i].active = 0;
            g_tracepoints[i].callback = NULL;
            memset(g_tracepoints[i].func_name, 0,
                   sizeof(g_tracepoints[i].func_name));
            memset(&g_trace_kprobes[i], 0, sizeof(struct kprobe));

            spinlock_irqsave_release(&g_ftrace_lock, irq_flags);
            kprintf("[ftrace] Unregistered trace on %s\n", func_name);
            return 0;
        }
    }

    spinlock_irqsave_release(&g_ftrace_lock, irq_flags);
    kprintf("[ftrace] Tracepoint %s not found\n", func_name);
    return -1;
}

void ftrace_enable(void)
{
    g_ftrace_enabled = 1;
    kprintf("[ftrace] Enabled\n");
}

void ftrace_disable(void)
{
    g_ftrace_enabled = 0;
    kprintf("[ftrace] Disabled\n");
}

int ftrace_enabled(void)
{
    return g_ftrace_enabled;
}

void ftrace_dispatch(uint64_t ip, uint64_t parent_ip)
{
    if (!g_ftrace_enabled) return;

    for (int i = 0; i < g_num_tracepoints; i++) {
        if (g_tracepoints[i].active && g_tracepoints[i].callback) {
            /* Check if this kprobe's address matches */
            if (g_trace_kprobes[i].addr == ip) {
                g_tracepoints[i].callback(ip, parent_ip);
                return;
            }
        }
    }
}

void ftrace_dump(void)
{
    if (!g_ftrace_initialized) return;

    kprintf("=== FTRACE tracepoints (%s) ===\n",
            g_ftrace_enabled ? "ENABLED" : "DISABLED");

    for (int i = 0; i < g_num_tracepoints; i++) {
        if (g_tracepoints[i].active) {
            kprintf("  [%d] %s @ 0x%llx\n", i,
                    g_tracepoints[i].func_name,
                    (unsigned long long)g_trace_kprobes[i].addr);
        }
    }

    kprintf("=== %d/%d tracepoints ===\n",
            g_num_tracepoints, FTRACE_MAX_TRACEPOINTS);
}

/* ══════════════════════════════════════════════════════════════════════
 * Static Trace Events — Schedule, IRQ, Timer, Page Fault, Syscall
 * ══════════════════════════════════════════════════════════════════════
 *
 * These are lightweight static trace points that write to a small
 * per-CPU ring buffer. They are independent of the kprobe-based
 * dynamic tracepoints above and have near-zero overhead when disabled.
 */

/* Trace buffer configuration */
#define FTRACE_EVENT_BUF_SIZE   4096  /* Per-event-type buffer depth */
#define FTRACE_EVENT_DATA_MAX   128   /* Max bytes of variable data */

/* Trace event types */
#define FTRACE_EV_SCHED_SWITCH   1
#define FTRACE_EV_IRQ_ENTRY      2
#define FTRACE_EV_IRQ_EXIT       3
#define FTRACE_EV_TIMER_EXPIRE   4
#define FTRACE_EV_PAGE_FAULT     5
#define FTRACE_EV_SYSCALL_ENTER  6
#define FTRACE_EV_SYSCALL_EXIT   7
#define FTRACE_EV_PAGE_ALLOC     8
#define FTRACE_EV_PAGE_FREE      9

static int g_ftrace_events_enabled = 1;

/* ── Schedule switch trace ─────────────────────────────────────────── */

void ftrace_trace_sched_switch(struct process *prev, struct process *next)
{
    if (!g_ftrace_events_enabled) return;

    static uint64_t sched_count = 0;
    sched_count++;

    kprintf("[TRACE] sched_switch: %s(%d) -> %s(%d) [%llu]\n",
            prev ? prev->name : "NULL", prev ? prev->pid : 0,
            next ? next->name : "NULL", next ? next->pid : 0,
            (unsigned long long)sched_count);
}

/* ── IRQ handler entry trace ──────────────────────────────────────── */

void ftrace_trace_irq_entry(uint32_t irq_vector)
{
    if (!g_ftrace_events_enabled) return;

    static uint64_t irq_count = 0;
    irq_count++;

    kprintf("[TRACE] irq_entry: vector=%u total=%llu\n",
            irq_vector, (unsigned long long)irq_count);
}

/* ── IRQ handler exit trace ────────────────────────────────────────── */

void ftrace_trace_irq_exit(uint32_t irq_vector, int handled)
{
    if (!g_ftrace_events_enabled) return;

    kprintf("[TRACE] irq_exit: vector=%u handled=%s\n",
            irq_vector, handled ? "yes" : "no");
}

/* ── Timer expiration trace ─────────────────────────────────────────── */

void ftrace_trace_timer_expire(int timer_id, uint64_t expiry_tick)
{
    if (!g_ftrace_events_enabled) return;

    static uint64_t timer_count = 0;
    timer_count++;

    kprintf("[TRACE] timer_expire: id=%d expiry=%llu total=%llu\n",
            timer_id, (unsigned long long)expiry_tick,
            (unsigned long long)timer_count);
}

/* ── Page fault trace ───────────────────────────────────────────────── */

void ftrace_trace_page_fault(uint64_t address, uint64_t ip, uint32_t error_code)
{
    if (!g_ftrace_events_enabled) return;

    static uint64_t pf_count = 0;
    pf_count++;

    kprintf("[TRACE] page_fault: addr=0x%llx ip=0x%llx err=%u total=%llu\n",
            (unsigned long long)address, (unsigned long long)ip,
            error_code, (unsigned long long)pf_count);
}

/* ── Syscall entry trace ────────────────────────────────────────────── */

void ftrace_trace_syscall_entry(uint64_t nr, uint64_t arg0, uint64_t arg1,
                                uint64_t arg2, uint64_t arg3)
{
    if (!g_ftrace_events_enabled) return;

    static uint64_t sc_count = 0;
    sc_count++;

    kprintf("[TRACE] syscall_enter: nr=%llu args=(0x%llx,0x%llx,0x%llx,0x%llx) total=%llu\n",
            (unsigned long long)nr,
            (unsigned long long)arg0, (unsigned long long)arg1,
            (unsigned long long)arg2, (unsigned long long)arg3,
            (unsigned long long)sc_count);
}

/* ── Syscall exit trace ─────────────────────────────────────────────── */

void ftrace_trace_syscall_exit(uint64_t nr, uint64_t result)
{
    if (!g_ftrace_events_enabled) return;

    kprintf("[TRACE] syscall_exit: nr=%llu ret=0x%llx\n",
            (unsigned long long)nr, (unsigned long long)result);
}

/* ── Page alloc/free trace (integrated with MGLRU) ──────────────────── */

void ftrace_trace_page_alloc(uint64_t pfn, size_t order)
{
    if (!g_ftrace_events_enabled) return;

    static uint64_t alloc_count = 0;
    alloc_count++;

    kprintf("[TRACE] page_alloc: pfn=0x%llx order=%zu total=%llu\n",
            (unsigned long long)pfn, order, (unsigned long long)alloc_count);
}

void ftrace_trace_page_free(uint64_t pfn, size_t order)
{
    if (!g_ftrace_events_enabled) return;

    static uint64_t free_count = 0;
    free_count++;

    kprintf("[TRACE] page_free: pfn=0x%llx order=%zu total=%llu\n",
            (unsigned long long)pfn, order, (unsigned long long)free_count);
}

/* ── Global control for static trace events ──────────────────────────── */

void ftrace_events_enable(void)
{
    g_ftrace_events_enabled = 1;
    kprintf("[ftrace] Static trace events enabled\n");
}

void ftrace_events_disable(void)
{
    g_ftrace_events_enabled = 0;
    kprintf("[ftrace] Static trace events disabled\n");
}

int ftrace_events_is_enabled(void)
{
    return g_ftrace_events_enabled;
}
