/* audit.c — Audit subsystem with NETLINK_AUDIT multicast transport (Item 185)
 *
 * Collects security-relevant events and delivers them:
 *   1) Via NETLINK_AUDIT multicast netlink messages (primary, real-time)
 *   2) Via in-kernel ring buffer (secondary, for local readers)
 *
 * Enhanced with structured audit event formatting (S105):
 *   - Syscall records: SYSCALL fields (arch, syscall, args, pid, uid, etc.)
 *   - Path records:   PATH fields (pathname, inode, mode)
 *   - AVC records:    SELinux access vector decisions
 *   - Cred records:   Subject/object security contexts
 *
 * Userspace: open AF_NETLINK socket with protocol NETLINK_AUDIT (9),
 * bind with nl_groups |= AUDIT_NL_GROUP, then read() to receive events.
 */

#define KERNEL_INTERNAL
#include "audit.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "process.h"
#include "netlink.h"
#include "heap.h"

int audit_enabled = 0;

/* ── Ring buffer (secondary/fallback) ────────────────────────────── */
static char audit_buf[AUDIT_BUF_SIZE];
static int  audit_pos = 0;

/* ── Netlink state ───────────────────────────────────────────────── */
static int  audit_nl_initialized = 0;  /* Netlink family registered? */
static unsigned int  audit_sequence = 0;        /* Monotonic event sequence */

/* ── Audit event formatting helpers (S105) ──────────────────────── */

/*
 * Build a structured audit record in a buffer.  Format is similar to
 * Linux audit record format: "type=<ET> msg=audit(<seq>.<time>: ... )\n"
 *
 * Returns the number of bytes written (not including NUL).
 */
static int audit_format_syscall(char *buf, int buf_size,
                                 uint64_t syscall_num,
                                 uint64_t arg1, uint64_t arg2,
                                 uint64_t arg3, uint64_t arg4,
                                 uint64_t ret_val)
{
    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;
    uint32_t uid = p ? p->uid : 0;
    uint32_t gid = p ? p->gid : 0;
    uint64_t now = timer_get_ticks();

    return snprintf(buf, (size_t)buf_size,
        "type=SYSCALL msg=audit(%llu.%03u:%u): "
        "arch=c000003f syscall=%llu success=%s exit=%llu "
        "pid=%u uid=%u gid=%u "
        "a1=%llx a2=%llx a3=%llx a4=%llx",
        (unsigned long long)(now / 1000),
        (unsigned int)(now % 1000),
        audit_sequence + 1,
        (unsigned long long)syscall_num,
        (long long)ret_val >= 0 ? "yes" : "no",
        (unsigned long long)ret_val,
        pid, uid, gid,
        (unsigned long long)arg1,
        (unsigned long long)arg2,
        (unsigned long long)arg3,
        (unsigned long long)arg4);
}

static int audit_format_path(char *buf, int buf_size,
                               const char *pathname, uint32_t inode,
                               uint32_t mode)
{
    return snprintf(buf, (size_t)buf_size,
        "type=PATH msg=audit(%u): "
        "name=%s inode=%u mode=%o nametype=NORMAL",
        audit_sequence + 1,
        pathname ? pathname : "?",
        inode, mode);
}

static int audit_format_denial(char *buf, int buf_size,
                                 const char *subj, const char *obj,
                                 const char *requested)
{
    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    return snprintf(buf, (size_t)buf_size,
        "type=AVC msg=audit(%u): "
        "avc:  denied  { %s } for pid=%u "
        "subj=%s obj=%s",
        audit_sequence + 1,
        requested ? requested : "unknown",
        pid,
        subj ? subj : "kernel",
        obj ? obj : "unknown");
}

/* Internal helper: build and send a netlink audit message.
 * Allocates a temporary buffer, formats the netlink header + audit header
 * + payload, and broadcasts it on NETLINK_AUDIT protocol group 1.
 * The buffer is freed after sending.
 */
int audit_netlink_send(int event_type, const char *payload, int payload_len) {
    if (!audit_enabled || !audit_nl_initialized)
        return -ENOMEM;

    if (!payload) payload = "";
    if (payload_len <= 0) payload_len = strlen(payload);

    /* Message layout: nlmsghdr | audit_msg_hdr | payload */
    int audit_hdr_len = (int)sizeof(struct audit_msg_hdr);
    int total_len = NLMSG_HDRLEN + audit_hdr_len + payload_len;
    int aligned_len = NLMSG_ALIGN(total_len);

    /* Allocate a zeroed buffer for the message */
    struct nlmsghdr *nlh = (struct nlmsghdr *)kmalloc((size_t)aligned_len);
    if (!nlh) return -EINVAL;
    memset(nlh, 0, (size_t)aligned_len);

    /* Populate netlink header */
    nlh->nlmsg_len   = (uint32_t)total_len;
    nlh->nlmsg_type  = (uint16_t)event_type;
    nlh->nlmsg_flags = 0;
    nlh->nlmsg_seq   = (uint32_t)(audit_sequence + 1);
    nlh->nlmsg_pid   = 0;  /* Kernel origin */

    /* Populate audit message header */
    struct audit_msg_hdr *ah = (struct audit_msg_hdr *)NLMSG_DATA(nlh);
    ah->sequence        = (uint32_t)(audit_sequence + 1);
    ah->timestamp_ticks = timer_get_ticks();

    struct process *p = process_get_current();
    ah->pid = p ? p->pid : 0;
    ah->event_type = (uint8_t)event_type;

    /* Copy payload after audit header */
    if (payload_len > 0) {
        char *dst = (char *)ah + audit_hdr_len;
        memcpy(dst, payload, (size_t)payload_len);
    }

    /* Broadcast on NETLINK_AUDIT protocol, group 1 (AUDIT_NL_GROUP) */
    int ret = netlink_broadcast(NETLINK_AUDIT, AUDIT_NL_GROUP,
                                 nlh, aligned_len, 0);
    kfree(nlh);
    return ret;
}

