#ifndef AUDIT_H
#define AUDIT_H

#include "types.h"

/*
 * Audit subsystem — security event logging.
 *
 * Collects security-relevant events (syscalls, policy denials, etc.)
 * and delivers them via:
 *   1) NETLINK_AUDIT multicast (primary, real-time)
 *   2) In-kernel ring buffer (secondary, for /proc/audit_log)
 *
 * Userspace opens an AF_NETLINK socket with protocol NETLINK_AUDIT (9)
 * and binds to the AUDIT_NL_GROUP multicast group to receive events.
 */

/* Audit ring buffer size (8KB) */
#define AUDIT_BUF_SIZE 8192

/* Audit enable flag */
extern int audit_enabled;

/* ── Netlink multicast group for audit events ─────────────────────── */
#define AUDIT_NL_GROUP  1   /* Bit 0 in nl_groups mask */

/* Audit event types (netlink nlmsg_type values) */
#define AUDIT_NLMSG_BASE    0x100   /* Start of audit-specific message types */
#define AUDIT_EVENT_SYSCALL (AUDIT_NLMSG_BASE + 1)  /* Syscall entry/exit */
#define AUDIT_EVENT_LOG     (AUDIT_NLMSG_BASE + 2)  /* Generic log message */
#define AUDIT_EVENT_DENIAL  (AUDIT_NLMSG_BASE + 3)  /* Security denial */
#define AUDIT_EVENT_USER    (AUDIT_NLMSG_BASE + 4)  /* Userspace-generated event */

/* ── Audit message header (after netlink nlmsghdr) ───────────────── */
struct audit_msg_hdr {
    uint32_t sequence;         /* Event sequence number (monotonic) */
    uint32_t pid;              /* PID of the process that triggered the event */
    uint64_t timestamp_ticks;  /* Kernel timer ticks at event time */
    uint8_t  event_type;       /* Audit event type */
    uint8_t  reserved[7];      /* Padding to 8-byte alignment */
} __attribute__((packed));

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the audit subsystem (sets up netlink family) */
void audit_init(void);

/* Log a generic audit event message */
void audit_log_event(const char *msg);

/* Log syscall entry (called from syscall dispatch) */
void audit_syscall_entry(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);

/* Log syscall exit (called from syscall dispatch) */
void audit_syscall_exit(uint64_t ret);

/* Read from the in-kernel ring buffer (legacy /proc interface) */
int  audit_read_log(char *buf, int max);

/* Force-send an audit message via netlink multicast.
 * Returns 0 on success, -1 on error. */
int  audit_netlink_send(int event_type, const char *payload, int payload_len);

#endif /* AUDIT_H */
