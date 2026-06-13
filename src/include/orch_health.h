/*
 * orch_health.h — Pod health checking (Item B24)
 *
 * Defines health probe types, status codes, and the probe structure
 * for implementing liveness and readiness probes on containers/pods.
 *
 * Probe types:
 *   PROBE_EXEC — fork+exec a command inside the container, check exit code 0
 *   PROBE_HTTP — HTTP GET to container-ip:port/path, check 2xx/3xx
 *
 * Status values:
 *   HEALTH_UNKNOWN    — probe not started or initial delay active
 *   HEALTH_HEALTHY    — probe passes
 *   HEALTH_UNHEALTHY  — probe exceeded failure_threshold consecutive failures
 */

#ifndef ORCH_HEALTH_H
#define ORCH_HEALTH_H

#include "types.h"

/* ── Probe type constants ──────────────────────────────────────────── */
#define PROBE_EXEC  0
#define PROBE_HTTP  1

/* ── Health status constants ───────────────────────────────────────── */
#define HEALTH_UNKNOWN    0
#define HEALTH_HEALTHY    1
#define HEALTH_UNHEALTHY  2

/* ── String size limits ────────────────────────────────────────────── */
#define PROBE_COMMAND_MAX  256
#define PROBE_URL_MAX      512
#define PROBE_CONTAINER_ID_MAX 64

/* ── Health probe descriptor ──────────────────────────────────────────
 *
 * Describes a single health probe attached to a container.
 * For EXEC probes, 'command' holds the full command line to execute.
 * For HTTP probes, 'http_url' holds the complete URL to GET
 * (e.g. "http://10.0.2.15:8080/healthz").
 *
 * Timing fields are in seconds.
 */
struct health_probe {
    int   type;                             /* PROBE_EXEC or PROBE_HTTP  */
    char  command[PROBE_COMMAND_MAX];       /* EXEC: command + args      */
    char  http_url[PROBE_URL_MAX];          /* HTTP: full URL to GET     */
    int   initial_delay_seconds;            /* wait before first check   */
    int   period_seconds;                   /* interval between checks   */
    int   timeout_seconds;                  /* max time for single check  */
    int   success_threshold;                /* consecutive successes OK   */
    int   failure_threshold;                /* consecutive failures→UNHEALTHY */
};

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * health_probe_start() — Begin running a health probe on a container.
 *
 * Registers a probe and begins periodic health checking.  The probe
 * respects initial_delay_seconds before its first check.
 *
 * @container_id:  Container ID string (from container->id).
 * @probe:         Probe descriptor (copied internally).
 *
 * Returns 0 on success, negative errno on failure.
 */
int health_probe_start(const char *container_id,
                       const struct health_probe *probe);

/**
 * health_probe_stop() — Stop and unregister all probes for a container.
 *
 * @container_id:  Container ID string.
 *
 * Returns 0 on success, negative errno if no probes were registered.
 */
int health_probe_stop(const char *container_id);

/**
 * health_get_status() — Query the current health status of a container.
 *
 * @container_id:  Container ID string.
 *
 * Returns HEALTH_HEALTHY, HEALTH_UNHEALTHY, or HEALTH_UNKNOWN
 * (including when no probe is registered).
 */
int health_get_status(const char *container_id);

#endif /* ORCH_HEALTH_H */
