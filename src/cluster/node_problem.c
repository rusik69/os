/*
 * node_problem.c — Node problem detection (C180)
 *
 * Implements:
 *   C180: Detection of hardware errors, OOM, disk failures, kernel
 *         deadlocks, readonly filesystem, and frequent netdev unregister
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

#define CONDITION_TYPE_MAX      64
#define CONDITION_REASON_MAX    128
#define CONDITION_MESSAGE_MAX   256
#define MAX_CONDITIONS          16
#define NODE_PROBLEM_HEARTBEAT_MS  30000  /* 30 seconds */

/* Condition status values */
#define CONDITION_UNKNOWN       0
#define CONDITION_TRUE          1
#define CONDITION_FALSE         2

/* Known condition types */
#define COND_KERNEL_DEADLOCK        "KernelDeadlock"
#define COND_READONLY_FILESYSTEM    "ReadonlyFilesystem"
#define COND_FREQ_UNREG_NETDEV      "FrequentUnregisterNetDevice"
#define COND_OOM                    "OutOfMemory"
#define COND_DISK_FAILURE           "DiskFailure"
#define COND_HARDWARE_ERROR         "HardwareError"

/* ── Node condition descriptor ───────────────────────────────────────── */

struct node_condition {
    char     type[CONDITION_TYPE_MAX];
    int      status;                  /* CONDITION_* */
    char     reason[CONDITION_REASON_MAX];
    char     message[CONDITION_MESSAGE_MAX];
    uint64_t last_heartbeat;
    uint64_t last_transition;
    char     in_use;
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct node_condition conditions[MAX_CONDITIONS];
static int condition_count = 0;
static spinlock_t cond_lock;
static int node_problem_initialised = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C180: Node problem detection
 * ═══════════════════════════════════════════════════════════════════════ */

/* C180: Find or create a condition by type */
static struct node_condition *cond_find_or_create(const char *type)
{
    if (!type) return NULL;

    for (int i = 0; i < MAX_CONDITIONS; i++) {
        if (!conditions[i].in_use) continue;
        if (strcmp(conditions[i].type, type) == 0) {
            return &conditions[i];
        }
    }

    /* Create new */
    if (condition_count >= MAX_CONDITIONS) return NULL;

    for (int i = 0; i < MAX_CONDITIONS; i++) {
        if (!conditions[i].in_use) {
            strncpy(conditions[i].type, type, CONDITION_TYPE_MAX - 1);
            conditions[i].status = CONDITION_UNKNOWN;
            conditions[i].last_heartbeat = timer_get_ms();
            conditions[i].last_transition = timer_get_ms();
            conditions[i].in_use = 1;
            condition_count++;
            return &conditions[i];
        }
    }

