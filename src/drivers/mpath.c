/*
 * mpath.c — Multipath I/O (path failover, selectors)
 *
 * Implements multipath block I/O with automatic path failover and
 * multiple path selector algorithms:
 *   - round-robin (default)
 *   - least-queued (selects the path with fewest outstanding I/Os)
 *   - service-time (selects path with lowest estimated service time)
 *
 * Multipath groups paths to the same logical block device, providing
 * redundancy and load balancing. When a path fails, I/O is transparently
 * redirected to an alternative path.
 *
 * Item 451: Multipath I/O
 */

#define KERNEL_INTERNAL
#include "mpath.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"
#include "timer.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define MPATH_MAX_PATHS    8   /* maximum paths per multipath device */
#define MPATH_MAX_DEVS     4   /* maximum multipath devices */
#define MPATH_NAME_MAX     16

/* Path states */
#define MPATH_PATH_ACTIVE   0   /* path is operational */
#define MPATH_PATH_FAILED   1   /* path has failed */
#define MPATH_PATH_RECOVERING 2 /* path is being tested after failure */

/* Selector types */
#define MPATH_SEL_ROUND_ROBIN   0
#define MPATH_SEL_LEAST_QUEUED  1
#define MPATH_SEL_SERVICE_TIME  2

/* ── Per-path structure ────────────────────────────────────────────── */

struct mpath_path {
    int     dev_id;             /* block device ID of this path */
    int     state;              /* MPATH_PATH_* */
    uint64_t outstanding;       /* number of in-flight I/Os */
    uint64_t service_time_ns;   /* estimated service time (ns) */
    uint64_t fail_count;        /* number of failures */
    uint64_t last_fail_tick;    /* timer tick of last failure */
    int     in_use;
};

/* ── Multipath device ──────────────────────────────────────────────── */

struct mpath_dev {
    char    name[MPATH_NAME_MAX];
    int     in_use;
    int     selector;           /* MPATH_SEL_* */
    int     num_paths;
    struct mpath_path paths[MPATH_MAX_PATHS];
    int     rr_next;            /* round-robin next index */
    spinlock_t lock;
};

/* ── Global state ─────────────────────────────────────────────────── */

static struct mpath_dev g_mpath_devs[MPATH_MAX_DEVS];
static int g_mpath_initialized = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  Path selector algorithms
 * ═══════════════════════════════════════════════════════════════════════ */

/* Round-robin path selector */
static int selector_round_robin(struct mpath_dev *mp)
{
    for (int i = 0; i < mp->num_paths; i++) {
        int idx = (mp->rr_next + i) % mp->num_paths;
        if (mp->paths[idx].state == MPATH_PATH_ACTIVE &&
            mp->paths[idx].in_use) {
            mp->rr_next = (idx + 1) % mp->num_paths;
            return idx;
        }
    }
    return -1;
}

/* Least-queued path selector */
static int selector_least_queued(struct mpath_dev *mp)
{
    int best = -1;
    uint64_t min_q = UINT64_MAX;
    for (int i = 0; i < mp->num_paths; i++) {
        if (mp->paths[i].state == MPATH_PATH_ACTIVE &&
            mp->paths[i].in_use) {
            if (mp->paths[i].outstanding < min_q) {
                min_q = mp->paths[i].outstanding;
                best = i;
            }
        }
    }
    if (best >= 0)
        mp->rr_next = (best + 1) % mp->num_paths;
    return best;
}

