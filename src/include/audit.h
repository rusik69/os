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
#define AUDIT_EVENT_USER_CMD (AUDIT_NLMSG_BASE + 5) /* User command (exec) */

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

/* Persist an audit event to the on-disk audit log (/audit.log).
 * Best-effort append; returns the vfs_append result. */
int audit_log_to_disk(const char *payload, int payload_len);

/* ── Structured audit events (S105) ─────────────────────────────── */

/* Log a PATH record with file metadata */
void audit_log_path(const char *pathname, uint32_t inode, uint32_t mode);

/* Log an AVC denial record (access denied) */
void audit_log_denial(const char *subj, const char *obj,
                       const char *requested);

/* ── Audit record builder (AUX records) ─────────────────────────── */

/* Begin a new audit record of the given event type.  Any in-progress
 * record is finalized first.  Returns 0 on success, -EINVAL on a bad
 * event type. */
int audit_log_start(int type);

/* Append formatted fields to the in-progress record.  No-op if no record
 * is active. */
void audit_log_format(const char *fmt, ...);

/* Append a trailing AUX record (e.g. a PATH or EXECVE follow-up) bound
 * to the same audit sequence as the in-progress record.  Returns 0 on
 * success, -ENOSPC if the record buffer is full. */
int audit_log_add_aux(const char *aux_type, const char *fmt, ...);

/* Finalize the in-progress record (emit it to ring buffer + netlink) and
 * reset the builder. */
void audit_log_end(void);

/* Single-call generic audit log (starts + formats + ends). */
void audit_log(const char *msg);

/* Log a USER_CMD record: a command (exec) executed by a user. */
void audit_log_user_command(const char *cmdline);

/* ── Audit rules ────────────────────────────────────────────────── */

/* Maximum number of installed audit rules. */
#define AUDIT_RULE_MAX 32

/* Audit rule match fields (bitmask; OR of AUM_*). */
#define AUDIT_MATCH_SYSCALL (1u << 0) /* match on syscall number */
#define AUDIT_MATCH_PID (1u << 1)     /* match on process pid */
#define AUDIT_MATCH_PATH (1u << 2)    /* match on a path prefix */
#define AUDIT_MATCH_UID (1u << 3)     /* match on the calling user id */

/* Sentinel marking "no uid criterion" (a uid filter on root would be 0). */
#define AUDIT_UID_ANY 0xFFFFFFFFu

/* A single audit rule: a set of match criteria and a log action. */
struct audit_rule {
    unsigned int match; /* OR of AUDIT_MATCH_* enabled criteria */
    long syscall;       /* syscall number (-1 = any) */
    uint32_t pid;       /* pid (0 = any) */
    uint32_t uid;       /* uid (AUDIT_UID_ANY = any) */
    char path[128];     /* path prefix ("" = any) */
};

/* Install a rule (copy).  Returns the rule index or -1 on full table. */
int audit_rule_add(const struct audit_rule *r);

/* Remove the rule at index i.  Returns 0 on success, -ENOENT if unused. */
int audit_rule_remove(int idx);

/* Remove all rules. */
void audit_rule_clear(void);

/* True if any rule currently matches (syscall number, caller pid and
 * path all match where enabled). */
int audit_rule_matches(long syscall, uint32_t pid, uint32_t uid, const char *path);

/* True if any installed rule filters on syscall number. */
int audit_rules_filter_syscall(void);

/* Return the rule at index i (0..audit_rule_count-1) into *out.
 * Returns 1 if a rule exists, 0 if out of range. */
int audit_rule_get(int idx, struct audit_rule *out);

/* Current number of installed rules. */
int audit_rule_count(void);

/* True if any filesystem-watch (path prefix) rule is installed. */
int audit_path_watched(void);

/* Register the audit-rule sysctl interface. */
void audit_sysctl_register(void);

#endif /* AUDIT_H */
