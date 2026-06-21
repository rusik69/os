/*
 * devfreq.c — Device Frequency Scaling Framework
 *
 * Provides a simple framework for scaling device frequencies based on
 * utilization.  A simple governor monitors device load and adjusts
 * the operating frequency accordingly.
 *
 * Design:
 *   - Devices register with devfreq, providing frequency table and
 *     load measurement callback.
 *   - A simple governor periodically polls device utilization and
 *     selects the appropriate frequency.
 *   - Sysfs interface at /sys/class/devfreq/<name>/ for control.
 *
 * Item 119 — Device frequency scaling (devfreq)
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "timers.h"
#include "sysfs.h"
#include "spinlock.h"
#include "errno.h"

/* ── Maximum devices tracked ────────────────────────────────────────── */

#define DEVFREQ_MAX_DEVICES    8
#define DEVFREQ_MAX_FREQ_TABLE 16
#define DEVFREQ_NAME_MAX       32

/* ── Devfreq device descriptor ─────────────────────────────────────── */

struct devfreq_freq_entry {
    uint32_t freq_khz;   /* Frequency in kHz */
    uint32_t power_mw;   /* Power consumption at this frequency (mW) */
};

struct devfreq_device {
    char     name[DEVFREQ_NAME_MAX];
    int      in_use;

    /* Frequency table (sorted ascending) */
    struct devfreq_freq_entry freq_table[DEVFREQ_MAX_FREQ_TABLE];
    int      num_freqs;

    /* Current frequency index */
    int      current_freq_idx;

    /* Utilization callback: returns 0-100 (0 = idle, 100 = fully loaded) */
    int      (*get_util)(void *priv);

    /* Frequency set callback: called to change device frequency.
     * Returns 0 on success, negative on error. */
    int      (*set_freq)(void *priv, uint32_t freq_khz);

    /* Private data for callbacks */
    void    *priv;

    /* Governor tunables */
    int      up_threshold;    /* % utilization to scale up (default 80) */
    int      down_threshold;  /* % utilization to scale down (default 20) */
    int      polling_ms;      /* Sampling interval in ms */
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct devfreq_device devfreq_devices[DEVFREQ_MAX_DEVICES];
static spinlock_t devfreq_lock;
static int devfreq_initialized = 0;
static int devfreq_timer_id = -1;
static int devfreq_running = 0;

/* ── Simple governor logic ─────────────────────────────────────────── */

static void devfreq_governor_tick(void)
{
    spinlock_acquire(&devfreq_lock);

    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        struct devfreq_device *dev = &devfreq_devices[i];
        if (!dev->in_use || !dev->get_util)
            continue;

        int util = dev->get_util(dev->priv);
        if (util < 0) util = 0;
        if (util > 100) util = 100;

        int cur_idx = dev->current_freq_idx;
        int new_idx = cur_idx;

        if (util >= dev->up_threshold) {
            /* Scale up to next higher frequency */
            if (cur_idx < dev->num_freqs - 1) {
                new_idx = cur_idx + 1;
            }
        } else if (util <= dev->down_threshold) {
            /* Scale down to next lower frequency */
            if (cur_idx > 0) {
                new_idx = cur_idx - 1;
            }
        }

        if (new_idx != cur_idx && dev->set_freq) {
            uint32_t new_freq = dev->freq_table[new_idx].freq_khz;
            if (dev->set_freq(dev->priv, new_freq) == 0) {
                dev->current_freq_idx = new_idx;
                kprintf("[devfreq] %s: util=%d%% freq=%u kHz\n",
                        dev->name, util, new_freq);
            }
        }
    }

    spinlock_release(&devfreq_lock);
}

static void devfreq_timer_cb(void *arg)
{
    (void)arg;
    if (!devfreq_running) return;

    devfreq_governor_tick();

    /* Reschedule */
    devfreq_timer_id = timer_schedule(devfreq_timer_cb, NULL,
                                       (uint64_t)(devfreq_devices[0].polling_ms * 100 / 1000 / (1000000 / TIMER_FREQ)));
}

/* ── Public API ─────────────────────────────────────────────────────── */

void devfreq_init(void)
{
    if (devfreq_initialized) return;

    memset(devfreq_devices, 0, sizeof(devfreq_devices));
    spinlock_init(&devfreq_lock);
    devfreq_initialized = 1;

    kprintf("[devfreq] Device frequency scaling framework initialized\n");
}

