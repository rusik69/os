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
#include "kallsyms.h"
#include "errno.h"

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

/* ══════════════════════════════════════════════════════════════════════
 * Function Graph Tracer
 * ══════════════════════════════════════════════════════════════════════
 *
 * Uses kretprobes to intercept both function entry and return, recording
 * call duration.  Maintains a tracking stack for nesting depth and a ring
 * buffer of completed [pid, func_addr, entry_ns, duration_ns] records.
 */

/* ── Tracking stack ────────────────────────────────────────────────── */

/* Per-CPU tracking stack for active (entered but not yet returned) calls.
 * We use a global array indexed by CPU ID for simplicity. */
#define FTRACE_GRAPH_STACK_SIZE 64
static struct {
    uint64_t func_addr;
    uint64_t entry_ns;
    uint32_t pid;
} g_graph_tracking[SMP_MAX_CPUS][FTRACE_GRAPH_STACK_SIZE];
static int g_graph_depth[SMP_MAX_CPUS];

/* ── Output ring buffer ────────────────────────────────────────────── */

static struct {
    struct ftrace_graph_entry entries[FTRACE_GRAPH_BUF_SIZE];
    volatile uint32_t write_idx;
    int initialized;
} g_graph_buf;

/* ─── Configuration state ──────────────────────────────────────────── */

static int g_graph_enabled = 0;
static int g_graph_max_depth = 0;       /* 0 = unlimited */
static int g_graph_tracer_mode = FTRACE_TRACER_NOP;
static int g_graph_num_funcs = 0;

/* Registered kretprobes for each graph-traced function */
static struct kretprobe g_graph_kretprobes[FTRACE_GRAPH_MAX_FUNCS];
static char g_graph_func_names[FTRACE_GRAPH_MAX_FUNCS][64];
static spinlock_t g_graph_lock;

/* ── Internal helpers ──────────────────────────────────────────────── */

/* Pre-handler: called on function ENTRY (via kprobe in kretprobe).
 * Records the entry timestamp in the tracking stack. */
static int ftrace_graph_pre_handler(struct kprobe *kp, struct interrupt_frame *frame)
{
    (void)frame;
    if (!g_graph_enabled)
        return 0;

    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return 0;

    /* Check max depth */
    if (g_graph_max_depth > 0 && g_graph_depth[cpu] >= g_graph_max_depth)
        return 0;

    /* Check stack overflow */
    if (g_graph_depth[cpu] >= FTRACE_GRAPH_STACK_SIZE)
        return 0;

    /* Find the function address from the kprobe */
    uint64_t func_addr = kp->addr;
    uint64_t now_ns = timer_get_ns();
    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    int depth = g_graph_depth[cpu];
    g_graph_tracking[cpu][depth].func_addr = func_addr;
    g_graph_tracking[cpu][depth].entry_ns = now_ns;
    g_graph_tracking[cpu][depth].pid = pid;
    g_graph_depth[cpu] = depth + 1;

    return 0;
}

/* Handler: called on function RETURN (via kretprobe trampoline).
 * Pops the matching entry from the tracking stack and writes to the
 * ring buffer. */
static void ftrace_graph_handler(struct kretprobe *rp, uint64_t return_value)
{
    (void)return_value;
    if (!g_graph_enabled)
        return;

    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;

    uint64_t func_addr = rp ? rp->addr : 0;
    if (func_addr == 0)
        return;

    uint64_t now_ns = timer_get_ns();

    /* Search the tracking stack (from the top down) for a matching entry */
    int depth = g_graph_depth[cpu];
    int found = -1;
    for (int i = depth - 1; i >= 0; i--) {
        if (g_graph_tracking[cpu][i].func_addr == func_addr) {
            found = i;
            break;
        }
    }

    if (found < 0)
        return;  /* No matching entry found */

    uint64_t entry_ns = g_graph_tracking[cpu][found].entry_ns;
    uint32_t pid = g_graph_tracking[cpu][found].pid;
    uint64_t duration_ns = (now_ns > entry_ns) ? (now_ns - entry_ns) : 0;

    /* Remove this entry from the tracking stack by shifting later entries down */
    for (int i = found; i < depth - 1; i++) {
        g_graph_tracking[cpu][i] = g_graph_tracking[cpu][i + 1];
    }
    g_graph_depth[cpu] = depth - 1;

    /* Write to the output ring buffer */
    if (!g_graph_buf.initialized)
        return;

    uint32_t idx = __sync_fetch_and_add(&g_graph_buf.write_idx, 1) % FTRACE_GRAPH_BUF_SIZE;
    struct ftrace_graph_entry *entry = &g_graph_buf.entries[idx];
    entry->func_addr = func_addr;
    entry->entry_ns = entry_ns;
    entry->duration_ns = duration_ns;
    entry->pid = pid;
    entry->depth = (uint32_t)(depth - 1);
}

