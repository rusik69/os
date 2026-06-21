/*
 * trace_events.c — Static trace event infrastructure (v2)
 *
 * Implements a lightweight static tracepoint framework using
 * DECLARE_EVENT_CLASS / DEFINE_EVENT macros (modeled after the
 * Linux kernel ftrace approach).
 *
 * Key trace points:
 *   - sched_switch: Task switch events
 *   - timer_expire: Timer callback execution
 *   - irq_handler: Interrupt handler entry/exit
 *   - page_fault: Page fault events
 *   - syscall_entry/exit: System call tracing
 *
 * Each event stores: timestamp_ns, event_id, payload (up to 24 bytes).
 * Events are stored in a common ring buffer (8192 entries) with
 * per-event enable/disable control.
 * When the buffer is full, newest overwrites oldest.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "smp.h"
#include "process.h"  /* for struct process */
#include "timer.h"    /* for timer_get_ns */
#include "ftrace.h"   /* for TRACE_EV_V2_* constants and structs */
#include "spinlock.h"

/* ── Ring buffer ───────────────────────────────────────────────────── */

static struct {
    struct trace_event_v2_record entries[TRACE_EV_V2_BUF_SIZE];
    volatile uint32_t write_idx;      /* Next slot to write (atomically incremented) */
    int initialized;
    int enabled;                      /* Global enable/disable */
    uint32_t enabled_mask;            /* Bitmask: bit N = 1 if event ID N is enabled */
} trace_ev_v2_state;

static spinlock_t g_trace_ev_v2_lock;

/* Event-specific counters */
static uint64_t g_ev_counters[8];     /* Event ID -> count */

/* ── Core trace write ──────────────────────────────────────────────── */

static void trace_ev_v2_write_locked(uint16_t event_id, const void *payload)
{
    if (!trace_ev_v2_state.initialized || !trace_ev_v2_state.enabled)
        return;

    /* Check per-event enable */
    if (event_id >= 64)
        return;
    if (!(trace_ev_v2_state.enabled_mask & (1U << event_id)))
        return;

    uint32_t idx = __sync_fetch_and_add(&trace_ev_v2_state.write_idx, 1) % TRACE_EV_V2_BUF_SIZE;

    struct trace_event_v2_record *rec = &trace_ev_v2_state.entries[idx];
    rec->timestamp_ns = timer_get_ns();
    rec->event_id = event_id;

    if (payload) {
        memcpy(rec->payload, payload, TRACE_EV_V2_PAYLOAD_MAX);
    } else {
        memset(rec->payload, 0, TRACE_EV_V2_PAYLOAD_MAX);
    }

    if (event_id < 8)
        g_ev_counters[event_id]++;
}

/* ── Public API ────────────────────────────────────────────────────── */

void trace_events_v2_init(void)
{
    if (trace_ev_v2_state.initialized)
        return;

    memset(&trace_ev_v2_state, 0, sizeof(trace_ev_v2_state));
    trace_ev_v2_state.initialized = 1;
    trace_ev_v2_state.enabled = 1;

    /* Enable all event types by default */
    trace_ev_v2_state.enabled_mask = 0xFE;  /* bits 1-7 enabled (skip bit 0) */

    spinlock_init(&g_trace_ev_v2_lock);

    kprintf("[trace_events] v2 buffer initialized (%d records, %d bytes payload each)\n",
            TRACE_EV_V2_BUF_SIZE, TRACE_EV_V2_PAYLOAD_MAX);
}

void trace_events_v2_enable(void)
{
    if (trace_ev_v2_state.initialized)
        trace_ev_v2_state.enabled = 1;
}

void trace_events_v2_disable(void)
{
    if (trace_ev_v2_state.initialized)
        trace_ev_v2_state.enabled = 0;
}

int trace_events_v2_is_enabled(void)
{
    return trace_ev_v2_state.initialized && trace_ev_v2_state.enabled;
}

