/*
 * trace_events.c — Static trace event infrastructure
 *
 * Implements a lightweight static tracepoint framework using
 * DECLARE_EVENT_CLASS / DEFINE_EVENT macros (modeled after the
 * Linux kernel ftrace approach).
 *
 * Key trace points:
 *   - sched_switch: Task switch events
 *   - timer_expire: Timer callback execution
 *   - irq_handler: Interrupt handler entry/exit
 *
 * Item 130 — Trace events with DECLARE_EVENT_CLASS
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "smp.h"
#include "process.h"  /* for struct process */
#include "timer.h"    /* for timer_get_ticks */

/* ── Trace buffer ────────────────────────────────────────────────────── */

#define TRACE_EVENT_BUF_SIZE  (64 * 1024)  /* 64 KB circular buffer */
#define TRACE_EVENT_MAX_RECORDS 4096

struct trace_event_record {
    uint64_t timestamp;       /* TSC or timer ticks */
    uint32_t cpu_id;          /* CPU where event occurred */
    uint16_t event_id;        /* Event type ID */
    uint16_t data_len;        /* Length of variable data */
    char     data[128];       /* Variable data (up to 128 bytes) */
} __attribute__((packed));

/* Event IDs */
#define TRACE_EVENT_SCHED_SWITCH   1
#define TRACE_EVENT_TIMER_EXPIRE   2
#define TRACE_EVENT_IRQ_HANDLER    3
#define TRACE_EVENT_IRQ_RETURN     4
#define TRACE_EVENT_SCHED_WAKEUP   5

/* ── State ──────────────────────────────────────────────────────────── */

static struct {
    struct trace_event_record records[TRACE_EVENT_MAX_RECORDS];
    volatile uint32_t write_idx;   /* Next slot to write (atomically incremented) */
    int initialized;
    int enabled;                   /* 1 = tracing active, 0 = disabled */
} trace_events_state;

/* Internal event counter */
static uint64_t g_sched_switch_count = 0;
static uint64_t g_timer_expire_count = 0;
static uint64_t g_irq_count = 0;

/* ── Core trace write ──────────────────────────────────────────────── */

static void trace_event_write(uint16_t event_id, const void *data, uint16_t data_len)
{
    if (!trace_events_state.initialized || !trace_events_state.enabled)
        return;

    uint32_t idx = __sync_fetch_and_add(&trace_events_state.write_idx, 1) % TRACE_EVENT_MAX_RECORDS;

    struct trace_event_record *rec = &trace_events_state.records[idx];
    rec->timestamp = timer_get_ticks();  /* Use timer ticks as timestamp */
    rec->cpu_id = smp_get_cpu_id();
    rec->event_id = event_id;
    rec->data_len = (data_len > 128) ? 128 : data_len;
    if (data && rec->data_len > 0) {
        memcpy(rec->data, data, rec->data_len);
    }
}

/* ── Event class helpers ────────────────────────────────────────────── */

/*
 * DECLARE_EVENT_CLASS / DEFINE_EVENT equivalent:
 * Each trace point type has a 'probe' function that formats the data
 * and calls trace_event_write.
 */

/* ── sched_switch: trace task switch events ────────────────────────── */

struct sched_switch_data {
    uint32_t prev_pid;
    uint32_t next_pid;
    char     prev_comm[16];
    char     next_comm[16];
    int      prev_state;
};

void trace_sched_switch(struct process *prev, struct process *next)
{
    struct sched_switch_data d;
    memset(&d, 0, sizeof(d));

    if (prev) {
        d.prev_pid = prev->pid;
        strncpy(d.prev_comm, prev->name, 15);
        d.prev_comm[15] = '\0';
        d.prev_state = prev->state;
    }
    if (next) {
        d.next_pid = next->pid;
        strncpy(d.next_comm, next->name, 15);
        d.next_comm[15] = '\0';
    }

    trace_event_write(TRACE_EVENT_SCHED_SWITCH, &d, sizeof(d));
    g_sched_switch_count++;
}

/* ── timer_expire: trace timer callback execution ──────────────────── */

struct timer_expire_data {
    int      timer_id;
    uint64_t expiry_tick;
};

void trace_timer_expire(int timer_id, uint64_t expiry_tick)
{
    struct timer_expire_data d;
    d.timer_id = timer_id;
    d.expiry_tick = expiry_tick;

    trace_event_write(TRACE_EVENT_TIMER_EXPIRE, &d, sizeof(d));
    g_timer_expire_count++;
}

/* ── irq_handler: trace interrupt handler entry/exit ───────────────── */

