#ifndef FTRACE_H
#define FTRACE_H

#include "types.h"

/*
 * Dynamic function tracer (FTRACE).
 *
 * Provides a mechanism to register trace callbacks on kernel functions.
 * When a traced function is called, the registered callback is invoked
 * with the function's return address.
 *
 * Implementation: kprobe-based with a trampoline table for now.
 */

/* Maximum number of tracepoints */
#define FTRACE_MAX_TRACEPOINTS 64

/* Tracepoint entry */
struct ftrace_tracepoint {
    char     func_name[64];    /* function name to trace */
    void     (*callback)(uint64_t ip, uint64_t parent_ip);
    int      active;           /* 1 = registered */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialize the FTRACE subsystem */
void ftrace_init(void);

/* Register a trace callback on a function by name.
 * Returns 0 on success, -1 on error (table full or already registered). */
int  ftrace_register(const char *func_name, void (*callback)(uint64_t ip, uint64_t parent_ip));

/* Unregister a tracepoint by function name.
 * Returns 0 on success, -1 if not found. */
int  ftrace_unregister(const char *func_name);

/* Enable/disable all tracepoints globally. */
void ftrace_enable(void);
void ftrace_disable(void);

/* Check if FTRACE is currently enabled. */
int  ftrace_enabled(void);

/* Called by the kprobe handler when a traced function is hit.
 * Not intended for direct use by modules. */
void ftrace_dispatch(uint64_t ip, uint64_t parent_ip);

/* Dump registered tracepoints via kprintf. */
void ftrace_dump(void);

/* ══════════════════════════════════════════════════════════════════════
 * Function Graph Tracer
 * ══════════════════════════════════════════════════════════════════════
 *
 * Traces function entry and exit, recording duration for each call.
 * Uses kretprobes to intercept function returns.
 * Stores results in a ring buffer: [pid, func_addr, entry_ns, duration_ns].
 */

/* Maximum functions that can be graph-traced simultaneously */
#define FTRACE_GRAPH_MAX_FUNCS   32

/* Size of the graph trace output ring buffer (number of completed entries) */
#define FTRACE_GRAPH_BUF_SIZE    4096

/* Maximum call depth to track */
#define FTRACE_GRAPH_MAX_DEPTH   64

/* Tracer modes for current_tracer */
#define FTRACE_TRACER_NOP          0
#define FTRACE_TRACER_FUNCTION     1
#define FTRACE_TRACER_FUNCTION_GRAPH 2

/* A single completed function graph entry in the ring buffer */
struct ftrace_graph_entry {
    uint64_t func_addr;
    uint64_t entry_ns;
    uint64_t duration_ns;
    uint32_t pid;
    uint32_t depth;
} __attribute__((packed));

/* ── Function Graph Public API ─────────────────────────────────────── */

/* Initialize the function graph tracer state */
void ftrace_graph_init(void);

/* Add/remove a function to graph-trace by name.
 * Returns 0 on success, -ENOMEM or -ENOENT on error. */
int  ftrace_graph_register(const char *func_name);
int  ftrace_graph_unregister(const char *func_name);

/* Enable/disable graph tracing (independent of individual function registration) */
void ftrace_graph_enable(void);
void ftrace_graph_disable(void);
int  ftrace_graph_is_enabled(void);

/* Set/get the current tracer mode (FTRACE_TRACER_NOP/FUNCTION/FUNCTION_GRAPH) */
int  ftrace_set_tracer(int mode);
int  ftrace_get_tracer(void);

/* Set/get the maximum call depth to trace (0 = unlimited) */
void ftrace_graph_set_max_depth(int depth);
int  ftrace_graph_get_max_depth(void);

/* Read the graph trace ring buffer into a human-readable string.
 * Returns number of bytes written, or negative on error. */
int  ftrace_graph_read_trace(char *buf, int buf_size);

/* Clear the graph trace ring buffer */
void ftrace_graph_clear(void);

/* ══════════════════════════════════════════════════════════════════════
 * Trace Events — Static event infrastructure (trace_events.c)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Lightweight structured trace events with per-event enable/disable.
 * Stored in a common ring buffer (TRACE_EV_V2_BUF_SIZE entries).
 */

#define TRACE_EV_V2_BUF_SIZE      8192
#define TRACE_EV_V2_PAYLOAD_MAX   24

/* Trace event type IDs */
#define TRACE_EV_V2_SCHED_SWITCH   1
#define TRACE_EV_V2_IRQ_ENTRY      2
#define TRACE_EV_V2_IRQ_EXIT       3
#define TRACE_EV_V2_TIMER_EXPIRE   4
#define TRACE_EV_V2_PAGE_FAULT     5
#define TRACE_EV_V2_SYSCALL_ENTRY  6
#define TRACE_EV_V2_SYSCALL_EXIT   7

/* New trace event types (Items 29-30) */
#define TRACE_EV_V2_NET_RX         8
#define TRACE_EV_V2_NET_TX         9
#define TRACE_EV_V2_BLOCK_READ     10
#define TRACE_EV_V2_BLOCK_WRITE    11
#define TRACE_EV_V2_BLOCK_COMPLETE 12

/* ── trace_printk ──────────────────────────────────────────────────── */
#define TRACE_PRINTK_BUF_SIZE 8192

/* A single trace event record */
struct trace_event_v2_record {
    uint64_t timestamp_ns;                      /* Nanosecond timestamp */
    uint16_t event_id;                          /* TRACE_EV_V2_* */
    uint8_t  payload[TRACE_EV_V2_PAYLOAD_MAX];  /* Event-specific data (up to 24 bytes) */
} __attribute__((packed));

/* ── Trace Events Public API ───────────────────────────────────────── */

/* Initialize trace events subsystem */
void trace_events_v2_init(void);

/* Global enable/disable */
void trace_events_v2_enable(void);
void trace_events_v2_disable(void);
int  trace_events_v2_is_enabled(void);

/* Per-event enable/disable (event_id from TRACE_EV_V2_*) */
int  trace_events_v2_set_event_enabled(uint16_t event_id, int enabled);
int  trace_events_v2_get_event_enabled(uint16_t event_id);

/* Write a trace event to the ring buffer (respects per-event enable) */
void trace_events_v2_write(uint16_t event_id, const void *payload);

/* Read events from the ring buffer.  If event_filter != 0, only return
 * events matching that filter.  Returns number of events read. */
int  trace_events_v2_read(struct trace_event_v2_record *buf, int max_count, uint16_t event_filter);

/* Clear all events from the ring buffer */
void trace_events_v2_clear(void);
void trace_events_stats(uint64_t *sched, uint64_t *timer, uint64_t *irq);

/* ── Convenience helpers for common trace events ───────────────────── */

struct trace_ev_payload_sched_switch {
    uint32_t prev_pid;
    uint32_t next_pid;
    int32_t  prev_state;
} __attribute__((packed));

struct trace_ev_payload_irq_entry {
    uint32_t irq_num;
    char     handler_name[20];
} __attribute__((packed));

struct trace_ev_payload_irq_exit {
    uint32_t irq_num;
    uint64_t duration_ns;
} __attribute__((packed));

struct trace_ev_payload_timer_expire {
    uint64_t timer_fn;
    uint64_t expires_jiffies;
} __attribute__((packed));

struct trace_ev_payload_page_fault {
    uint64_t addr;
    uint32_t flags;
    uint32_t pid;
} __attribute__((packed));

struct trace_ev_payload_syscall_entry {
    uint32_t nr;
    uint64_t arg0;
    uint64_t arg1;
} __attribute__((packed));

struct trace_ev_payload_syscall_exit {
    uint32_t nr;
    uint64_t retval;
} __attribute__((packed));

/* Convenience wrapper functions */
void trace_v2_sched_switch(uint32_t prev_pid, uint32_t next_pid, int32_t prev_state);
void trace_v2_irq_entry(uint32_t irq_num, const char *handler_name);
void trace_v2_irq_exit(uint32_t irq_num, uint64_t duration_ns);
void trace_v2_timer_expire(uint64_t timer_fn, uint64_t expires_jiffies);
void trace_v2_page_fault(uint64_t addr, uint32_t flags, uint32_t pid);
void trace_v2_syscall_entry(uint32_t nr, uint64_t arg0, uint64_t arg1);
void trace_v2_syscall_exit(uint32_t nr, uint64_t retval);

/* Network trace events (Item 29) */
struct trace_ev_payload_net_rx {
    uint32_t ifindex;
    uint16_t eth_proto;
    uint16_t len;
} __attribute__((packed));

struct trace_ev_payload_net_tx {
    uint32_t ifindex;
    uint16_t eth_proto;
    uint16_t len;
} __attribute__((packed));

void trace_v2_net_rx(uint32_t ifindex, uint16_t eth_proto, uint16_t len);
void trace_v2_net_tx(uint32_t ifindex, uint16_t eth_proto, uint16_t len);

/* Block trace events (Item 30) */
struct trace_ev_payload_block_read {
    uint32_t dev_id;
    uint64_t sector;
    uint32_t nr_sectors;
} __attribute__((packed));

struct trace_ev_payload_block_write {
    uint32_t dev_id;
    uint64_t sector;
    uint32_t nr_sectors;
} __attribute__((packed));

struct trace_ev_payload_block_complete {
    uint32_t dev_id;
    uint64_t sector;
    uint32_t nr_sectors;
    uint32_t error;
} __attribute__((packed));

void trace_v2_block_read(uint32_t dev_id, uint64_t sector, uint32_t nr_sectors);
void trace_v2_block_write(uint32_t dev_id, uint64_t sector, uint32_t nr_sectors);
void trace_v2_block_complete(uint32_t dev_id, uint64_t sector, uint32_t nr_sectors, uint32_t error);

/* trace_printk (Item 31) — Write a formatted message to the trace buffer */
void trace_printk(const char *fmt, ...) __printf(1, 2);
void trace_printk_init(void);

#endif /* FTRACE_H */
