/*
 * pm_runtime.c — Runtime Power Management
 *
 * Implements a simple usage-count-based runtime PM framework.
 * Devices can register with runtime PM, get/put usage counts,
 * and auto-suspend after a configurable idle delay.
 */

#include "pm_runtime.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/* ── Maximum devices ─────────────────────────────────────────────────- */

#define PM_RUNTIME_MAX_DEVICES 32

/* ── State ──────────────────────────────────────────────────────────── */

/* Table of registered runtime PM devices */
static struct pm_runtime_device g_rpm_devices[PM_RUNTIME_MAX_DEVICES];
static int g_rpm_initialized = 0;

/* ── Internal helpers ──────────────────────────────────────────────── */

static struct pm_runtime_device *rpm_find_device(struct pm_runtime_device *dev)
{
    for (int i = 0; i < PM_RUNTIME_MAX_DEVICES; i++) {
        if (g_rpm_devices[i].in_use && &g_rpm_devices[i] == dev)
            return &g_rpm_devices[i];
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * pm_runtime_init - Initialize the Runtime Power Management subsystem
 *
 * Initialises the global runtime PM device table and marks the subsystem
 * as ready.  Must be called once during kernel boot before any other
 * runtime PM operations.  Safe to call multiple times (idempotent).
 */
void __init pm_runtime_init(void)
{
    if (g_rpm_initialized) {
        kprintf("[pm_runtime] Already initialized\n");
        return;
    }

    memset(g_rpm_devices, 0, sizeof(g_rpm_devices));
    g_rpm_initialized = 1;
    kprintf("[pm_runtime] Runtime PM subsystem initialized\n");
}

/**
 * pm_runtime_get_sync - Increment the usage count and resume a device
 * @dev: Target runtime PM device
 *
 * Increases the usage count of @dev.  If the device is currently
 * suspended, its runtime_resume callback is invoked to bring it back
 * to an active state.  Must be called before accessing device hardware.
 *
 * Return: 0 on success, -1 if the subsystem is not initialised, @dev
 *         is NULL, or resume fails
 */
int pm_runtime_get_sync(struct pm_runtime_device *dev)
{
    if (!g_rpm_initialized || !dev)
        return -1;

    struct pm_runtime_device *d = rpm_find_device(dev);
    if (!d)
        return -1;

    d->usage_count++;

    /* If the device is suspended, resume it */
    if (d->suspended && d->ops && d->ops->runtime_resume) {
        int ret = d->ops->runtime_resume(d->dev);
        if (ret == 0) {
            d->suspended = 0;
            kprintf("[pm_runtime] Resumed device '%s' (usage=%d)\n",
                    d->name, d->usage_count);
        } else {
            kprintf("[pm_runtime] Failed to resume device '%s': %d\n",
                    d->name, ret);
            d->usage_count--;
            return -1;
        }
    }

    return 0;
}

/**
 * pm_runtime_put_sync - Decrement the usage count and autosuspend a device
 * @dev: Target runtime PM device
 *
 * Decreases the usage count of @dev.  When the count reaches zero,
 * the runtime_idle callback is called first; if the device is still
 * active, runtime_suspend is invoked to save power.  Must be called
 * when the caller is done accessing device hardware.
 *
 * Return: 0 on success, -1 if the subsystem is not initialised, @dev
 *         is NULL, or usage count is already zero
 */
int pm_runtime_put_sync(struct pm_runtime_device *dev)
{
    if (!g_rpm_initialized || !dev)
        return -1;

    struct pm_runtime_device *d = rpm_find_device(dev);
    if (!d)
        return -1;

    if (d->usage_count <= 0) {
        kprintf("[pm_runtime] Usage count already 0 for '%s'\n", d->name);
        return -1;
    }

    d->usage_count--;

    /* If usage count drops to 0, attempt autosuspend */
    if (d->usage_count == 0 && d->ops) {
        if (d->ops->runtime_idle)
            d->ops->runtime_idle(d->dev);

        if (!d->suspended && d->ops->runtime_suspend) {
            int ret = d->ops->runtime_suspend(d->dev);
            if (ret == 0) {
                d->suspended = 1;
                kprintf("[pm_runtime] Suspended device '%s' (autosuspend)\n",
                        d->name);
            } else {
                kprintf("[pm_runtime] Failed to suspend device '%s': %d\n",
                        d->name, ret);
            }
        }
    }

    return 0;
}

/**
 * pm_runtime_register - Register a device with the runtime PM subsystem
 * @dev: Device descriptor to register (copied into internal table)
 *
 * Adds @dev to the runtime PM device table and starts it with a usage
 * count of 1.  The device must have valid ops pointers for runtime
 * suspend/resume.  Up to PM_RUNTIME_MAX_DEVICES devices can be registered.
 *
 * Return: 0 on success, -1 if the table is full or @dev is NULL
 */
int pm_runtime_register(struct pm_runtime_device *dev)
{
    if (!g_rpm_initialized || !dev)
        return -1;

    /* Find free slot */
    for (int i = 0; i < PM_RUNTIME_MAX_DEVICES; i++) {
        if (!g_rpm_devices[i].in_use) {
            g_rpm_devices[i] = *dev;
            g_rpm_devices[i].usage_count = 1;  /* Start with a reference */
            g_rpm_devices[i].suspended = 0;
            g_rpm_devices[i].in_use = 1;
            kprintf("[pm_runtime] Registered device '%s'\n", dev->name);
            return 0;
        }
    }

    kprintf("[pm_runtime] Device table full, cannot register '%s'\n",
            dev->name ? dev->name : "?");
    return -1;
}

/**
 * pm_runtime_unregister - Remove a device from runtime PM
 * @dev: Device descriptor to unregister
 *
 * Resumes the device if it is suspended, then removes it from the
 * internal device table.  After this call @dev is no longer tracked
 * by runtime PM.
 *
 * Return: 0 on success, -1 if @dev is not registered or is NULL
 */
int pm_runtime_unregister(struct pm_runtime_device *dev)
{
    if (!g_rpm_initialized || !dev)
        return -1;

    for (int i = 0; i < PM_RUNTIME_MAX_DEVICES; i++) {
        if (g_rpm_devices[i].in_use && &g_rpm_devices[i] == dev) {
            /* Resume the device before unregistering */
            if (g_rpm_devices[i].suspended && g_rpm_devices[i].ops &&
                g_rpm_devices[i].ops->runtime_resume) {
                g_rpm_devices[i].ops->runtime_resume(g_rpm_devices[i].dev);
            }
            memset(&g_rpm_devices[i], 0, sizeof(g_rpm_devices[i]));
            kprintf("[pm_runtime] Unregistered device '%s'\n", dev->name);
            return 0;
        }
    }

    return -1;
}

void pm_runtime_set_autosuspend_delay(struct pm_runtime_device *dev, int ms)
{
    if (dev)
        dev->suspend_after_ms = ms;
}

/**
 * pm_runtime_suspend_immediately - Force-suspend a device right away
 * @dev: Target runtime PM device
 *
 * Calls the device's runtime_suspend callback immediately, bypassing
 * the autosuspend delay.  The device is marked as suspended on success.
 *
 * Return: 0 on success, -1 if @dev or its ops are NULL or suspend fails
 */
int pm_runtime_suspend_immediately(struct pm_runtime_device *dev)
{
    if (!dev || !dev->ops || !dev->ops->runtime_suspend)
        return -1;

    int ret = dev->ops->runtime_suspend(dev->dev);
    if (ret == 0) {
        dev->suspended = 1;
        kprintf("[pm_runtime] Immediately suspended device '%s'\n", dev->name);
    }
    return ret;
}

int pm_runtime_resume_immediately(struct pm_runtime_device *dev)
{
    if (!dev || !dev->ops || !dev->ops->runtime_resume)
        return -1;

    int ret = dev->ops->runtime_resume(dev->dev);
    if (ret == 0) {
        dev->suspended = 0;
        kprintf("[pm_runtime] Immediately resumed device '%s'\n", dev->name);
    }
    return ret;
}

/* ── Stub: pm_runtime_get ─────────────────────────────── */
static int pm_runtime_get(void *dev)
{
    (void)dev;
    kprintf("[pm] pm_runtime_get: not yet implemented\n");
    return 0;
}
/* ── Stub: pm_runtime_put ─────────────────────────────── */
static int pm_runtime_put(void *dev)
{
    (void)dev;
    kprintf("[pm] pm_runtime_put: not yet implemented\n");
    return 0;
}
/* ── Stub: pm_runtime_resume ─────────────────────────────── */
static int pm_runtime_resume(void *dev)
{
    (void)dev;
    kprintf("[pm] pm_runtime_resume: not yet implemented\n");
    return 0;
}
/* ── Stub: pm_runtime_suspend ─────────────────────────────── */
static int pm_runtime_suspend(void *dev)
{
    (void)dev;
    kprintf("[pm] pm_runtime_suspend: not yet implemented\n");
    return 0;
}