int trace_events_v2_set_event_enabled(uint16_t event_id, int enabled)
{
    if (event_id >= 64)
        return -EINVAL;
    if (!trace_ev_v2_state.initialized)
        return -ENXIO;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_trace_ev_v2_lock, &irq_flags);

    if (enabled)
        trace_ev_v2_state.enabled_mask |= (1U << event_id);
    else
        trace_ev_v2_state.enabled_mask &= ~(1U << event_id);

    spinlock_irqsave_release(&g_trace_ev_v2_lock, irq_flags);
    return 0;
}

int trace_events_v2_get_event_enabled(uint16_t event_id)
{
    if (event_id >= 64 || !trace_ev_v2_state.initialized)
        return 0;
    return (trace_ev_v2_state.enabled_mask >> event_id) & 1;
}

void trace_events_v2_write(uint16_t event_id, const void *payload)
{
    trace_ev_v2_write_locked(event_id, payload);
}

int trace_events_v2_read(struct trace_event_v2_record *buf, int max_count,
                          uint16_t event_filter)
{
    if (!buf || max_count <= 0 || !trace_ev_v2_state.initialized)
        return 0;

    uint32_t count = trace_ev_v2_state.write_idx;
    uint32_t n = (count < TRACE_EV_V2_BUF_SIZE) ? count : TRACE_EV_V2_BUF_SIZE;

    if (n == 0)
        return 0;

    int out_idx = 0;

    /* Walk backwards (most recent first) for filtering */
    uint32_t start = (count > 0) ? (count - 1) % TRACE_EV_V2_BUF_SIZE : 0;

    for (uint32_t i = 0; i < n && out_idx < max_count; i++) {
        uint32_t idx = (start + TRACE_EV_V2_BUF_SIZE - i) % TRACE_EV_V2_BUF_SIZE;
        struct trace_event_v2_record *rec = &trace_ev_v2_state.entries[idx];

        if (event_filter == 0 || rec->event_id == event_filter) {
            buf[out_idx++] = *rec;
        }
    }

    return out_idx;
}

void trace_events_v2_clear(void)
{
    if (!trace_ev_v2_state.initialized)
        return;

    memset(trace_ev_v2_state.entries, 0, sizeof(trace_ev_v2_state.entries));
    trace_ev_v2_state.write_idx = 0;
    memset(g_ev_counters, 0, sizeof(g_ev_counters));
}

