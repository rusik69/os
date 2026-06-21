/*
 * upgrade.c — Cluster rolling upgrades and rollback (C184–C185)
 *
 * Implements:
 *   C184: Cluster rolling upgrades — cordon, drain, upgrade, uncordon
 *   C185: Upgrade rollback — revert to previous version on failure
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"
#include "socket.h"
#include "net.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define NODE_MAX               32
#define VERSION_MAX            32
#define NODE_STATUS_LEN        32
#define UPGRADE_PHASE_NAME_MAX 16

/* ── Phase enum ──────────────────────────────────────────────────────── */

enum upgrade_phase {
    UPGRADE_PHASE_IDLE       = 0,
    UPGRADE_PHASE_CORDON     = 1,
    UPGRADE_PHASE_DRAIN      = 2,
    UPGRADE_PHASE_UPGRADE    = 3,
    UPGRADE_PHASE_UNCORDON   = 4,
    UPGRADE_PHASE_ROLLBACK   = 5,
};

static const char *const upgrade_phase_names[] = {
    [UPGRADE_PHASE_IDLE]     = "idle",
    [UPGRADE_PHASE_CORDON]   = "cordon",
    [UPGRADE_PHASE_DRAIN]    = "drain",
    [UPGRADE_PHASE_UPGRADE]  = "upgrade",
    [UPGRADE_PHASE_UNCORDON] = "uncordon",
    [UPGRADE_PHASE_ROLLBACK] = "rollback",
};

/* ── Node status strings ─────────────────────────────────────────────── */

struct node_upgrade_status {
    char schedulable;
    char drained;
    char upgraded;
};

/* ── Upgrade state ───────────────────────────────────────────────────── */

struct upgrade_state {
    char   current_version[VERSION_MAX];
    char   target_version[VERSION_MAX];
    char   previous_version[VERSION_MAX];   /* for rollback */
    struct node_upgrade_status node_status[NODE_MAX];
    enum upgrade_phase        phase;
    int                       node_count;
    int                       current_node;  /* index of node being processed */
    spinlock_t                lock;
    int                       initialised;
};

static struct upgrade_state g_upgrade;

/* ── Forward declarations of node/pod helpers ────────────────────────── */

/* These would normally be linked from node.c / orch.c.
 * Stub implementations are provided for compile-time completeness. */
extern int node_set_schedulable(int node_id, int schedulable);
extern int pod_evict_on_node(int node_id);
extern int node_signal_upgrade(int node_id, const char *version);

/* ── Helpers ─────────────────────────────────────────────────────────── */

static const char *phase_name(enum upgrade_phase p)
{
    if (p >= 0 && (size_t)p < sizeof(upgrade_phase_names) / sizeof(upgrade_phase_names[0]))
        return upgrade_phase_names[p] ? upgrade_phase_names[p] : "unknown";
    return "unknown";
}

/* ── C184: Upgrade initialisation ────────────────────────────────────── */

int upgrade_init(void)
{
    memset(&g_upgrade, 0, sizeof(g_upgrade));
    g_upgrade.phase = UPGRADE_PHASE_IDLE;
    g_upgrade.node_count = 0;
    g_upgrade.current_node = -1;
    g_upgrade.initialised = 1;
    kprintf("[Upgrade] Cluster upgrade subsystem initialised\n");
    return 0;
}

/* ── C184: Begin rolling upgrade ─────────────────────────────────────── */

int upgrade_begin(const char *target_version)
{
    if (!target_version || !g_upgrade.initialised)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);

    if (g_upgrade.phase != UPGRADE_PHASE_IDLE) {
        spinlock_release(&g_upgrade.lock);
        return -EBUSY;
    }

    size_t tlen = strlen(target_version);
    if (tlen == 0 || tlen >= VERSION_MAX) {
        spinlock_release(&g_upgrade.lock);
        return -EINVAL;
    }

    /* Save previous version for rollback */
    strncpy(g_upgrade.previous_version, g_upgrade.current_version, VERSION_MAX - 1);
    g_upgrade.previous_version[VERSION_MAX - 1] = '\0';

    strncpy(g_upgrade.target_version, target_version, VERSION_MAX - 1);
    g_upgrade.target_version[VERSION_MAX - 1] = '\0';

    g_upgrade.phase = UPGRADE_PHASE_CORDON;
    g_upgrade.current_node = 0;

    /* Mark all nodes as schedulable initially */
    for (int i = 0; i < NODE_MAX; i++) {
        g_upgrade.node_status[i].schedulable = 1;
        g_upgrade.node_status[i].drained = 0;
        g_upgrade.node_status[i].upgraded = 0;
    }

    kprintf("[Upgrade] Beginning rolling upgrade to version %s (previous: %s)\n",
            g_upgrade.target_version, g_upgrade.previous_version);

    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── C184: Cordon a node (mark unschedulable) ────────────────────────── */