/* ── Initialisation ──────────────────────────────────────────────── */

void audit_init(void) {
    if (audit_enabled) return;

    audit_enabled = 1;
    audit_pos = 0;
    audit_sequence = 0;
    memset(audit_buf, 0, sizeof(audit_buf));

    /* Register the AUDIT generic netlink family (for future use) */
    /* Note: We primarily use raw NETLINK_AUDIT protocol, but also
     * register a generic netlink family so genl-aware tools can
     * discover the audit subsystem. */
    int fam_id = genl_register_family("AUDIT", 1, 0);
    if (fam_id < 0) {
        kprintf("[WARN] audit: failed to register genl family\n");
    } else {
        kprintf("[OK] audit: genl family id=%d\n", fam_id);
    }

    audit_nl_initialized = 1;
    kprintf("[OK] Audit subsystem initialized (NETLINK_AUDIT protocol %d, group %d)\n",
            NETLINK_AUDIT, AUDIT_NL_GROUP);
}

/* ── Event logging (S105 enhanced) ───────────────────────────────── */

/*
 * audit_log_formatted — Send a formatted audit event.
 * Takes a format string and arguments, builds the record, and sends it
 * via both ring buffer and netlink multicast.
 */
static void audit_log_formatted(int event_type, const char *fmt, ...)
{
    if (!audit_enabled || !fmt) return;

    char tmp[512];
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    __builtin_va_end(args);

    if (n <= 0) return;

    /* 1) Write to ring buffer */
    int i;
    for (i = 0; i < n && audit_pos < AUDIT_BUF_SIZE - 1; i++) {
        audit_buf[audit_pos++] = tmp[i];
    }
    if (audit_pos >= AUDIT_BUF_SIZE) audit_pos = 0;

    /* 2) Send via netlink multicast */
    audit_sequence++;
    audit_netlink_send(event_type, tmp, n);
}

void audit_log_event(const char *msg) {
    if (!audit_enabled || !msg) return;

    audit_log_formatted(AUDIT_EVENT_LOG, "%s", msg);
}

void audit_syscall_entry(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!audit_enabled) return;
    (void)a5;

    /* Store the syscall number per-process for use by audit_syscall_exit */
    struct process *p = process_get_current();
    if (p)
        p->audit_syscall_num = (int)num;

    char buf[512];
    audit_format_syscall(buf, sizeof(buf), num, a1, a2, a3, a4, 0);
    audit_log_formatted(AUDIT_EVENT_SYSCALL, "%s", buf);
}

void audit_syscall_exit(uint64_t ret) {
    if (!audit_enabled) return;

    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;
    int syscall_num = p ? p->audit_syscall_num : -1;

    char buf[256];
    int n;
    if (syscall_num >= 0) {
        n = snprintf(buf, sizeof(buf),
            "type=SYSCALL_EXIT msg=audit(%u): syscall=%d exit=%lld pid=%u",
            audit_sequence + 1, syscall_num, (long long)ret, pid);
        if (p) p->audit_syscall_num = -1; /* reset */
    } else {
        n = snprintf(buf, sizeof(buf),
            "type=SYSCALL_EXIT msg=audit(%u): exit=%lld pid=%u",
            audit_sequence + 1, (long long)ret, pid);
    }
    if (n > 0) {
        audit_log_formatted(AUDIT_EVENT_SYSCALL, "%s", buf);
    }
}

/* ── Structured audit events (S105) ─────────────────────────────── */

/*
 * audit_log_path — Log a PATH record.
 */
void audit_log_path(const char *pathname, uint32_t inode, uint32_t mode)
{
    char buf[256];
    audit_format_path(buf, sizeof(buf), pathname, inode, mode);
    audit_log_formatted(AUDIT_EVENT_LOG, "%s", buf);
}

/*
 * audit_log_denial — Log an AVC denial record.
 */
void audit_log_denial(const char *subj, const char *obj,
                       const char *requested)
{
    char buf[256];
    audit_format_denial(buf, sizeof(buf), subj, obj, requested);
    audit_log_formatted(AUDIT_EVENT_DENIAL, "%s", buf);
}

int audit_read_log(char *buf, int max) {
    if (!buf || max <= 0) return -EINVAL;
    int to_copy = audit_pos < max ? audit_pos : max;
    memcpy(buf, audit_buf, to_copy);
    return to_copy;
}

/* ── Stub: audit_log ───────────────────────────────────────────────── */
void audit_log(const char *msg)
{
    (void)msg;
    kprintf("[AUDIT] audit_log: not yet implemented\n");
}

/* ── Stub: audit_log_start ─────────────────────────────────────────── */
int audit_log_start(int type)
{
    (void)type;
    kprintf("[AUDIT] audit_log_start: not yet implemented\n");
    return 0;
}

/* ── Stub: audit_log_end ───────────────────────────────────────────── */
void audit_log_end(void)
{
    kprintf("[AUDIT] audit_log_end: not yet implemented\n");
}

/* ── Stub: audit_log_format ────────────────────────────────────────── */
void audit_log_format(const char *fmt, ...)
{
    (void)fmt;
    kprintf("[AUDIT] audit_log_format: not yet implemented\n");
}

/* ── Stub: audit_send_reply ────────────────────────────────────────── */
int audit_send_reply(void *skb, int type, int done, int seq, const void *data, int len)
{
    (void)skb; (void)type; (void)done; (void)seq; (void)data; (void)len;
    kprintf("[AUDIT] audit_send_reply: not yet implemented\n");
    return 0;
}