/* Service-time path selector */
static int selector_service_time(struct mpath_dev *mp)
{
    int best = -1;
    uint64_t best_st = UINT64_MAX;
    for (int i = 0; i < mp->num_paths; i++) {
        if (mp->paths[i].state == MPATH_PATH_ACTIVE &&
            mp->paths[i].in_use) {
            uint64_t st = mp->paths[i].service_time_ns *
                         (mp->paths[i].outstanding + 1);
            if (st < best_st) {
                best_st = st;
                best = i;
            }
        }
    }
    if (best >= 0)
        mp->rr_next = (best + 1) % mp->num_paths;
    return best;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API — Multipath device management
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize the multipath subsystem. */
void mpath_init(void)
{
    if (g_mpath_initialized) return;

    memset(g_mpath_devs, 0, sizeof(g_mpath_devs));
    g_mpath_initialized = 1;
    kprintf("[OK] Multipath I/O initialized\n");
}
EXPORT_SYMBOL(mpath_init);

/* Create a multipath device.
 * @name: device name (e.g., "mpatha")
 * @selector: MPATH_SEL_ROUND_ROBIN, MPATH_SEL_LEAST_QUEUED, or MPATH_SEL_SERVICE_TIME
 * Returns device index (>= 0) on success, negative on failure. */
int mpath_create(const char *name, int selector)
{
    if (!name || !g_mpath_initialized)
        return -EINVAL;

    for (int i = 0; i < MPATH_MAX_DEVS; i++) {
        if (!g_mpath_devs[i].in_use) {
            struct mpath_dev *mp = &g_mpath_devs[i];
            memset(mp, 0, sizeof(*mp));
            strncpy(mp->name, name, MPATH_NAME_MAX - 1);
            mp->name[MPATH_NAME_MAX - 1] = '\0';
            mp->in_use = 1;
            mp->selector = (selector >= 0 && selector <= MPATH_SEL_SERVICE_TIME)
                           ? selector : MPATH_SEL_ROUND_ROBIN;
            mp->rr_next = 0;
            spinlock_init(&mp->lock);
            kprintf("[MPATH] created '%s' (selector=%d)\n", name, mp->selector);
            return i;
        }
    }
    return -ENOSPC;
}
EXPORT_SYMBOL(mpath_create);

/* Destroy a multipath device.
 * Returns 0 on success. */
int mpath_destroy(int mpath_id)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -EINVAL;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    spinlock_acquire(&mp->lock);
    memset(mp, 0, sizeof(*mp));
    spinlock_release(&mp->lock);
    return 0;
}
EXPORT_SYMBOL(mpath_destroy);

/* Add a path to a multipath device.
 * @dev_id: block device ID of the path
 * Returns 0 on success, negative on failure. */
int mpath_add_path(int mpath_id, int dev_id)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -EINVAL;
    if (!blockdev_is_registered(dev_id))
        return -ENODEV;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    spinlock_acquire(&mp->lock);

    /* Check for duplicates */
    for (int i = 0; i < mp->num_paths; i++) {
        if (mp->paths[i].dev_id == dev_id) {
            spinlock_release(&mp->lock);
            return -EEXIST;
        }
    }

    if (mp->num_paths >= MPATH_MAX_PATHS) {
        spinlock_release(&mp->lock);
        return -ENOSPC;
    }

    struct mpath_path *p = &mp->paths[mp->num_paths];
    memset(p, 0, sizeof(*p));
    p->dev_id = dev_id;
    p->state = MPATH_PATH_ACTIVE;
    p->in_use = 1;
    mp->num_paths++;

    kprintf("[MPATH] added path dev_id=%d to '%s' (total %d)\n",
            dev_id, mp->name, mp->num_paths);
    spinlock_release(&mp->lock);
    return 0;
}
EXPORT_SYMBOL(mpath_add_path);

/* Remove a path from a multipath device.
 * Returns 0 on success. */
int mpath_remove_path(int mpath_id, int dev_id)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -EINVAL;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    spinlock_acquire(&mp->lock);

    for (int i = 0; i < mp->num_paths; i++) {
        if (mp->paths[i].dev_id == dev_id) {
            /* Shift remaining paths */
            for (int j = i; j < mp->num_paths - 1; j++)
                mp->paths[j] = mp->paths[j + 1];
            mp->num_paths--;
            spinlock_release(&mp->lock);
            return 0;
        }
    }
    spinlock_release(&mp->lock);
    return -ENOENT;
}
EXPORT_SYMBOL(mpath_remove_path);

/* Select a path for the next I/O request.
 * Returns the block device ID on success, -1 on failure (all paths down). */
int mpath_select_path(int mpath_id)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -1;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    spinlock_acquire(&mp->lock);

    int path_idx = -1;
    switch (mp->selector) {
    case MPATH_SEL_ROUND_ROBIN:
        path_idx = selector_round_robin(mp);
        break;
    case MPATH_SEL_LEAST_QUEUED:
        path_idx = selector_least_queued(mp);
        break;
    case MPATH_SEL_SERVICE_TIME:
        path_idx = selector_service_time(mp);
        break;
    default:
        path_idx = selector_round_robin(mp);
        break;
    }

    if (path_idx >= 0) {
        mp->paths[path_idx].outstanding++;
        int dev_id = mp->paths[path_idx].dev_id;
        spinlock_release(&mp->lock);
        return dev_id;
    }

    spinlock_release(&mp->lock);
    return -1; /* all paths down */
}
EXPORT_SYMBOL(mpath_select_path);

