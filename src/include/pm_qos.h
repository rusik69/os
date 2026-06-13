#ifndef PM_QOS_H
#define PM_QOS_H

#include "types.h"

/*
 * PM QoS — Power Management Quality of Service
 *
 * Provides a framework for kernel components (drivers, subsystems) to
 * register latency constraints that influence cpuidle C-state selection.
 *
 * The effective constraint is the MINIMUM of all registered requests.
 * cpuidle uses this to skip C-states whose wakeup latency exceeds the
 * constraint, ensuring that no driver is forced to tolerate longer
 * wakeup latency than it can handle.
 *
 * Usage:
 *   int req = pm_qos_add_request("my_driver", 100);  // 100 us max
 *   ...
 *   pm_qos_update_request(req, 50);  // tighter constraint
 *   ...
 *   pm_qos_remove_request(req);
 */

/* ── Constants ────────────────────────────────────────────────────── */

/** Maximum number of concurrent PM QoS requests (sufficient for all drivers). */
#define PM_QOS_MAX_REQUESTS  32

/** Maximum length of a request name (including NUL terminator). */
#define PM_QOS_NAME_MAX      32

/** Value meaning "no constraint" — the deepest idle state is always allowed. */
#define PM_QOS_NO_CONSTRAINT  0xFFFFFFFFU

/* ── Per-device constraint types ──────────────────────────────────── */

/** Resume latency constraint type (microseconds) */
#define PM_QOS_DEV_RESUME_LATENCY  0
/** Throughput constraint type (MB/s) */
#define PM_QOS_DEV_THROUGHPUT      1

/* ── Public API ───────────────────────────────────────────────────── */

/**
 * Initialise the PM QoS subsystem.
 * Must be called once during kernel boot, before any drivers register
 * latency constraints.
 */
void pm_qos_init(void);

/**
 * Register a latency constraint.
 *
 * @param name        Human-readable name for debugging (e.g. "e1000", "ahci").
 * @param latency_us  Maximum allowed wakeup latency in microseconds.
 *                    0 means "must be immediately responsive" (only C1 allowed).
 * @return            Request ID (>= 0) on success, or -ENOSPC if the
 *                    request table is full, -EINVAL if latency_us is bogus.
 */
int  pm_qos_add_request(const char *name, uint32_t latency_us);

/**
 * Update an existing latency constraint.
 *
 * @param id          Request ID returned by pm_qos_add_request().
 * @param latency_us  New maximum allowed wakeup latency in microseconds.
 * @return            0 on success, -ENOENT if the request ID is not found.
 */
int  pm_qos_update_request(int id, uint32_t latency_us);

/**
 * Remove a latency constraint.
 *
 * @param id  Request ID to remove.
 * @return    0 on success, -ENOENT if not found.
 */
int  pm_qos_remove_request(int id);

/**
 * Read the current effective latency constraint.
 *
 * Returns the MINIMUM latency_us across all registered requests.
 * If no requests are registered, returns PM_QOS_NO_CONSTRAINT
 * (meaning the deepest idle state is always permissible).
 */
uint32_t pm_qos_read_effective_latency(void);

/**
 * Return the number of currently registered PM QoS requests.
 */
int pm_qos_num_requests(void);

/**
 * Debug: print all registered PM QoS requests via kprintf.
 */
void pm_qos_dump_requests(void);

/* ── Per-Device PM QoS API ────────────────────────────────────────── */

/**
 * pm_qos_device_add_request — Register a per-device PM QoS constraint.
 * @dev_name:  Device name string.
 * @type:      PM_QOS_DEV_RESUME_LATENCY or PM_QOS_DEV_THROUGHPUT.
 * @value:     Constraint value (latency in us or throughput in MB/s).
 * @return     Request ID (>= 0) on success, or -ENOSPC/-EINVAL on error.
 */
int pm_qos_device_add_request(const char *dev_name, int type, uint32_t value);

/**
 * pm_qos_device_update_request — Update an existing per-device constraint.
 * @id:    Request ID returned by pm_qos_device_add_request().
 * @value: New constraint value.
 * @return 0 on success, -ENOENT if not found.
 */
int pm_qos_device_update_request(int id, uint32_t value);

/**
 * pm_qos_device_remove_request — Remove a per-device PM QoS constraint.
 * @id: Request ID to remove.
 * @return 0 on success, -ENOENT if not found.
 */
int pm_qos_device_remove_request(int id);

/**
 * pm_qos_device_read_effective — Read the effective constraint for a device.
 * @dev_name: Device name.
 * @type:     Constraint type.
 * @return    The effective constraint value, or PM_QOS_NO_CONSTRAINT if none.
 */
uint32_t pm_qos_device_read_effective(const char *dev_name, int type);

/**
 * pm_qos_num_device_requests — Return the number of per-device PM QoS requests.
 */
int pm_qos_num_device_requests(void);

#endif /* PM_QOS_H */
