/*
 * src/kernel/trace.c — Enhanced kernel tracing subsystem
 *
 * Provides both the legacy string-based trace buffer and a structured
 * trace event system for schedule, IRQ, timer, page fault, and syscall
 * events.  The structured events are written into a small ring buffer
 * and can be dumped via trace_dump_events().
 *
 * This complements the ftrace subsystem's static trace points
 * (ftrace_trace_* functions in ftrace.c) by providing a unified
 * dump interface.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "process.h"
#include "printf.h"
#include "trace.h"
#include "string.h"
#include "process.h"
#include "smp.h"
#include "timer.h"

/* ── Legacy trace buffer ───────────────────────────────────────────── */

#define TRACE_BUF_SIZE 4096
static char trace_buf[TRACE_BUF_SIZE];
static int trace_pos = 0;

void trace_init(void)
{
    memset(trace_buf, 0, sizeof(trace_buf));
    trace_pos = 0;
    kprintf("[OK] Kernel tracing initialized (buffer mode + structured events)\n");
}

void trace_write(const char *msg)
{
    if (!msg) return;
    while (*msg && trace_pos < TRACE_BUF_SIZE - 1)
        trace_buf[trace_pos++] = *msg++;
    trace_buf[trace_pos] = '\0';
}

void trace_dump(void)
{
    kprintf("=== TRACE DUMP ===\n%s\n=== END TRACE ===\n", trace_buf);
}

/* ── Structured trace event ring buffer ────────────────────────────── */

#define TRACE_EV_BUF_SIZE  256   /* Number of event records */

struct trace_event {
    uint64_t timestamp;      /* Timer ticks */
    uint32_t cpu_id;         /* CPU where event occurred */
    uint16_t event_type;     /* Event type ID */
    uint16_t data_len;       /* Length of variable data */
    char     data[64];       /* Variable data */
};

/* Event type IDs */
#define TRACE_EV_SCHED_SWITCH   1
#define TRACE_EV_IRQ_ENTRY      2
#define TRACE_EV_IRQ_EXIT       3
#define TRACE_EV_TIMER_EXPIRE   4
#define TRACE_EV_PAGE_FAULT     5
#define TRACE_EV_SYSCALL_ENTER  6
#define TRACE_EV_SYSCALL_EXIT   7

static struct {
    struct trace_event events[TRACE_EV_BUF_SIZE];
    volatile uint32_t write_idx;
    int enabled;
    int initialized;
} trace_ev_state;

/* ── Internal: write a trace event record ──────────────────────────── */

static void trace_ev_write(uint16_t type, const void *data, uint16_t len)
{
    if (!trace_ev_state.initialized || !trace_ev_state.enabled)
        return;

    uint32_t idx = __sync_fetch_and_add(&trace_ev_state.write_idx, 1) % TRACE_EV_BUF_SIZE;

    struct trace_event *ev = &trace_ev_state.events[idx];
    ev->timestamp = timer_get_ticks();
    ev->cpu_id = smp_get_cpu_id();
    ev->event_type = type;
    ev->data_len = (len > 64) ? 64 : len;
    if (data && ev->data_len > 0)
        memcpy(ev->data, data, ev->data_len);
}

/* ── Event: sched_switch ───────────────────────────────────────────── */

struct sched_switch_ev {
    uint32_t prev_pid;
    uint32_t next_pid;
    int      prev_state;
    char     prev_comm[16];
    char     next_comm[16];
};

void trace_ev_sched_switch(struct process *prev, struct process *next)
{
    struct sched_switch_ev d;
    memset(&d, 0, sizeof(d));

    if (prev) {
        d.prev_pid = prev->pid;
        d.prev_state = prev->state;
        strncpy(d.prev_comm, prev->name, 15);
        d.prev_comm[15] = '\0';
    }
    if (next) {
        d.next_pid = next->pid;
        strncpy(d.next_comm, next->name, 15);
        d.next_comm[15] = '\0';
    }

    trace_ev_write(TRACE_EV_SCHED_SWITCH, &d, sizeof(d));
}

/* ── Event: IRQ entry/exit ─────────────────────────────────────────── */

struct irq_ev {
    uint32_t vector;
    uint8_t  is_entry;
    uint8_t  handled;
};

void trace_ev_irq_entry(uint32_t vector)
{
    struct irq_ev d;
    d.vector = vector;
    d.is_entry = 1;
    d.handled = 0;
    trace_ev_write(TRACE_EV_IRQ_ENTRY, &d, sizeof(d));
}

void trace_ev_irq_exit(uint32_t vector, int handled)
{
    struct irq_ev d;
    d.vector = vector;
    d.is_entry = 0;
    d.handled = handled ? 1 : 0;
    trace_ev_write(TRACE_EV_IRQ_EXIT, &d, sizeof(d));
}

