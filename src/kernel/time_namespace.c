/*
 * time_namespace.c — Time namespace for containers
 *
 * Allows each namespace to have its own monotonic and boottime clocks.
 * This enables container migration without time warps by virtualizing
 * the CLOCK_MONOTONIC and CLOCK_BOOTTIME offsets.
 *
 * Each time namespace stores the offsets to apply to the host's
 * real-time clocks when read by processes inside the namespace.
 */

#define KERNEL_INTERNAL
#include "time_namespace.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"

#define TIME_NS_MAX  32

/* Time namespace descriptor */
struct time_ns {
    int      in_use;
    int      id;
    /* Offsets to subtract from host clocks to get namespace-relative time */
    int64_t  monotonic_offset_ns;
    int64_t  boottime_offset_ns;
};

static struct time_ns g_time_ns[TIME_NS_MAX];
static spinlock_t g_time_ns_lock;
static int g_time_ns_count;

/* ── Namespace operations ──────────────────────────────────────────── */

int time_ns_create(void)
{
    spinlock_acquire(&g_time_ns_lock);
    for (int i = 0; i < TIME_NS_MAX; i++) {
        if (!g_time_ns[i].in_use) {
            struct time_ns *ns = &g_time_ns[i];
            memset(ns, 0, sizeof(*ns));
            ns->in_use = 1;
            ns->id = i;
            g_time_ns_count++;
            spinlock_release(&g_time_ns_lock);
            kprintf("[TIME_NS] Created time namespace %d\n", i);
            return i;
        }
    }
    spinlock_release(&g_time_ns_lock);
    return -ENOSPC;
}

int time_ns_destroy(int ns_id)
{
    spinlock_acquire(&g_time_ns_lock);
    if (ns_id < 0 || ns_id >= TIME_NS_MAX || !g_time_ns[ns_id].in_use) {
        spinlock_release(&g_time_ns_lock);
        return -EINVAL;
    }
    memset(&g_time_ns[ns_id], 0, sizeof(struct time_ns));
    g_time_ns_count--;
    spinlock_release(&g_time_ns_lock);
    return 0;
}

int time_ns_set_offsets(int ns_id, int64_t monotonic_offset_ns,
                        int64_t boottime_offset_ns)
{
    spinlock_acquire(&g_time_ns_lock);
    if (ns_id < 0 || ns_id >= TIME_NS_MAX || !g_time_ns[ns_id].in_use) {
        spinlock_release(&g_time_ns_lock);
        return -EINVAL;
    }
    g_time_ns[ns_id].monotonic_offset_ns = monotonic_offset_ns;
    g_time_ns[ns_id].boottime_offset_ns = boottime_offset_ns;
    spinlock_release(&g_time_ns_lock);
    return 0;
}

int time_ns_get_offsets(int ns_id, int64_t *monotonic_offset_ns,
                        int64_t *boottime_offset_ns)
{
    spinlock_acquire(&g_time_ns_lock);
    if (ns_id < 0 || ns_id >= TIME_NS_MAX || !g_time_ns[ns_id].in_use) {
        spinlock_release(&g_time_ns_lock);
        return -EINVAL;
    }
    if (monotonic_offset_ns) *monotonic_offset_ns = g_time_ns[ns_id].monotonic_offset_ns;
    if (boottime_offset_ns) *boottime_offset_ns = g_time_ns[ns_id].boottime_offset_ns;
    spinlock_release(&g_time_ns_lock);
    return 0;
}

/* Get namespace-relative clock value.
 * Call this instead of timer_get_ns() for processes in a time namespace. */
uint64_t time_ns_clock_gettime_ns(int ns_id, int clock_id)
{
    uint64_t host_time = timer_get_ns();

    spinlock_acquire(&g_time_ns_lock);
    if (ns_id < 0 || ns_id >= TIME_NS_MAX || !g_time_ns[ns_id].in_use) {
        spinlock_release(&g_time_ns_lock);
        return host_time;  /* no namespace = host time */
    }

    int64_t offset = 0;
    if (clock_id == 1) {      /* CLOCK_MONOTONIC */
        offset = g_time_ns[ns_id].monotonic_offset_ns;
    } else if (clock_id == 7) { /* CLOCK_BOOTTIME */
        offset = g_time_ns[ns_id].boottime_offset_ns;
    }

    int64_t result = (int64_t)host_time - offset;
    spinlock_release(&g_time_ns_lock);

    if (result < 0) result = 0;
    return (uint64_t)result;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void time_ns_init(void)
{
    memset(g_time_ns, 0, sizeof(g_time_ns));
    spinlock_init(&g_time_ns_lock);
    g_time_ns_count = 0;
    kprintf("[OK] Time namespace initialized (%d max)\n", TIME_NS_MAX);
}

/* ── Stub: time_ns_delete ─────────────────────────────── */
int time_ns_delete(void *ns)
{
    (void)ns;
    kprintf("[time_ns] time_ns_delete: not yet implemented\n");
    return 0;
}
/* ── Stub: time_ns_set_offset ─────────────────────────────── */
int time_ns_set_offset(void *ns, int clock, uint64_t offset)
{
    (void)ns;
    (void)clock;
    (void)offset;
    kprintf("[time_ns] time_ns_set_offset: not yet implemented\n");
    return 0;
}
