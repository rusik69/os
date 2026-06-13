#ifndef PM_RUNTIME_H
#define PM_RUNTIME_H

#include "types.h"

/* ── Runtime PM device operations ──────────────────────────────────── */

/**
 * struct dev_pm_ops — Runtime PM callbacks for a device.
 *
 * All callbacks are optional (may be NULL).
 */
struct dev_pm_ops {
    int (*runtime_suspend)(void *dev);   /* Suspend device */
    int (*runtime_resume)(void *dev);    /* Resume device */
    int (*runtime_idle)(void *dev);      /* Device idle callback */
};

/* ── Runtime PM device state ───────────────────────────────────────── */

struct pm_runtime_device {
    const char      *name;              /* Device name */
    void            *dev;               /* Opaque device pointer */
    struct dev_pm_ops *ops;             /* Device PM callbacks */
    int              usage_count;       /* Runtime usage count */
    int              suspend_after_ms;  /* Autosuspend delay in ms */
    int              suspended;         /* 1 if currently suspended */
    int              in_use;            /* 1 if this slot is active */
};

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * pm_runtime_init — Initialise the runtime PM subsystem.
 *
 * Must be called once at boot.
 */
void pm_runtime_init(void);

/**
 * pm_runtime_get_sync — Increment usage count and resume device.
 * @dev: Pointer to pm_runtime_device.
 *
 * Returns 0 on success, -1 on error.
 */
int pm_runtime_get_sync(struct pm_runtime_device *dev);

/**
 * pm_runtime_put_sync — Decrement usage count and suspend if idle.
 * @dev: Pointer to pm_runtime_device.
 *
 * Returns 0 on success, -1 on error.
 */
int pm_runtime_put_sync(struct pm_runtime_device *dev);

/**
 * pm_runtime_register — Register a device with runtime PM.
 * @dev: Pointer to pm_runtime_device to register (must be populated).
 *
 * Returns 0 on success, -1 if the device table is full.
 */
int pm_runtime_register(struct pm_runtime_device *dev);

/**
 * pm_runtime_unregister — Unregister a device from runtime PM.
 * @dev: Pointer to pm_runtime_device to unregister.
 *
 * Returns 0 on success, -1 if not found.
 */
int pm_runtime_unregister(struct pm_runtime_device *dev);

/**
 * pm_runtime_set_autosuspend_delay — Set the autosuspend delay.
 * @dev:  Pointer to pm_runtime_device.
 * @ms:   Delay in milliseconds.
 */
void pm_runtime_set_autosuspend_delay(struct pm_runtime_device *dev, int ms);

/**
 * pm_runtime_suspend_immediately — Force suspend a device immediately.
 * @dev: Pointer to pm_runtime_device.
 *
 * Returns 0 on success, -1 if the suspend callback fails.
 */
int pm_runtime_suspend_immediately(struct pm_runtime_device *dev);

/**
 * pm_runtime_resume_immediately — Force resume a device immediately.
 * @dev: Pointer to pm_runtime_device.
 *
 * Returns 0 on success, -1 if the resume callback fails.
 */
int pm_runtime_resume_immediately(struct pm_runtime_device *dev);

#endif /* PM_RUNTIME_H */
