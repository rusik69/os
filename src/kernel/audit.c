/* audit.c — Audit subsystem with NETLINK_AUDIT multicast transport (Item 185)
 *
 * Collects security-relevant events and delivers them:
 *   1) Via NETLINK_AUDIT multicast netlink messages (primary, real-time)
 *   2) Via in-kernel ring buffer (secondary, for local readers)
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
static int  audit_sequence = 0;        /* Monotonic event sequence */

/* Internal helper: build and send a netlink audit message.
 * Allocates a temporary buffer, formats the netlink header + audit header
 * + payload, and broadcasts it on NETLINK_AUDIT protocol group 1.
 * The buffer is freed after sending.
 */
int audit_netlink_send(int event_type, const char *payload, int payload_len) {
    if (!audit_enabled || !audit_nl_initialized)
        return -1;

    if (!payload) payload = "";
    if (payload_len <= 0) payload_len = strlen(payload);

    /* Message layout: nlmsghdr | audit_msg_hdr | payload */
    int audit_hdr_len = (int)sizeof(struct audit_msg_hdr);
    int total_len = NLMSG_HDRLEN + audit_hdr_len + payload_len;
    int aligned_len = NLMSG_ALIGN(total_len);

    /* Allocate a zeroed buffer for the message */
    struct nlmsghdr *nlh = (struct nlmsghdr *)kmalloc((size_t)aligned_len);
    if (!nlh) return -1;
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

/* ── Event logging ───────────────────────────────────────────────── */

void audit_log_event(const char *msg) {
    if (!audit_enabled || !msg) return;

    uint64_t now = timer_get_ticks();
    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    /* Format: "[ticks] pid: msg\n" */
    char tmp[256];
    int n = snprintf(tmp, sizeof(tmp), "[%llu] %u: %s\n",
                     (unsigned long long)now, pid, msg);
    if (n <= 0) return;

    /* 1) Write to ring buffer */
    int i;
    for (i = 0; i < n && audit_pos < AUDIT_BUF_SIZE - 1; i++) {
        audit_buf[audit_pos++] = tmp[i];
    }
    if (audit_pos >= AUDIT_BUF_SIZE) audit_pos = 0;

    /* 2) Send via netlink multicast */
    audit_sequence++;
    audit_netlink_send(AUDIT_EVENT_LOG, tmp, n);
}

void audit_syscall_entry(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!audit_enabled) return;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    char msg[256];
    int n = snprintf(msg, sizeof(msg), "syscall(%llu) pid=%u\n",
                     (unsigned long long)num, pid);
    if (n > 0) {
        /* Write to ring buffer */
        int i;
        for (i = 0; i < n && audit_pos < AUDIT_BUF_SIZE - 1; i++)
            audit_buf[audit_pos++] = msg[i];
        if (audit_pos >= AUDIT_BUF_SIZE) audit_pos = 0;

        /* Send via netlink multicast */
        audit_sequence++;
        audit_netlink_send(AUDIT_EVENT_SYSCALL, msg, n);
    }
}

void audit_syscall_exit(uint64_t ret) {
    if (!audit_enabled) return;

    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    char msg[256];
    int n = snprintf(msg, sizeof(msg), "syscall_exit=%llu pid=%u\n",
                     (unsigned long long)ret, pid);
    if (n > 0) {
        /* Write to ring buffer */
        int i;
        for (i = 0; i < n && audit_pos < AUDIT_BUF_SIZE - 1; i++)
            audit_buf[audit_pos++] = msg[i];
        if (audit_pos >= AUDIT_BUF_SIZE) audit_pos = 0;

        /* Send via netlink multicast */
        audit_sequence++;
        audit_netlink_send(AUDIT_EVENT_SYSCALL, msg, n);
    }
}

int audit_read_log(char *buf, int max) {
    if (!buf || max <= 0) return -1;
    int to_copy = audit_pos < max ? audit_pos : max;
    memcpy(buf, audit_buf, to_copy);
    return to_copy;
}