int upgrade_cordon_node(int node_id)
{
    if (!g_upgrade.initialised)
        return -EINVAL;
    if (node_id < 0 || node_id >= NODE_MAX)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);

    if (g_upgrade.phase != UPGRADE_PHASE_CORDON) {
        spinlock_release(&g_upgrade.lock);
        return -EPERM;
    }

    g_upgrade.node_status[node_id].schedulable = 0;

    int ret = node_set_schedulable(node_id, 0);
    if (ret < 0) {
        kprintf("[Upgrade] Failed to cordon node %d: %d\n", node_id, ret);
        spinlock_release(&g_upgrade.lock);
        return ret;
    }

    kprintf("[Upgrade] Node %d cordoned (unschedulable)\n", node_id);
    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── C184: Drain a node (evict pods gracefully) ──────────────────────── */

int upgrade_drain_node(int node_id)
{
    if (!g_upgrade.initialised)
        return -EINVAL;
    if (node_id < 0 || node_id >= NODE_MAX)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);

    if (g_upgrade.phase != UPGRADE_PHASE_DRAIN) {
        spinlock_release(&g_upgrade.lock);
        return -EPERM;
    }

    /* Evict all pods from this node */
    int ret = pod_evict_on_node(node_id);
    if (ret < 0) {
        kprintf("[Upgrade] Failed to drain node %d: %d\n", node_id, ret);
        spinlock_release(&g_upgrade.lock);
        return ret;
    }

    g_upgrade.node_status[node_id].drained = 1;
    kprintf("[Upgrade] Node %d drained (pods evicted)\n", node_id);

    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── C184: Apply upgrade to a node ───────────────────────────────────── */

int upgrade_apply_node(int node_id)
{
    if (!g_upgrade.initialised)
        return -EINVAL;
    if (node_id < 0 || node_id >= NODE_MAX)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);

    if (g_upgrade.phase != UPGRADE_PHASE_UPGRADE) {
        spinlock_release(&g_upgrade.lock);
        return -EPERM;
    }

    /* Signal the node to perform its upgrade */
    int ret = node_signal_upgrade(node_id, g_upgrade.target_version);
    if (ret < 0) {
        kprintf("[Upgrade] Failed to upgrade node %d: %d\n", node_id, ret);
        spinlock_release(&g_upgrade.lock);
        return ret;
    }

    g_upgrade.node_status[node_id].upgraded = 1;
    g_upgrade.current_node = node_id;

    kprintf("[Upgrade] Node %d upgraded to version %s\n",
            node_id, g_upgrade.target_version);

    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── C184: Unordon a node (mark schedulable) ─────────────────────────── */

int upgrade_uncordon_node(int node_id)
{
    if (!g_upgrade.initialised)
        return -EINVAL;
    if (node_id < 0 || node_id >= NODE_MAX)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);

    if (g_upgrade.phase != UPGRADE_PHASE_UNCORDON) {
        spinlock_release(&g_upgrade.lock);
        return -EPERM;
    }

    g_upgrade.node_status[node_id].schedulable = 1;

    int ret = node_set_schedulable(node_id, 1);
    if (ret < 0) {
        kprintf("[Upgrade] Failed to uncordon node %d: %d\n", node_id, ret);
        spinlock_release(&g_upgrade.lock);
        return ret;
    }

    kprintf("[Upgrade] Node %d uncordoned (schedulable)\n", node_id);
    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── C185: Rollback upgrade ──────────────────────────────────────────── */