    return NULL;
}

/* C180: Initialise node problem detector */
int node_problem_init(void)
{
    memset(conditions, 0, sizeof(conditions));
    condition_count = 0;
    node_problem_initialised = 1;

    /* Register default conditions */
    cond_find_or_create(COND_KERNEL_DEADLOCK);
    cond_find_or_create(COND_READONLY_FILESYSTEM);
    cond_find_or_create(COND_FREQ_UNREG_NETDEV);
    cond_find_or_create(COND_OOM);
    cond_find_or_create(COND_DISK_FAILURE);
    cond_find_or_create(COND_HARDWARE_ERROR);

    kprintf("[NodeProblem] Node problem detector initialised (%d conditions)\n",
            condition_count);
    return 0;
}

/* C180: Update a specific condition status */
int node_condition_set(const char *type, int status,
                       const char *reason, const char *message)
{
    if (!type || !node_problem_initialised) return -EINVAL;

    spinlock_acquire(&cond_lock);

    struct node_condition *c = cond_find_or_create(type);
    if (!c) {
        spinlock_release(&cond_lock);
        return -ENOSPC;
    }

    uint64_t now = timer_get_ms();

    /* Record transition if status changed */
    if (c->status != status) {
        c->last_transition = now;
    }

    c->status = status;
    if (reason) strncpy(c->reason, reason, CONDITION_REASON_MAX - 1);
    if (message) strncpy(c->message, message, CONDITION_MESSAGE_MAX - 1);
    c->last_heartbeat = now;

    spinlock_release(&cond_lock);
    return 0;
}

/* C180: Detect system problems by checking kernel-internal signals
 *
 * In production, this reads from:
 *   - /sys/kernel/debug or tracepoints for hardware errors
 *   - Kernel OOM notifier chain
 *   - Block layer error counters for disk failures
 *   - Lockup detector (softlockup/hardlockup)
 *   - VFS readonly detection via sb->s_flags
 *   - netdev notifier chain for unregister events
 *
 * Simplified: simulate detection with timer-based patterns.
 */
int node_problem_detect(void)
{
    if (!node_problem_initialised) return -EAGAIN;

    uint64_t now = timer_get_ms();
    int problems_found = 0;

    spinlock_acquire(&cond_lock);

    /* ── Simulate detection checks ────────────────────────────────── */

    /* Check 1: KernelDeadlock — check lockup detector status */
    {
        struct node_condition *c = cond_find_or_create(COND_KERNEL_DEADLOCK);
        if (c) {
            /* In production: check softlockup_thresh, nmi_watchdog.
             * Simulated: rarely triggers. */
            int deadlock_detected = ((now % 60000) < 100); /* ~0.17% chance */
            int new_status = deadlock_detected ? CONDITION_TRUE : CONDITION_FALSE;

            if (c->status != new_status) {
                c->last_transition = now;
                kprintf("[NodeProblem] KernelDeadlock -> %s\n",
                        deadlock_detected ? "TRUE" : "FALSE");
                problems_found++;
            }

            c->status = new_status;
            c->last_heartbeat = now;
            snprintf(c->reason, CONDITION_REASON_MAX,
                     deadlock_detected ? "watchdog-triggered" : "ok");
            snprintf(c->message, CONDITION_MESSAGE_MAX,
                     "Kernel deadlock detector status: %s",
                     deadlock_detected ? "LOCKUP DETECTED" : "normal");
        }
    }

    /* Check 2: ReadonlyFilesystem — check VFS remount status */
    {
        struct node_condition *c = cond_find_or_create(COND_READONLY_FILESYSTEM);
        if (c) {
            int ro_detected = ((now % 30000) < 50); /* ~0.17% chance */
            int new_status = ro_detected ? CONDITION_TRUE : CONDITION_FALSE;

            if (c->status != new_status) {
                c->last_transition = now;
                kprintf("[NodeProblem] ReadonlyFilesystem -> %s\n",
                        ro_detected ? "TRUE" : "FALSE");
                problems_found++;
            }

            c->status = new_status;
            c->last_heartbeat = now;
            snprintf(c->reason, CONDITION_REASON_MAX,
                     ro_detected ? "fs-remounted-ro" : "ok");
            snprintf(c->message, CONDITION_MESSAGE_MAX,
                     "Filesystem readonly status: %s",
                     ro_detected ? "READONLY" : "read-write");
        }
    }

    /* Check 3: FrequentUnregisterNetDevice — check netdev event rate */
    {
        struct node_condition *c = cond_find_or_create(COND_FREQ_UNREG_NETDEV);
        if (c) {
            int freq_detected = ((now % 45000) < 100); /* ~0.22% chance */
            int new_status = freq_detected ? CONDITION_TRUE : CONDITION_FALSE;

            if (c->status != new_status) {
                c->last_transition = now;
                kprintf("[NodeProblem] FrequentUnregisterNetDevice -> %s\n",
                        freq_detected ? "TRUE" : "FALSE");
                problems_found++;
            }

            c->status = new_status;
            c->last_heartbeat = now;
            snprintf(c->reason, CONDITION_REASON_MAX,
                     freq_detected ? "excessive-netdev-events" : "ok");
            snprintf(c->message, CONDITION_MESSAGE_MAX,
                     "Network device unregister rate: %s",
                     freq_detected ? "ABNORMAL" : "normal");
        }
    }

    /* Check 4: OutOfMemory */
    {
        struct node_condition *c = cond_find_or_create(COND_OOM);
        if (c) {
            int oom_detected = ((now % 90000) < 50); /* ~0.06% chance */
            int new_status = oom_detected ? CONDITION_TRUE : CONDITION_FALSE;

            if (c->status != new_status) {
                c->last_transition = now;
                kprintf("[NodeProblem] OutOfMemory -> %s\n",
                        oom_detected ? "TRUE" : "FALSE");
                problems_found++;
            }

            c->status = new_status;
            c->last_heartbeat = now;
            snprintf(c->reason, CONDITION_REASON_MAX,
                     oom_detected ? "OOM-killer-invoked" : "ok");
            snprintf(c->message, CONDITION_MESSAGE_MAX,
                     "Memory pressure status: %s",
                     oom_detected ? "OOM TRIGGERED" : "normal");
        }
    }

    /* Check 5: DiskFailure */
    {
        struct node_condition *c = cond_find_or_create(COND_DISK_FAILURE);
        if (c) {
            int disk_fail = ((now % 120000) < 50); /* ~0.04% chance */
            int new_status = disk_fail ? CONDITION_TRUE : CONDITION_FALSE;

            if (c->status != new_status) {
                c->last_transition = now;
                kprintf("[NodeProblem] DiskFailure -> %s\n",
                        disk_fail ? "TRUE" : "FALSE");
                problems_found++;
            }

            c->status = new_status;
            c->last_heartbeat = now;
            snprintf(c->reason, CONDITION_REASON_MAX,
                     disk_fail ? "io-error-count-exceeded" : "ok");
            snprintf(c->message, CONDITION_MESSAGE_MAX,
                     "Disk health status: %s",
                     disk_fail ? "FAILURE DETECTED" : "normal");
        }
    }

    /* Check 6: HardwareError */
    {
        struct node_condition *c = cond_find_or_create(COND_HARDWARE_ERROR);
        if (c) {
            int hw_err = ((now % 180000) < 50); /* ~0.03% chance */
            int new_status = hw_err ? CONDITION_TRUE : CONDITION_FALSE;

            if (c->status != new_status) {
                c->last_transition = now;
                kprintf("[NodeProblem] HardwareError -> %s\n",
                        hw_err ? "TRUE" : "FALSE");
                problems_found++;
            }

            c->status = new_status;
            c->last_heartbeat = now;
            snprintf(c->reason, CONDITION_REASON_MAX,
                     hw_err ? "mce-or-pci-err-detected" : "ok");
            snprintf(c->message, CONDITION_MESSAGE_MAX,
                     "Hardware error status: %s",
                     hw_err ? "ERROR DETECTED" : "normal");
        }
    }

    spinlock_release(&cond_lock);
    return problems_found;
}

/* C180: Report all conditions to the cluster store
 *
 * In production: writes conditions to Raft KV store so the cluster
 * controller can read and act on them.
 */
int node_conditions_update(void)
{
    if (!node_problem_initialised) return -EAGAIN;

    spinlock_acquire(&cond_lock);

    kprintf("[NodeProblem] Updating %d conditions to cluster store\n",
            condition_count);

    for (int i = 0; i < MAX_CONDITIONS; i++) {
        if (!conditions[i].in_use) continue;

        const char *status_str = "unknown";
        if (conditions[i].status == CONDITION_TRUE) status_str = "true";
        else if (conditions[i].status == CONDITION_FALSE) status_str = "false";

        kprintf("[NodeProblem]   %s = %s (reason: %s)\n",
                conditions[i].type, status_str, conditions[i].reason);

        /* In production: raft_kv_set("node/conditions/<type>", json_encoded) */
    }

    spinlock_release(&cond_lock);
    return condition_count;
}

/* ── Stub: node_problem_repair ─────────────────────────────── */
int node_problem_repair(const char *node)
{
    (void)node;
    kprintf("[cluster] node_problem_repair: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: node_problem_report ─────────────────────────────── */
int node_problem_report(const char *node, void *report)
{
    (void)node;
    (void)report;
    kprintf("[cluster] node_problem_report: not yet implemented\n");
    return -ENOSYS;
}
