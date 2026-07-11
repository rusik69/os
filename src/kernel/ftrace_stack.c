// SPDX-License-Identifier: GPL-2.0-only
/*
 * ftrace_stack.c — Ftrace stack tracer (max stack usage tracker)
 *
 * Tracks maximum kernel stack usage by checking stack depth
 * at function entry/exit points.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "smp.h"

#define STACK_TRACER_MAX_DEPTH 4096

struct stack_trace_entry {
    uintptr_t return_addr;
    const char *func_name;
};

struct stack_trace_state {
    uint64_t max_depth;
    uint64_t current_depth;
    uint64_t max_stack_usage;
    struct stack_trace_entry trace[STACK_TRACER_MAX_DEPTH];
};

static struct stack_trace_state stack_state;
static int stack_tracer_enabled = 0;
static spinlock_t g_stack_lock;

/* Enable the stack tracer */
static void ftrace_stack_enable(void)
{
    stack_tracer_enabled = 1;
    stack_state.max_depth = 0;
    stack_state.max_stack_usage = 0;
    kprintf("[FTRACE_STACK] Stack tracer enabled\n");
}

/* Disable the stack tracer */
static void ftrace_stack_disable(void)
{
    stack_tracer_enabled = 0;
}

/* Called at function entry (via instrumentation) */
static void __attribute__((used)) ftrace_stack_func_entry(uintptr_t return_addr,
                                                     const char *func_name)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_stack_lock, &irq_flags);

    if (!stack_tracer_enabled) {
        spinlock_irqsave_release(&g_stack_lock, irq_flags);
        return;
    }

    if (stack_state.current_depth < STACK_TRACER_MAX_DEPTH) {
        stack_state.trace[stack_state.current_depth].return_addr = return_addr;
        stack_state.trace[stack_state.current_depth].func_name = func_name;
    }
    stack_state.current_depth++;

    if (stack_state.current_depth > stack_state.max_depth) {
        stack_state.max_depth = stack_state.current_depth;
    }

    spinlock_irqsave_release(&g_stack_lock, irq_flags);
}

/* Called at function exit (via instrumentation) */
static void __attribute__((used)) ftrace_stack_func_exit(void)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_stack_lock, &irq_flags);

    if (!stack_tracer_enabled) {
        spinlock_irqsave_release(&g_stack_lock, irq_flags);
        return;
    }

    if (stack_state.current_depth > 0)
        stack_state.current_depth--;

    spinlock_irqsave_release(&g_stack_lock, irq_flags);
}

/* Record stack usage measurement */
static void ftrace_stack_record_usage(uint64_t usage)
{
    if (usage > stack_state.max_stack_usage) {
        stack_state.max_stack_usage = usage;
        if (usage > 2048) { /* > 2KB is noteworthy */
            kprintf("[FTRACE_STACK] New max stack usage: %llu bytes (depth=%llu)\n",
                    (unsigned long long)usage,
                    (unsigned long long)stack_state.max_depth);
        }
    }
}

/* Print stack tracer statistics */
static void ftrace_stack_print_stats(void)
{
    kprintf("Stack tracer: enabled=%d\n", stack_tracer_enabled);
    kprintf("  Max depth: %llu\n", (unsigned long long)stack_state.max_depth);
    kprintf("  Max stack usage: %llu bytes\n",
            (unsigned long long)stack_state.max_stack_usage);
    kprintf("  Current depth: %llu\n",
            (unsigned long long)stack_state.current_depth);

    /* Print the deepest recorded stack trace */
    kprintf("  Deepest stack trace:\n");
    for (uint64_t i = 0; i < stack_state.max_depth && i < 32; i++) {
        if (stack_state.trace[i].func_name) {
            kprintf("    [%llu] %s\n",
                    (unsigned long long)i,
                    stack_state.trace[i].func_name);
        }
    }
}

static void ftrace_stack_init(void)
{
    memset(&stack_state, 0, sizeof(stack_state));
    spinlock_init(&g_stack_lock);
    kprintf("[OK] Ftrace stack tracer — max stack usage tracker\n");
}

/* ── Stub: ftrace_stack_trace_save ─────────────────────────────────── */
static int ftrace_stack_trace_save(unsigned long *store, unsigned int size,
                            unsigned int skip)
{
    (void)store; (void)size; (void)skip;
    kprintf("[FTRACE_STACK] ftrace_stack_trace_save: not yet implemented\n");
    return 0;
}

/* ── Stub: ftrace_stack_trace_snprint ──────────────────────────────── */
static int ftrace_stack_trace_snprint(char *buf, size_t size,
                               unsigned long *entries, unsigned int nr_entries)
{
    (void)buf; (void)size; (void)entries; (void)nr_entries;
    kprintf("[FTRACE_STACK] ftrace_stack_trace_snprint: not yet implemented\n");
    return 0;
}

/* ── Stub: ftrace_stack_check ──────────────────────────────────────── */
static int ftrace_stack_check(unsigned long addr, size_t size)
{
    (void)addr; (void)size;
    kprintf("[FTRACE_STACK] ftrace_stack_check: not yet implemented\n");
    return 0;
}

/* ── Stub: ftrace_stack_reserve ────────────────────────────────────── */
static void *ftrace_stack_reserve(size_t size)
{
    (void)size;
    kprintf("[FTRACE_STACK] ftrace_stack_reserve: not yet implemented\n");
    return NULL;
}