/* Complete an I/O on a path (called after I/O finishes).
 * Updates statistics. */
void mpath_complete_io(int mpath_id, int dev_id, int success,
                       uint64_t start_tick)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    spinlock_acquire(&mp->lock);

    for (int i = 0; i < mp->num_paths; i++) {
        if (mp->paths[i].dev_id == dev_id) {
            if (mp->paths[i].outstanding > 0)
                mp->paths[i].outstanding--;

            if (success) {
                /* Update service time estimate (exponential weighted moving avg) */
                uint64_t elapsed = timer_get_ticks() - start_tick;
                uint64_t elapsed_ns = elapsed * 1000000000ULL / TIMER_FREQ;
                if (mp->paths[i].service_time_ns == 0)
                    mp->paths[i].service_time_ns = elapsed_ns;
                else
                    mp->paths[i].service_time_ns =
                        (mp->paths[i].service_time_ns * 7 + elapsed_ns * 3) / 10;
            } else {
                mp->paths[i].fail_count++;
                mp->paths[i].last_fail_tick = timer_get_ticks();
                mp->paths[i].state = MPATH_PATH_FAILED;
            }
            break;
        }
    }

    spinlock_release(&mp->lock);
}
EXPORT_SYMBOL(mpath_complete_io);

/* Set the path selector algorithm for a multipath device. */
int mpath_set_selector(int mpath_id, int selector)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -EINVAL;
    if (selector < MPATH_SEL_ROUND_ROBIN || selector > MPATH_SEL_SERVICE_TIME)
        return -EINVAL;

    g_mpath_devs[mpath_id].selector = selector;
    return 0;
}
EXPORT_SYMBOL(mpath_set_selector);

/* Fail a path (mark as failed) — called by error detection. */
int mpath_fail_path(int mpath_id, int dev_id)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -EINVAL;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    spinlock_acquire(&mp->lock);
    for (int i = 0; i < mp->num_paths; i++) {
        if (mp->paths[i].dev_id == dev_id) {
            mp->paths[i].state = MPATH_PATH_FAILED;
            mp->paths[i].fail_count++;
            spinlock_release(&mp->lock);
            return 0;
        }
    }
    spinlock_release(&mp->lock);
    return -ENOENT;
}
EXPORT_SYMBOL(mpath_fail_path);

/* Restore a path (mark as active after recovery). */
int mpath_restore_path(int mpath_id, int dev_id)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -EINVAL;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    spinlock_acquire(&mp->lock);
    for (int i = 0; i < mp->num_paths; i++) {
        if (mp->paths[i].dev_id == dev_id) {
            mp->paths[i].state = MPATH_PATH_ACTIVE;
            spinlock_release(&mp->lock);
            return 0;
        }
    }
    spinlock_release(&mp->lock);
    return -ENOENT;
}
EXPORT_SYMBOL(mpath_restore_path);

/* Query multipath device status. */
int mpath_status(int mpath_id, char *buf, int max)
{
    if (mpath_id < 0 || mpath_id >= MPATH_MAX_DEVS ||
        !g_mpath_devs[mpath_id].in_use)
        return -EINVAL;

    struct mpath_dev *mp = &g_mpath_devs[mpath_id];
    int pos = 0;

    spinlock_acquire(&mp->lock);
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "mpath '%s': %d paths, selector %d\n",
                    mp->name, mp->num_paths, mp->selector);
    for (int i = 0; i < mp->num_paths && pos < max; i++) {
        struct mpath_path *p = &mp->paths[i];
        const char *state_str = "active";
        if (p->state == MPATH_PATH_FAILED) state_str = "FAILED";
        else if (p->state == MPATH_PATH_RECOVERING) state_str = "recovering";
        pos += snprintf(buf + pos, (size_t)(max - pos),
                        "  path %d: dev=%d state=%s out=%llu fails=%llu svc=%lluns\n",
                        i, p->dev_id, state_str,
                        (unsigned long long)p->outstanding,
                        (unsigned long long)p->fail_count,
                        (unsigned long long)p->service_time_ns);
    }
    spinlock_release(&mp->lock);
    return pos;
}
#include "module.h"
module_init(mpath_init);

/* ── Stub: mpath_failover ─────────────────────────────── */
int mpath_failover(__maybe_unused const char *dev)
{
    kprintf("[MPATH] mpath_failover: not yet implemented\n");
    return 0;
}
