#ifndef SERVICE_H
#define SERVICE_H

#include "types.h"

/* Service states */
#define SERVICE_STOPPED  0
#define SERVICE_RUNNING  1
#define SERVICE_CRASHED  2   /* service process exited unexpectedly */

/* Maximum number of registered services */
#define SERVICE_MAX 8

/* Maximum length of service name */
#define SERVICE_NAME_MAX 16

/* Maximum number of dependencies per service */
#define SERVICE_DEPS_MAX 8

/* Log path template: /var/log/<name>.log */
#define SERVICE_LOG_DIR  "/var/log"

/* ── Watchdog health-monitoring constants ──────────────────────────── */

/* Default max auto-restarts before escalation */
#define SERVICE_DEFAULT_MAX_RESTARTS  3

/* Heartbeat interval in ticks: if no heartbeat within this window,
 * the service is considered unresponsive (stale).  Default 5 seconds.
 * TIMER_FREQ is 100 Hz, so 500 ticks = 5 seconds. */
#define SERVICE_HEARTBEAT_TIMEOUT_TICKS  500

/* Cooldown ticks between restart attempts (200 ticks = 2 seconds) */
#define SERVICE_RESTART_COOLDOWN_TICKS   200

/* Watchdog check interval: how often the periodic checker runs (300 ticks = 3 sec) */
#define SERVICE_WATCHDOG_INTERVAL_TICKS  300

/* Escalation: after this many restarts on a critical service, panic */
#define SERVICE_CRITICAL_ESCALATION_LIMIT  5

struct service {
    char     name[SERVICE_NAME_MAX];
    int      state;           /* SERVICE_STOPPED / SERVICE_RUNNING / SERVICE_CRASHED */
    char     log_path[40];    /* /var/log/<name>.log */
    int    (*start)(void);    /* returns 0 on success */
    void   (*stop)(void);

    /* ── Dependency metadata (Item U3) ────────────────────────── */
    char     deps[SERVICE_DEPS_MAX][SERVICE_NAME_MAX]; /* Required-Start dependency names */
    int      ndeps;                                      /* number of valid deps */

    /* ── Watchdog fields ─────────────────────────────────────────── */
    int      pid;             /* PID of the service process (0 = kernel service) */
    int      crash_count;     /* number of times this service has crashed */
    int      max_restarts;    /* max auto-restarts before giving up (default 3) */
    int      critical;        /* 1 = critical service (panic on repeated crash) */
    uint64_t last_heartbeat;  /* tick of last heartbeat (0 = never) */
    uint64_t last_restart;    /* tick of last restart attempt (for cooldown) */
};

/* Initialize the service subsystem and prepare the filesystem structure */
void service_init(void);

/* Register a service (called at boot by httpd/telnetd init) */
int  service_register(const char *name, int (*start)(void), void (*stop)(void));

/* Start / stop a named service */
int  service_start(const char *name);
int  service_stop(const char *name);

/* Query */
int         service_count(void);
struct service *service_get(int idx);
struct service *service_find(const char *name);

/* Write a line to the service's log file (appends with newline) */
void service_log(const char *name, const char *msg);

/* ── Dependency management (Item U3) ──────────────────────────────── */

/* Add a Required-Start dependency: service 'name' depends on 'dep'.
 * Returns 0 on success, -1 if name not found or dep table full. */
int service_add_dep(const char *name, const char *dep);

/* Return the number of dependencies for a given service.
 * Returns -1 if the service is not found. */
int service_num_deps(const char *name);

/* Get the i-th dependency name for a service.
 * Returns NULL if i is out of range or service not found. */
const char *service_get_dep(const char *name, int i);

/* Topological sort of service start order.
 * Fills 'order' array with service indices in dependency order.
 * Returns the number of entries in 'order', or -1 on error (cycle detected). */
int service_sort_deps(int *order, int max_order);

/* ── Watchdog health-monitoring API ─────────────────────────────────── */

/* Set the PID of a service process (extensible for process-based services) */
void service_set_pid(const char *name, int pid);

/* Mark a service as critical (repeated crashes trigger panic escalation) */
void service_set_critical(const char *name, int critical);

/* Record a heartbeat timestamp for a service.
 * If a service's heartbeat is older than SERVICE_HEARTBEAT_TIMEOUT_TICKS,
 * the watchdog considers it unresponsive and may restart it. */
void service_heartbeat(const char *name);

/* Periodic health check — called from the kernel timer or idle loop.
 * Scans registered services, checks aliveness, and auto-restarts
 * crashed/unresponsive services up to max_restarts times. */
void service_watchdog_check(void);

/* Initialise the watchdog subsystem (set up defaults, etc.) */
void service_watchdog_init(void);

#endif
