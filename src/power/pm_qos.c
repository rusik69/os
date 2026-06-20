/*
 * pm_qos.c — Power Management Quality of Service
 *
 * Provides a lightweight framework for kernel components (drivers,
 * subsystems) to register latency constraints that influence cpuidle
 * C-state selection.  The effective constraint is the MINIMUM of all
 * registered requests; cpuidle skips any state whose wakeup latency
 * exceeds the effective constraint.
 *
 * Supports two types of constraints:
 *   1. Global latency constraints (original API)
 *   2. Per-device latency constraints with resume latency and throughput
 *
 * Design:
 *   - Simple fixed-size array of requests (no dynamic allocation).
 *   - Locking via spinlock (PM QoS is queried on the idle path, but
 *     the fast path is a single integer READ_ONCE-like load).
 *   - Requests are identified by an integer ID which is their index
 *     in the array (constant-time lookup).
 *   - Per-device constraints use a separate table with device tracking.
 *
 * Reference: Linux kernel's pm_qos API (drivers/base/power/qos.c).
 *
 * Item 123 — PM QoS with per-device latency constraints
 */

#define KERNEL_INTERNAL
#include "pm_qos.h"
#include "spinlock.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ── Notifier support ───────────────────────────────────────────────── */

/** Max number of registered PM QoS notifiers */
#define PM_QOS_NOTIFIER_MAX 16

/** Notifier callback type */
typedef void (*pm_qos_notifier_fn_t)(uint32_t old_effective, uint32_t new_effective, void *data);

/** A registered notifier entry */
struct pm_qos_notifier {
    pm_qos_notifier_fn_t fn;
    void                *data;
    int                  used;
};

static struct pm_qos_notifier pm_qos_notifiers[PM_QOS_NOTIFIER_MAX];

/* ── Internal data structures ─────────────────────────────────────── */

/** A single PM QoS latency request. */
struct pm_qos_request {
    char      name[PM_QOS_NAME_MAX];  /* Human-readable name for debug */
    uint32_t  latency_us;             /* Max allowed wakeup latency (us) */
    int       used;                   /* 1 = slot in use, 0 = free */
};

/* ── Per-device PM QoS constraint ─────────────────────────────────── */

#define PM_QOS_MAX_DEVICE_REQUESTS 16

/* Types of per-device constraints */
#define PM_QOS_DEV_RESUME_LATENCY  0   /* Resume latency in us */
#define PM_QOS_DEV_THROUGHPUT      1   /* Throughput requirement (MB/s) */

struct pm_qos_device_request {
    char      dev_name[PM_QOS_NAME_MAX];  /* Device name */
    int       type;                        /* PM_QOS_DEV_RESUME_LATENCY or PM_QOS_DEV_THROUGHPUT */
    uint32_t  value;                       /* Constraint value */
    int       used;                        /* 1 = slot in use */
};

/** Global PM QoS state. */
static struct {
    struct pm_qos_request        requests[PM_QOS_MAX_REQUESTS];
    struct pm_qos_device_request device_requests[PM_QOS_MAX_DEVICE_REQUESTS];
    spinlock_t            lock;
    int                   initialized;
} pm_qos_state;

/* ═══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * Compute the effective latency constraint as the minimum of all
 * registered requests.  Must be called with the lock held.
 * Returns the new effective value.
 */
static uint32_t pm_qos_compute_effective_locked(void)
{
    uint32_t effective = PM_QOS_NO_CONSTRAINT;

    for (int i = 0; i < PM_QOS_MAX_REQUESTS; i++) {
        if (pm_qos_state.requests[i].used &&
            pm_qos_state.requests[i].latency_us < effective) {
            effective = pm_qos_state.requests[i].latency_us;
        }
    }

    return effective;
}

/**
 * Notify all registered notifiers of a change in the effective constraint.
 * Must be called with the lock held.
 */