void trace_events_stats(uint64_t *sched, uint64_t *timer, uint64_t *irq)
{
    if (sched) *sched = trace_ev_v2_state.write_idx;
    if (timer) *timer = 0;
    if (irq) *irq = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * Convenience trace event functions
 * ══════════════════════════════════════════════════════════════════════ */

void trace_v2_sched_switch(uint32_t prev_pid, uint32_t next_pid,
                            int32_t prev_state)
{
    struct trace_ev_payload_sched_switch pld;
    pld.prev_pid = prev_pid;
    pld.next_pid = next_pid;
    pld.prev_state = prev_state;
    trace_ev_v2_write_locked(TRACE_EV_V2_SCHED_SWITCH, &pld);
}

void trace_v2_irq_entry(uint32_t irq_num, const char *handler_name)
{
    struct trace_ev_payload_irq_entry pld;
    memset(&pld, 0, sizeof(pld));
    pld.irq_num = irq_num;
    if (handler_name) {
        strncpy(pld.handler_name, handler_name, sizeof(pld.handler_name) - 1);
        pld.handler_name[sizeof(pld.handler_name) - 1] = '\0';
    }
    trace_ev_v2_write_locked(TRACE_EV_V2_IRQ_ENTRY, &pld);
}

void trace_v2_irq_exit(uint32_t irq_num, uint64_t duration_ns)
{
    struct trace_ev_payload_irq_exit pld;
    pld.irq_num = irq_num;
    pld.duration_ns = duration_ns;
    trace_ev_v2_write_locked(TRACE_EV_V2_IRQ_EXIT, &pld);
}

void trace_v2_timer_expire(uint64_t timer_fn, uint64_t expires_jiffies)
{
    struct trace_ev_payload_timer_expire pld;
    pld.timer_fn = timer_fn;
    pld.expires_jiffies = expires_jiffies;
    trace_ev_v2_write_locked(TRACE_EV_V2_TIMER_EXPIRE, &pld);
}

void trace_v2_page_fault(uint64_t addr, uint32_t flags, uint32_t pid)
{
    struct trace_ev_payload_page_fault pld;
    pld.addr = addr;
    pld.flags = flags;
    pld.pid = pid;
    trace_ev_v2_write_locked(TRACE_EV_V2_PAGE_FAULT, &pld);
}

void trace_v2_syscall_entry(uint32_t nr, uint64_t arg0, uint64_t arg1)
{
    struct trace_ev_payload_syscall_entry pld;
    pld.nr = nr;
    pld.arg0 = arg0;
    pld.arg1 = arg1;
    trace_ev_v2_write_locked(TRACE_EV_V2_SYSCALL_ENTRY, &pld);
}

void trace_v2_syscall_exit(uint32_t nr, uint64_t retval)
{
    struct trace_ev_payload_syscall_exit pld;
    pld.nr = nr;
    pld.retval = retval;
    trace_ev_v2_write_locked(TRACE_EV_V2_SYSCALL_EXIT, &pld);
}

/* ══════════════════════════════════════════════════════════════════════
 * Network trace events (Item 29)
 * ══════════════════════════════════════════════════════════════════════ */

void trace_v2_net_rx(uint32_t ifindex, uint16_t eth_proto, uint16_t len)
{
    struct trace_ev_payload_net_rx pld;
    pld.ifindex = ifindex;
    pld.eth_proto = eth_proto;
    pld.len = len;
    trace_ev_v2_write_locked(TRACE_EV_V2_NET_RX, &pld);
}

void trace_v2_net_tx(uint32_t ifindex, uint16_t eth_proto, uint16_t len)
{
    struct trace_ev_payload_net_tx pld;
    pld.ifindex = ifindex;
    pld.eth_proto = eth_proto;
    pld.len = len;
    trace_ev_v2_write_locked(TRACE_EV_V2_NET_TX, &pld);
}

/* ══════════════════════════════════════════════════════════════════════
 * Block trace events (Item 30)
 * ══════════════════════════════════════════════════════════════════════ */

void trace_v2_block_read(uint32_t dev_id, uint64_t sector, uint32_t nr_sectors)
{
    struct trace_ev_payload_block_read pld;
    pld.dev_id = dev_id;
    pld.sector = sector;
    pld.nr_sectors = nr_sectors;
    trace_ev_v2_write_locked(TRACE_EV_V2_BLOCK_READ, &pld);
}

void trace_v2_block_write(uint32_t dev_id, uint64_t sector, uint32_t nr_sectors)
{
    struct trace_ev_payload_block_write pld;
    pld.dev_id = dev_id;
    pld.sector = sector;
    pld.nr_sectors = nr_sectors;
    trace_ev_v2_write_locked(TRACE_EV_V2_BLOCK_WRITE, &pld);
}

void trace_v2_block_complete(uint32_t dev_id, uint64_t sector,
                             uint32_t nr_sectors, uint32_t error)
{
    struct trace_ev_payload_block_complete pld;
    pld.dev_id = dev_id;
    pld.sector = sector;
    pld.nr_sectors = nr_sectors;
    pld.error = error;
    trace_ev_v2_write_locked(TRACE_EV_V2_BLOCK_COMPLETE, &pld);
}

/* ── Stub: trace_event_register ─────────────────────────────── */
int trace_event_register(const char *name, void *event)
{
    (void)name;
    (void)event;
    kprintf("[trace] trace_event_register: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: trace_event_unregister ─────────────────────────────── */
int trace_event_unregister(const char *name)
{
    (void)name;
    kprintf("[trace] trace_event_unregister: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: trace_event_write ─────────────────────────────── */
int trace_event_write(const char *name, const void *data, size_t len)
{
    (void)name;
    (void)data;
    (void)len;
    kprintf("[trace] trace_event_write: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: trace_event_read ─────────────────────────────── */
int trace_event_read(const char *name, void *buf, size_t len)
{
    (void)name;
    (void)buf;
    (void)len;
    kprintf("[trace] trace_event_read: not yet implemented\n");
    return -ENOSYS;
}
