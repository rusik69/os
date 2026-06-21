/*
 * cluster_autoscaler.c — Cluster autoscaler (C144)
 *
 * Monitors pending pod count against node capacity and triggers
 * scale-up events when unschedulable pods exceed a configurable threshold.
 * A cooldown period (default 5 minutes) prevents thrashing between
 * successive scale events.
 *
 * Functions:
 *   cluster_autoscaler_init()        — Initialise the subsystem
 *   cluster_autoscaler_should_scale()— Check whether scaling is required
 *   cluster_autoscaler_scale_up()    — Add nodes
 *   cluster_autoscaler_scale_down()  — Remove nodes
 *   cluster_autoscaler_report_pending() — Feed pending-pod counts
 *   cluster_autoscaler_update_capacity() — Update resource capacity
 *   cluster_autoscaler_get_info()    — Query current state
 */

#define KERNEL_INTERNAL
#include "cluster_autoscaler.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "timer.h"
#include "errno.h"
#include "heap.h"

/* ── Internal state ─────────────────────────────────────────────────── */

static struct cluster_autoscaler g_autoscaler;
static spinlock_t g_as_lock;
static int g_initialised;

/* ── Initialisation ─────────────────────────────────────────────────── */

int cluster_autoscaler_init(uint32_t min_nodes, uint32_t max_nodes)
{
    if (min_nodes > max_nodes || max_nodes == 0)
        return -EINVAL;
    if (max_nodes > AUTOSCALER_MAX_NODES)
        return -EINVAL;

    memset(&g_autoscaler, 0, sizeof(g_autoscaler));
    g_autoscaler.min_nodes    = min_nodes;
    g_autoscaler.max_nodes    = max_nodes;
    g_autoscaler.current_nodes = min_nodes;
    g_autoscaler.cooldown_ms  = AUTOSCALER_DEFAULT_COOLDOWN_MS;
    g_autoscaler.threshold    = AUTOSCALER_DEFAULT_THRESHOLD;
    g_autoscaler.enabled      = 1;

    g_initialised = 1;

    kprintf("[Autoscaler] Initialised: min=%u max=%u cooldown=%ums threshold=%d\n",
            min_nodes, max_nodes,
            (unsigned)g_autoscaler.cooldown_ms,
            g_autoscaler.threshold);
    return 0;
}

/* ── Scale decision ─────────────────────────────────────────────────── */

int cluster_autoscaler_should_scale(void)
{
    if (!g_initialised || !g_autoscaler.enabled)
        return 0;

    uint64_t now = timer_get_ms();
    spinlock_acquire(&g_as_lock);

    int decision = 0;

    /* Scale-up: pending pods exceed threshold AND cooldown elapsed */
    if (g_autoscaler.pending_pods > (uint32_t)g_autoscaler.threshold &&
        g_autoscaler.current_nodes < g_autoscaler.max_nodes &&
        now - g_autoscaler.last_scale_up > g_autoscaler.cooldown_ms) {
        decision = 1;
    }
    /* Scale-down: no pending pods, above minimum, cooldown elapsed */
    else if (g_autoscaler.pending_pods == 0 &&
             g_autoscaler.current_nodes > g_autoscaler.min_nodes &&
             now - g_autoscaler.last_scale_down > g_autoscaler.cooldown_ms) {
        decision = -1;
    }

    spinlock_release(&g_as_lock);
    return decision;
}

/* ── Scale actions ──────────────────────────────────────────────────── */

int cluster_autoscaler_scale_up(void)
{
    if (!g_initialised)
        return -EAGAIN;

    spinlock_acquire(&g_as_lock);

    if (g_autoscaler.current_nodes >= g_autoscaler.max_nodes) {
        spinlock_release(&g_as_lock);
        return 0;
    }

    /* Calculate how many nodes to add: ~1 node per 4 pending pods, min 1 */
    int to_add = (g_autoscaler.pending_pods + 3) / 4;
    if (to_add < 1) to_add = 1;

    uint32_t max_add = g_autoscaler.max_nodes - g_autoscaler.current_nodes;
    if ((uint32_t)to_add > max_add)
        to_add = (int)max_add;

    uint32_t prev = g_autoscaler.current_nodes;
    g_autoscaler.current_nodes += to_add;
    g_autoscaler.last_scale_up = timer_get_ms();

    spinlock_release(&g_as_lock);

    kprintf("[Autoscaler] Scale UP: +%d nodes (%u → %u)\n",
            to_add, prev, g_autoscaler.current_nodes);
    return to_add;
}

int cluster_autoscaler_scale_down(void)
{
    if (!g_initialised)
        return -EAGAIN;

    spinlock_acquire(&g_as_lock);

    if (g_autoscaler.current_nodes <= g_autoscaler.min_nodes) {
        spinlock_release(&g_as_lock);
        return -EPERM;
    }

    g_autoscaler.current_nodes--;
    g_autoscaler.last_scale_down = timer_get_ms();

    spinlock_release(&g_as_lock);

    kprintf("[Autoscaler] Scale DOWN: 1 node (%u)\n",
            g_autoscaler.current_nodes);
    return 0;
}

/* ── Capacity tracking ──────────────────────────────────────────────── */

void cluster_autoscaler_report_pending(int count)
{
    if (!g_initialised) return;
    spinlock_acquire(&g_as_lock);
    g_autoscaler.pending_pods = (count < 0) ? 0 : (uint32_t)count;
    spinlock_release(&g_as_lock);
}

void cluster_autoscaler_update_capacity(uint64_t cpu, uint64_t memory)
{
    if (!g_initialised) return;
    spinlock_acquire(&g_as_lock);
    g_autoscaler.cpu_capacity    = cpu;
    g_autoscaler.memory_capacity = memory;
    spinlock_release(&g_as_lock);
}

void cluster_autoscaler_get_info(struct cluster_autoscaler *out)
{
    if (!g_initialised || !out) return;
    spinlock_acquire(&g_as_lock);
    *out = g_autoscaler;
    spinlock_release(&g_as_lock);
}

/* ── Stub: autoscaler_run ─────────────────────────────── */
int autoscaler_run(int interval)
{
    (void)interval;
    kprintf("[cluster] autoscaler_run: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: autoscaler_stop ─────────────────────────────── */
int autoscaler_stop(void)
{
    kprintf("[cluster] autoscaler_stop: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: autoscaler_scale_up ─────────────────────────────── */
int autoscaler_scale_up(int count)
{
    (void)count;
    kprintf("[cluster] autoscaler_scale_up: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: autoscaler_scale_down ─────────────────────────────── */
int autoscaler_scale_down(int count)
{
    (void)count;
    kprintf("[cluster] autoscaler_scale_down: not yet implemented\n");
    return -ENOSYS;
}