static void pm_qos_notify_locked(uint32_t old_effective, uint32_t new_effective)
{
    if (old_effective == new_effective)
        return;

    for (int i = 0; i < PM_QOS_NOTIFIER_MAX; i++) {
        if (pm_qos_notifiers[i].used && pm_qos_notifiers[i].fn)
            pm_qos_notifiers[i].fn(old_effective, new_effective,
                                   pm_qos_notifiers[i].data);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void pm_qos_init(void)
{
    if (pm_qos_state.initialized)
        return;

    memset(&pm_qos_state, 0, sizeof(pm_qos_state));
    spinlock_init(&pm_qos_state.lock);
    pm_qos_state.initialized = 1;

    kprintf("[PM QoS] Initialised (%d slots, default constraint: none)\n",
            PM_QOS_MAX_REQUESTS);
}

int pm_qos_add_request(const char *name, uint32_t latency_us)
{
    if (!pm_qos_state.initialized)
        return -ENOSYS;

    if (!name || !name[0])
        return -EINVAL;

    spinlock_acquire(&pm_qos_state.lock);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < PM_QOS_MAX_REQUESTS; i++) {
        if (!pm_qos_state.requests[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&pm_qos_state.lock);
        kprintf("[PM QoS] ERROR: No free request slots (max %d)\n",
                PM_QOS_MAX_REQUESTS);
        return -ENOSPC;
    }

    /* Capture effective constraint before adding */
    uint32_t old_effective = pm_qos_compute_effective_locked();

    /* Fill the slot */
    struct pm_qos_request *req = &pm_qos_state.requests[slot];
    strncpy(req->name, name, PM_QOS_NAME_MAX - 1);
    req->name[PM_QOS_NAME_MAX - 1] = '\0';
    req->latency_us = latency_us;
    req->used       = 1;

    uint32_t effective = pm_qos_compute_effective_locked();
    pm_qos_notify_locked(old_effective, effective);

    spinlock_release(&pm_qos_state.lock);

    kprintf("[PM QoS] Request #%d: \"%s\" latency=%u us (effective=%u us)\n",
            slot, name, (unsigned)latency_us, (unsigned)effective);

    return slot;
}

int pm_qos_update_request(int id, uint32_t latency_us)
{
    if (!pm_qos_state.initialized)
        return -ENOSYS;

    if (id < 0 || id >= PM_QOS_MAX_REQUESTS)
        return -ENOENT;

    spinlock_acquire(&pm_qos_state.lock);

    if (!pm_qos_state.requests[id].used) {
        spinlock_release(&pm_qos_state.lock);
        return -ENOENT;
    }

    uint32_t old_effective = pm_qos_compute_effective_locked();

    pm_qos_state.requests[id].latency_us = latency_us;
    uint32_t effective = pm_qos_compute_effective_locked();
    pm_qos_notify_locked(old_effective, effective);

    spinlock_release(&pm_qos_state.lock);

    kprintf("[PM QoS] Request #%d updated: latency=%u us (effective=%u us)\n",
            id, (unsigned)latency_us, (unsigned)effective);

    return 0;
}

int pm_qos_remove_request(int id)
{
    if (!pm_qos_state.initialized)
        return -ENOSYS;

    if (id < 0 || id >= PM_QOS_MAX_REQUESTS)
        return -ENOENT;

    spinlock_acquire(&pm_qos_state.lock);

    if (!pm_qos_state.requests[id].used) {
        spinlock_release(&pm_qos_state.lock);
        return -ENOENT;
    }

    const char *name = pm_qos_state.requests[id].name;

    uint32_t old_effective = pm_qos_compute_effective_locked();

    pm_qos_state.requests[id].used = 0;
    pm_qos_state.requests[id].latency_us = 0;
    memset(pm_qos_state.requests[id].name, 0, PM_QOS_NAME_MAX);

    uint32_t effective = pm_qos_compute_effective_locked();
    pm_qos_notify_locked(old_effective, effective);

    spinlock_release(&pm_qos_state.lock);

    kprintf("[PM QoS] Request #%d \"%s\" removed (effective=%u us)\n",
            id, name, (unsigned)effective);

    return 0;
}

uint32_t pm_qos_read_effective_latency(void)
{
    if (!pm_qos_state.initialized)
        return PM_QOS_NO_CONSTRAINT;

    /*
     * Fast path: the effective latency changes infrequently (only when
     * requests are added/updated/removed).  We compute it lazily and
     * cache the result; for simplicity we re-compute each time under
     * lock.  The lock is held only briefly and this function is called
     * from the idle path, which is inherently non-performance-critical.
     *
     * Future optimisation: maintain a cached effective value updated
     * on every request change, and here just READ_ONCE it.
     */
    spinlock_acquire(&pm_qos_state.lock);
    uint32_t effective = pm_qos_compute_effective_locked();
    spinlock_release(&pm_qos_state.lock);

    return effective;
}

int pm_qos_num_requests(void)
{
    if (!pm_qos_state.initialized)
        return 0;

    spinlock_acquire(&pm_qos_state.lock);
    int count = 0;
    for (int i = 0; i < PM_QOS_MAX_REQUESTS; i++) {
        if (pm_qos_state.requests[i].used)
            count++;
    }
    spinlock_release(&pm_qos_state.lock);

    return count;
}

void pm_qos_dump_requests(void)
{
    if (!pm_qos_state.initialized) {
        kprintf("[PM QoS] Not initialised\n");
        return;
    }

    spinlock_acquire(&pm_qos_state.lock);

    kprintf("[PM QoS] Latency requests (%d active):\n", pm_qos_num_requests());
    kprintf("  %-4s %-32s %s\n", "ID", "Name", "Latency (us)");
    kprintf("  %-4s %-32s %s\n", "----", "--------------------------------", "------------");

    for (int i = 0; i < PM_QOS_MAX_REQUESTS; i++) {
        if (pm_qos_state.requests[i].used) {
            kprintf("  %-4d %-32s %u\n",
                    i,
                    pm_qos_state.requests[i].name,
                    (unsigned)pm_qos_state.requests[i].latency_us);
        }
    }

    uint32_t effective = pm_qos_compute_effective_locked();
    kprintf("  Effective constraint: %u us\n", (unsigned)effective);

    /* Dump per-device requests */
    kprintf("  Per-device constraints (%d):\n", pm_qos_num_device_requests());
    kprintf("  %-4s %-32s %-16s %s\n", "ID", "Device", "Type", "Value");
    for (int i = 0; i < PM_QOS_MAX_DEVICE_REQUESTS; i++) {
        if (!pm_qos_state.device_requests[i].used) continue;
        const char *type_str = "?";
        if (pm_qos_state.device_requests[i].type == PM_QOS_DEV_RESUME_LATENCY)
            type_str = "resume_lat";
        else if (pm_qos_state.device_requests[i].type == PM_QOS_DEV_THROUGHPUT)
            type_str = "throughput";
        kprintf("  %-4d %-32s %-16s %u\n",
                i,
                pm_qos_state.device_requests[i].dev_name,
                type_str,
                (unsigned)pm_qos_state.device_requests[i].value);
    }

    spinlock_release(&pm_qos_state.lock);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Notifier API
 * ═══════════════════════════════════════════════════════════════════════ */

int pm_qos_add_notifier(pm_qos_notifier_fn_t fn, void *data)
{
    if (!fn)
        return -EINVAL;
    if (!pm_qos_state.initialized)
        return -ENOSYS;

    spinlock_acquire(&pm_qos_state.lock);

    int slot = -1;
    for (int i = 0; i < PM_QOS_NOTIFIER_MAX; i++) {
        if (!pm_qos_notifiers[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&pm_qos_state.lock);
        return -ENOSPC;
    }

    pm_qos_notifiers[slot].fn = fn;
    pm_qos_notifiers[slot].data = data;
    pm_qos_notifiers[slot].used = 1;

    spinlock_release(&pm_qos_state.lock);
    return slot;
}

int pm_qos_remove_notifier(int id)
{
    if (!pm_qos_state.initialized)
        return -ENOSYS;
    if (id < 0 || id >= PM_QOS_NOTIFIER_MAX)
        return -ENOENT;

    spinlock_acquire(&pm_qos_state.lock);

    if (!pm_qos_notifiers[id].used) {
        spinlock_release(&pm_qos_state.lock);
        return -ENOENT;
    }

    memset(&pm_qos_notifiers[id], 0, sizeof(pm_qos_notifiers[id]));

    spinlock_release(&pm_qos_state.lock);
    return 0;
}

uint32_t pm_qos_read_value(void)
{
    return pm_qos_read_effective_latency();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Per-Device PM QoS API
 * ═══════════════════════════════════════════════════════════════════════ */

int pm_qos_device_add_request(const char *dev_name, int type, uint32_t value)
{
    if (!pm_qos_state.initialized)
        return -ENOSYS;
    if (!dev_name || !dev_name[0])
        return -EINVAL;
    if (type != PM_QOS_DEV_RESUME_LATENCY && type != PM_QOS_DEV_THROUGHPUT)
        return -EINVAL;

    spinlock_acquire(&pm_qos_state.lock);

    int slot = -1;
    for (int i = 0; i < PM_QOS_MAX_DEVICE_REQUESTS; i++) {
        if (!pm_qos_state.device_requests[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&pm_qos_state.lock);
        return -ENOSPC;
    }

    struct pm_qos_device_request *req = &pm_qos_state.device_requests[slot];
    strncpy(req->dev_name, dev_name, PM_QOS_NAME_MAX - 1);
    req->dev_name[PM_QOS_NAME_MAX - 1] = '\0';
    req->type = type;
    req->value = value;
    req->used = 1;

    spinlock_release(&pm_qos_state.lock);

    kprintf("[PM QoS] Device request #%d: '%s' type=%d value=%u\n",
            slot, dev_name, type, (unsigned)value);
    return slot;
}

int pm_qos_device_update_request(int id, uint32_t value)
{
    if (!pm_qos_state.initialized)
        return -ENOSYS;
    if (id < 0 || id >= PM_QOS_MAX_DEVICE_REQUESTS)
        return -ENOENT;

    spinlock_acquire(&pm_qos_state.lock);

    if (!pm_qos_state.device_requests[id].used) {
        spinlock_release(&pm_qos_state.lock);
        return -ENOENT;
    }

    pm_qos_state.device_requests[id].value = value;

    spinlock_release(&pm_qos_state.lock);
    return 0;
}

int pm_qos_device_remove_request(int id)
{
    if (!pm_qos_state.initialized)
        return -ENOSYS;
    if (id < 0 || id >= PM_QOS_MAX_DEVICE_REQUESTS)
        return -ENOENT;

    spinlock_acquire(&pm_qos_state.lock);

    if (!pm_qos_state.device_requests[id].used) {
        spinlock_release(&pm_qos_state.lock);
        return -ENOENT;
    }

    memset(&pm_qos_state.device_requests[id], 0,
           sizeof(pm_qos_state.device_requests[id]));

    spinlock_release(&pm_qos_state.lock);
    return 0;
}

uint32_t pm_qos_device_read_effective(const char *dev_name, int type)
{
    if (!pm_qos_state.initialized || !dev_name)
        return PM_QOS_NO_CONSTRAINT;

    spinlock_acquire(&pm_qos_state.lock);

    uint32_t effective = PM_QOS_NO_CONSTRAINT;

    for (int i = 0; i < PM_QOS_MAX_DEVICE_REQUESTS; i++) {
        if (pm_qos_state.device_requests[i].used &&
            strcmp(pm_qos_state.device_requests[i].dev_name, dev_name) == 0 &&
            pm_qos_state.device_requests[i].type == type) {
            if (type == PM_QOS_DEV_RESUME_LATENCY) {
                /* Resume latency: minimum value (tightest constraint) */
                if (pm_qos_state.device_requests[i].value < effective)
                    effective = pm_qos_state.device_requests[i].value;
            } else {
                /* Throughput: maximum value (highest requirement) */
                if (pm_qos_state.device_requests[i].value > effective &&
                    effective == PM_QOS_NO_CONSTRAINT)
                    effective = pm_qos_state.device_requests[i].value;
                else if (pm_qos_state.device_requests[i].value > effective &&
                         effective != PM_QOS_NO_CONSTRAINT)
                    effective = pm_qos_state.device_requests[i].value;
            }
        }
    }

    spinlock_release(&pm_qos_state.lock);
    return effective;
}

int pm_qos_num_device_requests(void)
{
    if (!pm_qos_state.initialized)
        return 0;

    spinlock_acquire(&pm_qos_state.lock);
    int count = 0;
    for (int i = 0; i < PM_QOS_MAX_DEVICE_REQUESTS; i++) {
        if (pm_qos_state.device_requests[i].used)
            count++;
    }
    spinlock_release(&pm_qos_state.lock);
    return count;
}