/* ── Lookup helper: find kretprobe index by function name ──────────── */
static int ftrace_graph_find_func(const char *func_name)
{
    if (!func_name)
        return -1;
    for (int i = 0; i < g_graph_num_funcs; i++) {
        if (strcmp(g_graph_func_names[i], func_name) == 0)
            return i;
    }
    return -1;
}

/* ── Public API ────────────────────────────────────────────────────── */

void ftrace_graph_init(void)
{
    if (g_graph_buf.initialized)
        return;

    memset(&g_graph_buf, 0, sizeof(g_graph_buf));
    memset(g_graph_tracking, 0, sizeof(g_graph_tracking));
    memset(g_graph_depth, 0, sizeof(g_graph_depth));
    memset(g_graph_kretprobes, 0, sizeof(g_graph_kretprobes));
    memset(g_graph_func_names, 0, sizeof(g_graph_func_names));
    g_graph_buf.initialized = 1;
    g_graph_num_funcs = 0;
    g_graph_enabled = 0;
    g_graph_max_depth = 0;
    g_graph_tracer_mode = FTRACE_TRACER_NOP;
    spinlock_init(&g_graph_lock);

    kprintf("[ftrace] Function graph tracer initialized "
            "(buf=%d entries, max %d functions)\n",
            FTRACE_GRAPH_BUF_SIZE, FTRACE_GRAPH_MAX_FUNCS);
}

int ftrace_graph_register(const char *func_name)
{
    if (!func_name)
        return -EINVAL;

    if (!g_graph_buf.initialized)
        ftrace_graph_init();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_graph_lock, &irq_flags);

    /* Check if already registered */
    if (ftrace_graph_find_func(func_name) >= 0) {
        spinlock_irqsave_release(&g_graph_lock, irq_flags);
        return 0;  /* Already registered */
    }

    if (g_graph_num_funcs >= FTRACE_GRAPH_MAX_FUNCS) {
        spinlock_irqsave_release(&g_graph_lock, irq_flags);
        kprintf("[ftrace] Graph max functions (%d) reached\n", FTRACE_GRAPH_MAX_FUNCS);
        return -ENOMEM;
    }

    /* Find the kernel symbol address */
    uint64_t addr = find_ksym(func_name, 1);
    if (addr == 0) {
        spinlock_irqsave_release(&g_graph_lock, irq_flags);
        kprintf("[ftrace] Graph: symbol %s not found\n", func_name);
        return -ENOENT;
    }

    int idx = g_graph_num_funcs;

    /* Set up kretprobe */
    struct kretprobe *rp = &g_graph_kretprobes[idx];
    memset(rp, 0, sizeof(*rp));
    rp->addr = addr;
    rp->handler = ftrace_graph_handler;
    rp->maxactive = 4;  /* Allow some recursion */

    /* Set the pre_handler on the underlying kprobe for entry recording */
    rp->kp.pre_handler = ftrace_graph_pre_handler;

    int ret = register_kretprobe(rp);
    if (ret != 0) {
        spinlock_irqsave_release(&g_graph_lock, irq_flags);
        kprintf("[ftrace] Graph: register_kretprobe(%s) failed: %d\n",
                func_name, ret);
        return -ENOENT;
    }

    strncpy(g_graph_func_names[idx], func_name,
            sizeof(g_graph_func_names[idx]) - 1);
    g_graph_func_names[idx][sizeof(g_graph_func_names[idx]) - 1] = '\0';
    g_graph_num_funcs++;

    spinlock_irqsave_release(&g_graph_lock, irq_flags);

    kprintf("[ftrace] Graph: tracing %s @ 0x%llx\n",
            func_name, (unsigned long long)addr);
    return 0;
}

int ftrace_graph_unregister(const char *func_name)
{
    if (!func_name)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_graph_lock, &irq_flags);

    int idx = ftrace_graph_find_func(func_name);
    if (idx < 0) {
        spinlock_irqsave_release(&g_graph_lock, irq_flags);
        return -ENOENT;
    }

    /* Unregister the kretprobe */
    unregister_kretprobe(&g_graph_kretprobes[idx]);

    /* Remove from the list by shifting */
    memset(&g_graph_kretprobes[idx], 0, sizeof(struct kretprobe));
    memset(g_graph_func_names[idx], 0, sizeof(g_graph_func_names[idx]));

    for (int i = idx; i < g_graph_num_funcs - 1; i++) {
        g_graph_kretprobes[i] = g_graph_kretprobes[i + 1];
        memcpy(g_graph_func_names[i], g_graph_func_names[i + 1],
               sizeof(g_graph_func_names[i]));
    }
    g_graph_num_funcs--;

    spinlock_irqsave_release(&g_graph_lock, irq_flags);

    kprintf("[ftrace] Graph: stopped tracing %s\n", func_name);
    return 0;
}