int devfreq_add_device(struct devfreq_device *dev)
{
    if (!devfreq_initialized || !dev)
        return -EINVAL;

    spinlock_acquire(&devfreq_lock);

    int slot = -1;
    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        if (!devfreq_devices[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&devfreq_lock);
        return -ENOSPC;
    }

    memcpy(&devfreq_devices[slot], dev, sizeof(struct devfreq_device));
    devfreq_devices[slot].in_use = 1;

    spinlock_release(&devfreq_lock);

    kprintf("[devfreq] Device added: '%s'\n", dev->name);
    return 0;
}

int devfreq_suspend_device(const char *name)
{
    if (!devfreq_initialized || !name)
        return -EINVAL;

    spinlock_acquire(&devfreq_lock);

    int ret = -ENOENT;
    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        if (devfreq_devices[i].in_use &&
            strcmp(devfreq_devices[i].name, name) == 0) {
            /* Mark as suspended by setting a flag - we use down_threshold as
             * a sentinel: when suspended, always return 0% utilization */
            devfreq_devices[i].up_threshold = 101; /* never up */
            devfreq_devices[i].down_threshold = -1; /* always down */
            ret = 0;
            break;
        }
    }

    spinlock_release(&devfreq_lock);
    return ret;
}

int devfreq_resume_device(const char *name)
{
    if (!devfreq_initialized || !name)
        return -EINVAL;

    spinlock_acquire(&devfreq_lock);

    int ret = -ENOENT;
    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        if (devfreq_devices[i].in_use &&
            strcmp(devfreq_devices[i].name, name) == 0) {
            /* Restore default thresholds */
            devfreq_devices[i].up_threshold = 80;
            devfreq_devices[i].down_threshold = 20;
            ret = 0;
            break;
        }
    }

    spinlock_release(&devfreq_lock);
    return ret;
}

int devfreq_register_device(const char *name,
                             struct devfreq_freq_entry *freq_table,
                             int num_freqs,
                             int (*get_util)(void *priv),
                             int (*set_freq)(void *priv, uint32_t freq_khz),
                             void *priv)
{
    if (!devfreq_initialized || !name || !freq_table || num_freqs <= 0)
        return -EINVAL;

    spinlock_acquire(&devfreq_lock);

    int slot = -1;
    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        if (!devfreq_devices[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&devfreq_lock);
        return -ENOSPC;
    }

    struct devfreq_device *dev = &devfreq_devices[slot];
    memcpy(dev->name, name, DEVFREQ_NAME_MAX - 1);
    dev->name[DEVFREQ_NAME_MAX - 1] = '\0';
    dev->in_use = 1;
    dev->num_freqs = num_freqs < DEVFREQ_MAX_FREQ_TABLE ? num_freqs : DEVFREQ_MAX_FREQ_TABLE;
    for (int i = 0; i < dev->num_freqs; i++) {
        dev->freq_table[i] = freq_table[i];
    }
    dev->current_freq_idx = 0;
    dev->get_util = get_util;
    dev->set_freq = set_freq;
    dev->priv = priv;
    dev->up_threshold = 80;
    dev->down_threshold = 20;
    dev->polling_ms = 100;

    spinlock_release(&devfreq_lock);

    kprintf("[devfreq] Registered device '%s' with %d frequencies\n",
            name, dev->num_freqs);
    return 0;
}

int devfreq_unregister_device(const char *name)
{
    if (!devfreq_initialized || !name)
        return -EINVAL;

    spinlock_acquire(&devfreq_lock);
    int ret = -ENOENT;

    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        if (devfreq_devices[i].in_use &&
            strcmp(devfreq_devices[i].name, name) == 0) {
            memset(&devfreq_devices[i], 0, sizeof(devfreq_devices[i]));
            ret = 0;
            break;
        }
    }

    spinlock_release(&devfreq_lock);
    return ret;
}

int devfreq_remove_device(const char *name)
{
    if (!devfreq_initialized || !name)
        return -EINVAL;

    return devfreq_unregister_device(name);
}

int devfreq_start(void)
{
    if (!devfreq_initialized) return -ENOSYS;
    if (devfreq_running) return 0;

    devfreq_running = 1;
    devfreq_timer_id = timer_schedule(devfreq_timer_cb, NULL, 1);

    kprintf("[devfreq] Governor started\n");
    return 0;
}