struct irq_handler_data {
    uint32_t irq_vector;
    uint8_t  is_entry;    /* 1 = entry, 0 = exit */
    uint8_t  handled;     /* 1 = handled, 0 = not handled */
};

void trace_irq_handler_entry(uint32_t irq_vector)
{
    struct irq_handler_data d;
    d.irq_vector = irq_vector;
    d.is_entry = 1;
    d.handled = 0;

    trace_event_write(TRACE_EVENT_IRQ_HANDLER, &d, sizeof(d));
    g_irq_count++;
}

void trace_irq_handler_exit(uint32_t irq_vector, int handled)
{
    struct irq_handler_data d;
    d.irq_vector = irq_vector;
    d.is_entry = 0;
    d.handled = (handled ? 1 : 0);

    trace_event_write(TRACE_EVENT_IRQ_RETURN, &d, sizeof(d));
}

/* ── Public API ─────────────────────────────────────────────────────── */

void trace_events_init(void)
{
    if (trace_events_state.initialized)
        return;

    memset(&trace_events_state, 0, sizeof(trace_events_state));
    trace_events_state.initialized = 1;
    trace_events_state.enabled = 1;  /* Enabled by default */

    kprintf("[trace_events] Static tracepoint infrastructure initialized\n");
    kprintf("[trace_events] Buffer: %d records (%u bytes)\n",
            TRACE_EVENT_MAX_RECORDS,
            (unsigned int)sizeof(trace_events_state.records));
}

void trace_events_enable(void)
{
    if (trace_events_state.initialized)
        trace_events_state.enabled = 1;
}

void trace_events_disable(void)
{
    if (trace_events_state.initialized)
        trace_events_state.enabled = 0;
}

int trace_events_is_enabled(void)
{
    return trace_events_state.initialized && trace_events_state.enabled;
}

/*
 * Dump trace events to the console.
 * @limit: Maximum number of records to dump (0 = all).
 */
void trace_events_dump(int limit)
{
    if (!trace_events_state.initialized) {
        kprintf("[trace_events] Not initialized\n");
        return;
    }

    uint32_t count = trace_events_state.write_idx;
    if (count > TRACE_EVENT_MAX_RECORDS)
        count = TRACE_EVENT_MAX_RECORDS;

    if (limit > 0 && (int)count > limit)
        count = (uint32_t)limit;

    kprintf("=== Trace Events Dump (%u records) ===\n", count);
    kprintf("  Events: sched_switch=%llu timer=%llu irq=%llu\n",
            (unsigned long long)g_sched_switch_count,
            (unsigned long long)g_timer_expire_count,
            (unsigned long long)g_irq_count);

    /* Walk backwards from the last written record */
    uint32_t start = (trace_events_state.write_idx > 0)
                      ? (trace_events_state.write_idx - 1) % TRACE_EVENT_MAX_RECORDS
                      : 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + TRACE_EVENT_MAX_RECORDS - i) % TRACE_EVENT_MAX_RECORDS;
        struct trace_event_record *rec = &trace_events_state.records[idx];

        const char *event_name = "?";
        switch (rec->event_id) {
            case TRACE_EVENT_SCHED_SWITCH: event_name = "sched_switch"; break;
            case TRACE_EVENT_TIMER_EXPIRE: event_name = "timer_expire"; break;
            case TRACE_EVENT_IRQ_HANDLER:  event_name = "irq_handler";  break;
            case TRACE_EVENT_IRQ_RETURN:   event_name = "irq_return";   break;
            case TRACE_EVENT_SCHED_WAKEUP: event_name = "sched_wakeup"; break;
        }

        kprintf("  [%u] CPU%u tick=%llu %s\n",
                idx, rec->cpu_id,
                (unsigned long long)rec->timestamp,
                event_name);
    }

    kprintf("=== End Trace Dump ===\n");
}

/*
 * Clear all trace events.
 */
void trace_events_clear(void)
{
    if (!trace_events_state.initialized)
        return;

    memset(trace_events_state.records, 0, sizeof(trace_events_state.records));
    trace_events_state.write_idx = 0;
    g_sched_switch_count = 0;
    g_timer_expire_count = 0;
    g_irq_count = 0;

    kprintf("[trace_events] Buffer cleared\n");
}

/*
 * Get event statistics.
 */
void trace_events_stats(uint64_t *sched_count, uint64_t *timer_count, uint64_t *irq_count)
{
    if (sched_count) *sched_count = g_sched_switch_count;
    if (timer_count) *timer_count = g_timer_expire_count;
    if (irq_count)   *irq_count   = g_irq_count;
}