void ftrace_graph_enable(void)
{
    g_graph_enabled = 1;
    kprintf("[ftrace] Graph tracer enabled\n");
}

void ftrace_graph_disable(void)
{
    g_graph_enabled = 0;
    kprintf("[ftrace] Graph tracer disabled\n");
}

int ftrace_graph_is_enabled(void)
{
    return g_graph_enabled && g_graph_buf.initialized;
}

int ftrace_set_tracer(int mode)
{
    switch (mode) {
    case FTRACE_TRACER_NOP:
        ftrace_graph_disable();
        ftrace_disable();
        g_graph_tracer_mode = FTRACE_TRACER_NOP;
        return 0;
    case FTRACE_TRACER_FUNCTION:
        ftrace_graph_disable();
        ftrace_enable();
        g_graph_tracer_mode = FTRACE_TRACER_FUNCTION;
        return 0;
    case FTRACE_TRACER_FUNCTION_GRAPH:
        ftrace_disable();
        ftrace_graph_enable();
        g_graph_tracer_mode = FTRACE_TRACER_FUNCTION_GRAPH;
        return 0;
    default:
        return -EINVAL;
    }
}

int ftrace_get_tracer(void)
{
    return g_graph_tracer_mode;
}

void ftrace_graph_set_max_depth(int depth)
{
    if (depth < 0)
        depth = 0;
    g_graph_max_depth = depth;
}

int ftrace_graph_get_max_depth(void)
{
    return g_graph_max_depth;
}

int ftrace_graph_read_trace(char *buf, int buf_size)
{
    if (!buf || buf_size <= 0)
        return -EINVAL;

    if (!g_graph_buf.initialized) {
        if (buf_size > 0) buf[0] = '\0';
        return 0;
    }

    int total = 0;
    uint32_t count = g_graph_buf.write_idx;
    uint32_t n = (count < FTRACE_GRAPH_BUF_SIZE) ? count : FTRACE_GRAPH_BUF_SIZE;

    if (n == 0) {
        if (buf_size > 0)
            buf[0] = '\0';
        return 0;
    }

    /* Walk backwards (most recent entries first) */
    uint32_t start = (count > 0) ? (count - 1) % FTRACE_GRAPH_BUF_SIZE : 0;

    for (uint32_t i = 0; i < n && total < buf_size - 1; i++) {
        uint32_t idx = (start + FTRACE_GRAPH_BUF_SIZE - i) % FTRACE_GRAPH_BUF_SIZE;
        struct ftrace_graph_entry *e = &g_graph_buf.entries[idx];

        /* Look up the symbol name for func_addr */
        const char *sym = kallsyms_lookup(e->func_addr);
        if (!sym)
            sym = "???";

        int written = snprintf(buf + total, (size_t)(buf_size - total),
                               "%5u) %-40s %llu ns (entry=%llu)\n",
                               (unsigned int)e->pid,
                               sym,
                               (unsigned long long)e->duration_ns,
                               (unsigned long long)e->entry_ns);
        if (written > 0)
            total += written;
        if (total >= buf_size - 1)
            break;
    }

    if (total < buf_size)
        buf[total] = '\0';
    else
        buf[buf_size - 1] = '\0';

    return total;
}

void ftrace_graph_clear(void)
{
    if (!g_graph_buf.initialized)
        return;

    memset(g_graph_buf.entries, 0, sizeof(g_graph_buf.entries));
    g_graph_buf.write_idx = 0;
}

/* ── Sysfs helper: write function addresses to set_graph_function ──── */
int ftrace_graph_parse_and_set_func(const char *input)
{
    if (!input)
        return -EINVAL;

    /* Input format: "func_name" or "0x<hex_addr>" */
    char buf[64];
    int i = 0;
    while (*input && *input != '\n' && i < (int)sizeof(buf) - 1) {
        buf[i++] = *input++;
    }
    buf[i] = '\0';

    if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        /* Hex address format — parse manually */
        uint64_t addr = 0;
        int j = 2;
        while (buf[j] != '\0') {
            addr <<= 4;
            if (buf[j] >= '0' && buf[j] <= '9')
                addr |= (uint64_t)(buf[j] - '0');
            else if (buf[j] >= 'a' && buf[j] <= 'f')
                addr |= (uint64_t)(buf[j] - 'a' + 10);
            else if (buf[j] >= 'A' && buf[j] <= 'F')
                addr |= (uint64_t)(buf[j] - 'A' + 10);
            else
                break;
            j++;
        }

        if (addr == 0)
            return -EINVAL;

        /* Try to find the function name from the address */
        const char *name = kallsyms_lookup(addr);
        if (name)
            return ftrace_graph_register(name);
        return -ENOENT;
    }

    /* Plain function name */
    return ftrace_graph_register(buf);
}