/* ── Event: timer expire ───────────────────────────────────────────── */

struct timer_ev {
    int      timer_id;
    uint64_t expiry_tick;
};

void trace_ev_timer_expire(int timer_id, uint64_t expiry_tick)
{
    struct timer_ev d;
    d.timer_id = timer_id;
    d.expiry_tick = expiry_tick;
    trace_ev_write(TRACE_EV_TIMER_EXPIRE, &d, sizeof(d));
}

/* ── Event: page fault ─────────────────────────────────────────────── */

struct pf_ev {
    uint64_t address;
    uint64_t ip;
    uint32_t error_code;
};

void trace_ev_page_fault(uint64_t address, uint64_t ip, uint32_t error_code)
{
    struct pf_ev d;
    d.address = address;
    d.ip = ip;
    d.error_code = error_code;
    trace_ev_write(TRACE_EV_PAGE_FAULT, &d, sizeof(d));
}

/* ── Event: syscall entry/exit ─────────────────────────────────────── */

struct syscall_ev {
    uint64_t nr;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t result;
    uint8_t  is_entry;
};

void trace_ev_syscall_entry(uint64_t nr, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    struct syscall_ev d;
    d.nr = nr;
    d.arg0 = arg0;
    d.arg1 = arg1;
    d.result = arg2;
    d.is_entry = 1;
    trace_ev_write(TRACE_EV_SYSCALL_ENTER, &d, sizeof(d));
}

void trace_ev_syscall_exit(uint64_t nr, uint64_t result)
{
    struct syscall_ev d;
    d.nr = nr;
    d.arg0 = 0;
    d.arg1 = 0;
    d.result = result;
    d.is_entry = 0;
    trace_ev_write(TRACE_EV_SYSCALL_EXIT, &d, sizeof(d));
}

/* ── Control API ───────────────────────────────────────────────────── */

void trace_ev_enable(void)
{
    trace_ev_state.enabled = 1;
}

void trace_ev_disable(void)
{
    trace_ev_state.enabled = 0;
}

int trace_ev_is_enabled(void)
{
    return trace_ev_state.initialized && trace_ev_state.enabled;
}

/* ── Dump all structured trace events ──────────────────────────────── */

void trace_ev_dump(int limit)
{
    if (!trace_ev_state.initialized) {
        kprintf("[trace] Structured events not initialized\n");
        return;
    }

    uint32_t count = trace_ev_state.write_idx;
    if (count > TRACE_EV_BUF_SIZE)
        count = TRACE_EV_BUF_SIZE;
    if (limit > 0 && (int)count > limit)
        count = (uint32_t)limit;

    kprintf("=== Structured Trace Events (%u records) ===\n", count);

    uint32_t start = (trace_ev_state.write_idx > 0)
                     ? (trace_ev_state.write_idx - 1) % TRACE_EV_BUF_SIZE
                     : 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + TRACE_EV_BUF_SIZE - i) % TRACE_EV_BUF_SIZE;
        struct trace_event *ev = &trace_ev_state.events[idx];

        const char *type_name = "?";
        switch (ev->event_type) {
            case TRACE_EV_SCHED_SWITCH:  type_name = "sched_switch";  break;
            case TRACE_EV_IRQ_ENTRY:     type_name = "irq_entry";     break;
            case TRACE_EV_IRQ_EXIT:      type_name = "irq_exit";      break;
            case TRACE_EV_TIMER_EXPIRE:  type_name = "timer_expire";  break;
            case TRACE_EV_PAGE_FAULT:    type_name = "page_fault";    break;
            case TRACE_EV_SYSCALL_ENTER: type_name = "syscall_enter"; break;
            case TRACE_EV_SYSCALL_EXIT:  type_name = "syscall_exit";  break;
        }

        kprintf("  [%u] CPU%u tick=%llu %s\n",
                idx, ev->cpu_id,
                (unsigned long long)ev->timestamp,
                type_name);
    }

    kprintf("=== End Trace Events ===\n");
}

/* ── Clear events ──────────────────────────────────────────────────── */

void trace_ev_clear(void)
{
    if (!trace_ev_state.initialized) return;
    memset(trace_ev_state.events, 0, sizeof(trace_ev_state.events));
    trace_ev_state.write_idx = 0;
}

/* ── Combined init (called from init) ──────────────────────────────── */

void trace_ev_init(void)
{
    if (trace_ev_state.initialized) return;

    memset(&trace_ev_state, 0, sizeof(trace_ev_state));
    trace_ev_state.initialized = 1;
    trace_ev_state.enabled = 1;

    kprintf("[trace] Structured event buffer initialized (%d records)\n",
            TRACE_EV_BUF_SIZE);
}