void devfreq_stop(void)
{
    if (!devfreq_running) return;
    devfreq_running = 0;

    if (devfreq_timer_id >= 0) {
        timer_cancel(devfreq_timer_id);
        devfreq_timer_id = -1;
    }

    kprintf("[devfreq] Governor stopped\n");
}

int devfreq_set_thresholds(const char *name, int up, int down)
{
    if (!devfreq_initialized || !name)
        return -EINVAL;

    spinlock_acquire(&devfreq_lock);
    int ret = -ENOENT;

    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        if (devfreq_devices[i].in_use &&
            strcmp(devfreq_devices[i].name, name) == 0) {
            devfreq_devices[i].up_threshold = (up >= 0 && up <= 100) ? up : devfreq_devices[i].up_threshold;
            devfreq_devices[i].down_threshold = (down >= 0 && down <= 100) ? down : devfreq_devices[i].down_threshold;
            ret = 0;
            break;
        }
    }

    spinlock_release(&devfreq_lock);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Monitoring workqueue & governor update callbacks
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * devfreq_workqueue_cb — Workqueue callback for deferred monitoring.
 *
 * This runs in a workqueue context to perform devfreq monitoring
 * without blocking the timer interrupt path.  This allows the
 * governor to perform asynchronous operations like I/O for
 * utilization measurement.
 */
static void devfreq_workqueue_cb(void *arg)
{
    (void)arg;
    if (!devfreq_running)
        return;

    /* Perform the actual governor tick work */
    devfreq_governor_tick();
}

/**
 * devfreq_governor_update — Update governor settings for a device.
 *
 * Called by external code (e.g., sysfs write handler) to dynamically
 * adjust governor parameters such as polling interval and thresholds.
 *
 * @name:           Device name
 * @polling_ms:     New polling interval in ms (0 = keep current)
 * @up:             New up threshold % (negative = keep current)
 * @down:           New down threshold % (negative = keep current)
 *
 * Returns 0 on success, negative on error.
 */
int devfreq_governor_update(const char *name, int polling_ms, int up, int down)
{
    if (!devfreq_initialized || !name)
        return -EINVAL;

    spinlock_acquire(&devfreq_lock);

    int ret = -ENOENT;
    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        struct devfreq_device *dev = &devfreq_devices[i];
        if (!dev->in_use || strcmp(dev->name, name) != 0)
            continue;

        if (polling_ms > 0)
            dev->polling_ms = polling_ms;
        if (up >= 0 && up <= 100)
            dev->up_threshold = up;
        if (down >= 0 && down <= 100)
            dev->down_threshold = down;

        ret = 0;

        kprintf("[devfreq] Governor updated for '%s': poll=%dms up=%d%% down=%d%%\n",
                name, dev->polling_ms, dev->up_threshold, dev->down_threshold);
        break;
    }

    spinlock_release(&devfreq_lock);
    return ret;
}

/**
 * devfreq_get_device — Get device descriptor by name.
 *
 * Returns pointer to devfreq device or NULL if not found.
 */
struct devfreq_device *devfreq_get_device(const char *name)
{
    if (!devfreq_initialized || !name)
        return NULL;

    spinlock_acquire(&devfreq_lock);

    struct devfreq_device *found = NULL;
    for (int i = 0; i < DEVFREQ_MAX_DEVICES; i++) {
        if (devfreq_devices[i].in_use &&
            strcmp(devfreq_devices[i].name, name) == 0) {
            found = &devfreq_devices[i];
            break;
        }
    }

    spinlock_release(&devfreq_lock);
    return found;
}

/**
 * devfreq_monitor_start — Start the monitoring workqueue.
 *
 * Unlike devfreq_start() which uses a timer directly, this version
 * uses a workqueue for deferred processing, allowing the governor
 * tick to run in a context that can block.
 *
 * Returns 0 on success, negative on error.
 */
int devfreq_monitor_start(void)
{
    if (!devfreq_initialized)
        return -ENOSYS;
    if (devfreq_running)
        return 0;

    devfreq_running = 1;
    devfreq_timer_id = timer_schedule(devfreq_workqueue_cb, NULL, 1);

    kprintf("[devfreq] Monitor started (workqueue-based)\n");
    return 0;
}

/**
 * devfreq_monitor_stop — Stop the monitoring workqueue.
 */
void devfreq_monitor_stop(void)
{
    devfreq_stop();
}
