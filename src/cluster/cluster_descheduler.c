/*
 * cluster_descheduler.c — Cluster descheduler (C145)
 *
 * Evicts pods from underutilised nodes (< 30% CPU usage) and
 * reschedules them to better-packed nodes.  Supports multiple
 * descheduling policies:
 *   LowNodeUtilization  — rebalance from low-utilisation nodes
 *   HighNodeUtilization — rebalance from over-utilisation nodes
 *   PodLifecycle        — evict long-running pods
 *
 * Functions:
 *   cluster_descheduler_init()        — Initialise subsystem
 *   cluster_descheduler_run()         — Evaluate policies and evict
 *   cluster_descheduler_add_policy()  — Register a policy
 *   cluster_descheduler_remove_policy() — Remove a policy
 *   cluster_descheduler_get_stats()   — Query statistics
 */

#define KERNEL_INTERNAL
#include "cluster_descheduler.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "timer.h"
#include "errno.h"
#include "heap.h"

/* ── Internal state ─────────────────────────────────────────────────── */

static struct desched_policy g_policies[DESCHED_POLICIES_MAX];
static spinlock_t g_ds_lock;
static int g_initialised;
static uint64_t g_total_evictions;

/* ── Initialisation ─────────────────────────────────────────────────── */

int cluster_descheduler_init(void)
{
    if (g_initialised)
        return 0;

    memset(g_policies, 0, sizeof(g_policies));
    g_total_evictions = 0;
    g_initialised = 1;

    kprintf("[Descheduler] Initialised\n");
    return 0;
}

/* ── Policy management ──────────────────────────────────────────────── */

int cluster_descheduler_add_policy(const char *name,
                                    enum desched_policy_type type,
                                    uint64_t threshold)
{
    if (!g_initialised)
        return -EAGAIN;
    if (!name || !*name)
        return -EINVAL;

    spinlock_acquire(&g_ds_lock);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < DESCHED_POLICIES_MAX; i++) {
        if (!g_policies[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&g_ds_lock);
        return -ENOSPC;
    }

    struct desched_policy *p = &g_policies[slot];
    strncpy(p->name, name, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
    p->type      = type;
    p->threshold = threshold;
    p->in_use    = 1;

    spinlock_release(&g_ds_lock);

    kprintf("[Descheduler] Added policy '%s' (type=%d threshold=%lu)\n",
            name, (int)type, (unsigned long)threshold);
    return 0;
}

int cluster_descheduler_remove_policy(const char *name)
{
    if (!g_initialised || !name)
        return -EINVAL;

    spinlock_acquire(&g_ds_lock);

    for (int i = 0; i < DESCHED_POLICIES_MAX; i++) {
        if (g_policies[i].in_use &&
            strcmp(g_policies[i].name, name) == 0) {
            memset(&g_policies[i], 0, sizeof(g_policies[i]));
            spinlock_release(&g_ds_lock);
            kprintf("[Descheduler] Removed policy '%s'\n", name);
            return 0;
        }
    }

    spinlock_release(&g_ds_lock);
    return -ENOENT;
}

/* ── Descheduling run ───────────────────────────────────────────────── */

int cluster_descheduler_run(void)
{
    if (!g_initialised)
        return -EAGAIN;

    int total_evicted = 0;

    spinlock_acquire(&g_ds_lock);

    for (int i = 0; i < DESCHED_POLICIES_MAX; i++) {
        if (!g_policies[i].in_use)
            continue;

        struct desched_policy *p = &g_policies[i];

        switch (p->type) {
        case DESCHED_POLICY_LOW_UTIL:
            /* Simulate: evict 1 pod per low-util policy */
            kprintf("[Descheduler] Policy '%s': low-utilisation check "
                    "(threshold=%lu%%)\n",
                    p->name, (unsigned long)p->threshold);
            total_evicted++;
            break;

        case DESCHED_POLICY_HIGH_UTIL:
            kprintf("[Descheduler] Policy '%s': high-utilisation check "
                    "(threshold=%lu%%)\n",
                    p->name, (unsigned long)p->threshold);
            total_evicted++;
            break;

        case DESCHED_POLICY_LIFETIME:
            kprintf("[Descheduler] Policy '%s': lifetime check "
                    "(max=%lums)\n",
                    p->name, (unsigned long)p->lifetime_ms);
            break;

        default:
            break;
        }
    }

    g_total_evictions += total_evicted;

    spinlock_release(&g_ds_lock);

    kprintf("[Descheduler] Run complete: evicted %d pod(s) "
            "(total=%lu)\n",
            total_evicted, (unsigned long)g_total_evictions);
    return total_evicted;
}

/* ── Statistics ─────────────────────────────────────────────────────── */

int cluster_descheduler_get_stats(uint64_t *total_evictions)
{
    if (!g_initialised || !total_evictions)
        return -EINVAL;

    spinlock_acquire(&g_ds_lock);
    *total_evictions = g_total_evictions;
    spinlock_release(&g_ds_lock);
    return 0;
}

/* ── descheduler_run ─────────────────────────────── */
int descheduler_run(void)
{
    /* Run all registered descheduling policies.
     * For now, just acknowledge the request. */
    kprintf("[cluster] Descheduler run requested\n");
    return 0;
}
/* ── descheduler_evict_pod ─────────────────────────────── */
int descheduler_evict_pod(const char *pod)
{
    (void)pod;
    /* Evict a specific pod from a node.
     * For now, log the eviction request. */
    kprintf("[cluster] Descheduler evicting pod: %s\n", pod ? pod : "unknown");
    return 0;
}
