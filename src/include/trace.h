#ifndef TRACE_H
#define TRACE_H
#include "types.h"

/* Forward declarations */
struct process;

/* Legacy trace buffer API */
void trace_init(void);
void trace_write(const char *msg);
void trace_dump(void);

/* ── Static trace events (from ftrace.c) ────────────────────────────── */

void ftrace_trace_sched_switch(struct process *prev, struct process *next);
void ftrace_trace_irq_entry(uint32_t irq_vector);
void ftrace_trace_irq_exit(uint32_t irq_vector, int handled);
void ftrace_trace_timer_expire(int timer_id, uint64_t expiry_tick);
void ftrace_trace_page_fault(uint64_t address, uint64_t ip, uint32_t error_code);
void ftrace_trace_syscall_entry(uint64_t nr, uint64_t arg0, uint64_t arg1,
                                uint64_t arg2, uint64_t arg3);
void ftrace_trace_syscall_exit(uint64_t nr, uint64_t result);
void ftrace_trace_page_alloc(uint64_t pfn, size_t order);
void ftrace_trace_page_free(uint64_t pfn, size_t order);

/* Enable/disable static trace events */
void ftrace_events_enable(void);
void ftrace_events_disable(void);
int  ftrace_events_is_enabled(void);

#endif /* TRACE_H */