int upgrade_rollback(void)
{
    if (!g_upgrade.initialised)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);

    if (g_upgrade.phase == UPGRADE_PHASE_IDLE) {
        spinlock_release(&g_upgrade.lock);
        return -EPERM;
    }

    enum upgrade_phase saved_phase = g_upgrade.phase;
    g_upgrade.phase = UPGRADE_PHASE_ROLLBACK;

    kprintf("[Upgrade] Rolling back from %s to %s (phase was %s)\n",
            g_upgrade.target_version,
            g_upgrade.previous_version[0] ? g_upgrade.previous_version : "(none)",
            phase_name(saved_phase));

    /* For each node that was upgraded, signal rollback */
    for (int i = 0; i < NODE_MAX; i++) {
        if (g_upgrade.node_status[i].upgraded) {
            int ret = node_signal_upgrade(i, g_upgrade.previous_version);
            if (ret < 0) {
                kprintf("[Upgrade] Rollback failed on node %d: %d\n", i, ret);
            } else {
                g_upgrade.node_status[i].upgraded = 0;
                kprintf("[Upgrade] Node %d rolled back to %s\n",
                        i, g_upgrade.previous_version);
            }
        }
    }

    /* Restore previous version */
    strncpy(g_upgrade.current_version, g_upgrade.previous_version, VERSION_MAX - 1);
    g_upgrade.current_version[VERSION_MAX - 1] = '\0';
    g_upgrade.previous_version[0] = '\0';

    /* Mark all nodes schedulable */
    for (int i = 0; i < NODE_MAX; i++) {
        g_upgrade.node_status[i].schedulable = 1;
        g_upgrade.node_status[i].drained = 0;
        g_upgrade.node_status[i].upgraded = 0;
        node_set_schedulable(i, 1);
    }

    g_upgrade.phase = UPGRADE_PHASE_IDLE;
    g_upgrade.current_node = -1;

    kprintf("[Upgrade] Rollback complete\n");
    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── Utility: query current upgrade state ────────────────────────────── */

int upgrade_status(struct upgrade_state *out)
{
    if (!out || !g_upgrade.initialised)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);
    memcpy(out, &g_upgrade, sizeof(*out));
    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── Advance the rolling upgrade to the next phase ───────────────────── */

int upgrade_next_phase(void)
{
    if (!g_upgrade.initialised)
        return -EINVAL;

    spinlock_acquire(&g_upgrade.lock);

    if (g_upgrade.phase == UPGRADE_PHASE_IDLE) {
        spinlock_release(&g_upgrade.lock);
        return -EPERM;
    }

    enum upgrade_phase next;

    switch (g_upgrade.phase) {
    case UPGRADE_PHASE_CORDON:
        next = UPGRADE_PHASE_DRAIN;
        break;
    case UPGRADE_PHASE_DRAIN:
        next = UPGRADE_PHASE_UPGRADE;
        break;
    case UPGRADE_PHASE_UPGRADE:
        next = UPGRADE_PHASE_UNCORDON;
        break;
    case UPGRADE_PHASE_UNCORDON:
        /* All nodes processed — upgrade complete */
        strncpy(g_upgrade.current_version, g_upgrade.target_version, VERSION_MAX - 1);
        g_upgrade.current_version[VERSION_MAX - 1] = '\0';
        g_upgrade.target_version[0] = '\0';
        g_upgrade.phase = UPGRADE_PHASE_IDLE;
        g_upgrade.current_node = -1;
        kprintf("[Upgrade] Rolling upgrade to %s complete\n", g_upgrade.current_version);
        spinlock_release(&g_upgrade.lock);
        return 0;
    default:
        spinlock_release(&g_upgrade.lock);
        return -EINVAL;
    }

    kprintf("[Upgrade] Advancing phase: %s -> %s\n",
            phase_name(g_upgrade.phase), phase_name(next));
    g_upgrade.phase = next;
    g_upgrade.current_node = 0;

    spinlock_release(&g_upgrade.lock);
    return 0;
}

/* ── Stub: upgrade_apply ─────────────────────────────── */
int upgrade_apply(const char *version)
{
    (void)version;
    kprintf("[cluster] upgrade_apply: not yet implemented\n");
    return -ENOSYS;
}