/* ══════════════════════════════════════════════════════════════════════
 * available_filter_functions — list all functions that can be traced
 * ══════════════════════════════════════════════════════════════════════
 *
 * Reads the kernel symbol table and returns a newline-separated list of
 * function names that could potentially be traced.
 */
int ftrace_available_filter_functions(char *buf, int buf_size)
{
    if (!buf || buf_size <= 0)
        return -EINVAL;

    /* Placeholder: return empty list with just a newline.
     * A full implementation would iterate the kernel symbol table
     * via ksym_for_each or kallsyms to produce a function list. */
    if (buf_size > 1) {
        buf[0] = '\n';
        buf[1] = '\0';
    }
    return 1;
}

/* ── Initialize trace.c / trace_events.c integration ───────────────── */
/* This function is called from the kernel init to set up graph tracing */
void __attribute__((used)) ftrace_late_init(void)
{
    ftrace_graph_init();
}

/* ── trace_printk (Item 31) ──────────────────────────────────────────
 *
 * trace_printk() writes formatted messages to a dedicated ring buffer
 * that can be read via /sys/kernel/debug/tracing/trace.
 * This provides a lightweight way to emit debug messages from any
 * context without going through the full kernel log buffer.
 */

/* Simple ring buffer for trace_printk output */
static char trace_printk_buf[TRACE_PRINTK_BUF_SIZE];
static volatile int trace_printk_head;
static volatile int trace_printk_enabled;

void trace_printk_init(void)
{
    trace_printk_head = 0;
    trace_printk_enabled = 1;
    kprintf("[OK] trace_printk initialized (%d bytes)\n", TRACE_PRINTK_BUF_SIZE);
}

void trace_printk(const char *fmt, ...)
{
    if (!trace_printk_enabled || !fmt)
        return;

    /* Format the message into a temporary buffer */
    char msg[256];
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int len = vsnprintf(msg, sizeof(msg), fmt, args);
    __builtin_va_end(args);

    if (len < 0) len = 0;
    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;

    /* Write to the ring buffer with a timestamp prefix */
    uint64_t now_ns = timer_get_ns();

    /* Format: [timestamp] message\n */
    char entry[320];
    int entry_len = snprintf(entry, sizeof(entry), "[%llu] %s\n",
                             (unsigned long long)now_ns, msg);
    if (entry_len < 0) return;
    if (entry_len >= (int)sizeof(entry)) entry_len = (int)sizeof(entry) - 1;

    /* Copy into ring buffer */
    for (int i = 0; i < entry_len; i++) {
        int idx = trace_printk_head;
        trace_printk_buf[idx] = entry[i];
        trace_printk_head = (idx + 1) % TRACE_PRINTK_BUF_SIZE;
    }

    /* NUL-terminate (best effort) */
    trace_printk_buf[trace_printk_head] = '\0';
}

/* Read trace_printk buffer — copies to user buffer.
 * Returns number of bytes read. */
int trace_printk_read(char *buf, int buf_size)
{
    if (!buf || buf_size <= 0) return 0;

    int head = trace_printk_head;
    int count = 0;

    /* Simple linear scan: find the oldest entry and return from there */
    int start = head;
    for (int i = 0; i < TRACE_PRINTK_BUF_SIZE && count < buf_size - 1; i++) {
        int idx = (start + i) % TRACE_PRINTK_BUF_SIZE;
        char c = trace_printk_buf[idx];
        if (c == '\0') continue;  /* skip holes */
        buf[count++] = c;
    }
    buf[count] = '\0';
    return count;
}

void trace_printk_enable(void) { trace_printk_enabled = 1; }
void trace_printk_disable(void) { trace_printk_enabled = 0; }
int  trace_printk_is_enabled(void) { return trace_printk_enabled; }

/* ── Stub: ftrace_set_filter ───────────────────────────────────────── */
int ftrace_set_filter(const char *filter_str)
{
    (void)filter_str;
    kprintf("[FTRACE] ftrace_set_filter: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: ftrace_dyn_arch_init ────────────────────────────────────── */
int ftrace_dyn_arch_init(void)
{
    kprintf("[FTRACE] ftrace_dyn_arch_init: not yet implemented\n");
    return -ENOSYS;
}
