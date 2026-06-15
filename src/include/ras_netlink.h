#ifndef RAS_NETLINK_H
#define RAS_NETLINK_H

#include "types.h"

/*
 * ── RAS (Reliability, Availability, Serviceability) Netlink ──────
 *
 * Delivers hardware error events from kernel error handlers to
 * userspace via AF_NETLINK sockets using NETLINK_RAS protocol.
 *
 * Protocol family: NETLINK_RAS = 18
 *
 * Event format (struct ras_event):
 *   - timestamp (ticks since boot)
 *   - type      (e.g. RAS_UE, RAS_CE, RAS_FATAL)
 *   - severity  (0=INFO, 1=WARN, 2=CRIT, 3=FATAL)
 *   - cpu       (CPU that detected the error)
 *   - details   (64-byte human-readable description)
 *
 * A ring buffer stores the last 64 events for polling via
 * /sys/kernel/debug/ras/events.
 */

/* ── Protocol family ──────────────────────────────────────────────── */
#define NETLINK_RAS  18

/* ── Event types ──────────────────────────────────────────────────── */
#define RAS_UE       1   /* Uncorrectable Error (recoverable) */
#define RAS_CE       2   /* Correctable Error */
#define RAS_FATAL    3   /* Fatal / system-ending error */
#define RAS_INFO     4   /* Informational (e.g. PCIe hotplug) */

/* ── Severity levels ──────────────────────────────────────────────── */
#define RAS_SEV_INFO  0
#define RAS_SEV_WARN  1
#define RAS_SEV_CRIT  2
#define RAS_SEV_FATAL 3

/* ── Event structure (sent over netlink) ──────────────────────────── */
#define RAS_DETAILS_LEN 64

struct ras_event {
    uint64_t timestamp;              /* ticks since boot */
    uint32_t type;                   /* RAS_UE, RAS_CE, etc. */
    uint32_t severity;               /* RAS_SEV_* */
    uint32_t cpu;                    /* CPU that detected the error */
    char     details[RAS_DETAILS_LEN]; /* human-readable description */
} __attribute__((packed));

/* ── Ring buffer ──────────────────────────────────────────────────── */
#define RAS_RING_SIZE  64

/* ── Netlink message types (nlmsg_type) ───────────────────────────── */
#define RAS_NL_UEVENT  1   /* Hardware error event */
#define RAS_NL_MCG    1    /* Multicast group ID for RAS events */

/* ── API ──────────────────────────────────────────────────────────── */

/* Initialise the RAS netlink subsystem. */
void ras_netlink_init(void);

/* Report a hardware error event.
 * The event is recorded in the ring buffer and delivered to all
 * userspace listeners via netlink multicast (NETLINK_RAS, group 1).
 *
 * @type:     RAS_UE, RAS_CE, RAS_FATAL, or RAS_INFO
 * @severity: RAS_SEV_* severity level
 * @cpu:      CPU number that detected the error
 * @details:  NUL-terminated string (up to 63 chars + NUL)
 */
void ras_netlink_report_event(uint32_t type, uint32_t severity,
                               uint32_t cpu, const char *details);

#endif /* RAS_NETLINK_H */
