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

#include "heap.h"
#include "netlink.h"
#include "printf.h"
#include "process.h"
#include "string.h"
#include "sysctl.h"
#include "timer.h"

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

    /* Validate event type is within the defined audit message range */
    if (event_type < AUDIT_NLMSG_BASE || event_type > AUDIT_EVENT_USER_CMD)
        return -EINVAL;

    if (!payload) payload = "";
    if (payload_len <= 0) payload_len = (int)strlen(payload);

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

static int sysctl_read_audit_rule(char *buf, int max) {
    int pos = 0;
    int i;

    for (i = 0; i < audit_rule_count() && pos < max - 2; i++) {
        struct audit_rule r;
        if (!audit_rule_get(i, &r))
            break;
        int n = snprintf(buf + pos, (size_t)(max - pos - 2),
                         "rule[%d] syscall=%ld pid=%u uid=%u match=%u\n", i, r.syscall, r.pid,
                         r.uid, r.match);
        if (n > 0)
            pos += n;
    }
    buf[pos] = '\0';
    return pos;
}

static int sysctl_write_audit_rule(const char *buf, int len) {
    /* Accept "syscall=<N>" to install a syscall-filter rule. */
    if (len > 8 && strncmp(buf, "syscall=", 8) == 0) {
        long num = 0;
        int i;
        for (i = 8; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
            num = num * 10 + (long)(buf[i] - '0');

        struct audit_rule r;
        memset(&r, 0, sizeof(r));
        r.match = AUDIT_MATCH_SYSCALL;
        r.syscall = num;
        r.pid = 0;
        return audit_rule_add(&r) >= 0 ? 0 : -1;
    }

    /* Accept "pid=<N>" to install a process-filter rule (log only
     * syscalls issued by this pid). */
    if (len > 4 && strncmp(buf, "pid=", 4) == 0) {
        uint32_t val = 0;
        int i;
        for (i = 4; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
            val = val * 10 + (uint32_t)(buf[i] - '0');

        struct audit_rule r;
        memset(&r, 0, sizeof(r));
        r.match = AUDIT_MATCH_PID;
        r.syscall = -1;
        r.pid = val;
        return audit_rule_add(&r) >= 0 ? 0 : -1;
    }

    /* Accept "uid=<N>" to install a user-filter rule: audit syscalls
     * issued by the given user id. */
    if (len > 4 && strncmp(buf, "uid=", 4) == 0) {
        uint32_t val = 0;
        int i;
        for (i = 4; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
            val = val * 10 + (uint32_t)(buf[i] - '0');

        struct audit_rule r;
        memset(&r, 0, sizeof(r));
        r.match = AUDIT_MATCH_UID;
        r.syscall = -1;
        r.pid = 0;
        r.uid = val;
        return audit_rule_add(&r) >= 0 ? 0 : -1;
    }

    /* Accept "path=<prefix>" to install a filesystem-watch rule: audit
     * any file access whose path starts with <prefix>. */
    if (len > 5 && strncmp(buf, "path=", 5) == 0) {
        int plen = (int)len - 5;
        if (plen >= 1 && plen < (int)sizeof(((struct audit_rule *)0)->path)) {
            struct audit_rule r;
            memset(&r, 0, sizeof(r));
            r.match = AUDIT_MATCH_PATH;
            r.syscall = -1;
            r.pid = 0;
            memcpy(r.path, buf + 5, (size_t)plen);
            r.path[plen] = '\0';
            return audit_rule_add(&r) >= 0 ? 0 : -1;
        }
        return -EINVAL;
    }
    return -EINVAL;
}

/* Register the audit-rule sysctl interface. */
void audit_sysctl_register(void) {
    sysctl_register("audit.rule", sysctl_read_audit_rule, sysctl_write_audit_rule);
}

void __init audit_init(void) {
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
    audit_sysctl_register();
}

/*
 * audit_log_formatted — Send a formatted audit event.
 * Takes a format string and arguments, builds the record, and sends it
 * via both ring buffer and netlink multicast.
 */
static void __printf(2, 3) audit_log_formatted(int event_type, const char *fmt, ...)
{
    if (!audit_enabled || !fmt) return;

    /* Validate event type before formatting and sending */
    if (event_type < AUDIT_NLMSG_BASE || event_type > AUDIT_EVENT_USER_CMD)
        return;

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

    /* Store the syscall number + entry args per-process so the exit
     * record can reproduce a complete, correlated SYSCALL record. */
    struct process *p = process_get_current();
    if (p) {
        p->audit_syscall_num = (int)num;
        p->audit_syscall_args[0] = a1;
        p->audit_syscall_args[1] = a2;
        p->audit_syscall_args[2] = a3;
        p->audit_syscall_args[3] = a4;
    }

    /* Rule-based filtering: if any rule is installed (syscall or pid
     * filter), only emit a SYSCALL record for events the rules accept.
     * With no rules installed (default) everything is logged. */
    if (audit_rule_count() > 0) {
        uint32_t pid = p ? p->pid : 0;
        uint32_t uid = p ? p->uid : 0;
        if (!audit_rule_matches((long)num, pid, uid, NULL))
            return;
    }

    char buf[512];
    audit_format_syscall(buf, sizeof(buf), num, a1, a2, a3, a4, 0);
    audit_log_formatted(AUDIT_EVENT_SYSCALL, "%s", buf);
}

void audit_syscall_exit(uint64_t ret) {
    if (!audit_enabled) return;

    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;
    int syscall_num = p ? p->audit_syscall_num : -1;

    char buf[512];
    int n;
    if (syscall_num >= 0) {
        /* Correlated exit record: reuse the captured entry args so the
         * pair forms one complete SYSCALL record. */
        n = snprintf(buf, sizeof(buf),
                     "type=SYSCALL_EXIT msg=audit(%u): "
                     "syscall=%d exit=%lld pid=%u "
                     "a1=%llx a2=%llx a3=%llx a4=%llx",
                     audit_sequence + 1, syscall_num, (long long)ret, pid,
                     (unsigned long long)(p ? p->audit_syscall_args[0] : 0),
                     (unsigned long long)(p ? p->audit_syscall_args[1] : 0),
                     (unsigned long long)(p ? p->audit_syscall_args[2] : 0),
                     (unsigned long long)(p ? p->audit_syscall_args[3] : 0));
        if (p) {
            p->audit_syscall_num = -1; /* reset */
            p->audit_syscall_args[0] = 0;
            p->audit_syscall_args[1] = 0;
            p->audit_syscall_args[2] = 0;
            p->audit_syscall_args[3] = 0;
        }
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

/* Forward decl of the file-scope builder state variable, which is
 * defined later in this file.  The builder functions themselves are
 * declared in audit.h (already included above). */
static int audit_record_active; /* -1 when idle */

/*
 * audit_log_path — Log a PATH record (used to record a file access).
 * Emits the PATH record, optionally as an AUX record to the current
 * in-progress syscall record (build-a-record then end it).
 */
void audit_log_path(const char *pathname, uint32_t inode, uint32_t mode)
{
    if (!audit_enabled)
        return;

    if (audit_record_active != -1) {
        /* Attach as an AUX record to the in-progress record. */
        audit_log_add_aux("PATH", "name=%s inode=%u mode=%o nametype=NORMAL",
                          pathname ? pathname : "?", inode, mode);
        audit_log_end();
        return;
    }

    if (audit_log_start(AUDIT_EVENT_LOG) < 0)
        return;
    audit_log_format("type=PATH name=%s inode=%u mode=%o nametype=NORMAL",
                     pathname ? pathname : "?", inode, mode);
    audit_log_end();
}

/*
 * audit_log_denial — Log an AVC denial record.
 */
void audit_log_denial(const char *subj, const char *obj, const char *requested) {
    struct process *p;
    uint32_t pid;

    if (!audit_enabled)
        return;

    if (audit_record_active != -1) {
        audit_log_add_aux("AVC", "denied { %s } subj=%s obj=%s", requested ? requested : "unknown",
                          subj ? subj : "kernel", obj ? obj : "unknown");
        audit_log_end();
        return;
    }

    p = process_get_current();
    pid = p ? p->pid : 0;

    if (audit_log_start(AUDIT_EVENT_DENIAL) < 0)
        return;
    audit_log_format("avc: denied { %s } for pid=%u subj=%s obj=%s",
                     requested ? requested : "unknown", pid, subj ? subj : "kernel",
                     obj ? obj : "unknown");
    audit_log_end();
}

int audit_read_log(char *buf, int max) {
    if (!buf || max <= 0) return -EINVAL;
    int to_copy = audit_pos < max ? audit_pos : max;
    memcpy(buf, audit_buf, to_copy);
    return to_copy;
}

/* ── Audit record builder (AUX records) ───────────────────────────── */
/* A single in-progress record is accumulated in a staging buffer, then
 * flushed once on audit_log_end().  An optional trailing AUX record is
 * appended and emitted together, sharing the same audit sequence so a
 * user-space consumer can tie follow-up records (PATH, EXECVE, ...) to
 * the main SYSCALL record. */

#define AUDIT_RECORD_SIZE 512
#define AUDIT_AUX_SIZE 256

static char audit_record_buf[AUDIT_RECORD_SIZE];
static int audit_record_pos;
static int audit_record_active; /* event type, or -1 when idle */
static char audit_record_aux[AUDIT_AUX_SIZE];
static int audit_record_aux_active;

/* Internal: reset the record builder to idle. */
static void audit_record_reset(void) {
    audit_record_pos = 0;
    audit_record_active = -1;
    audit_record_aux_active = 0;
    audit_record_aux[0] = '\0';
}

int audit_log_start(int type) {
    /* Event type must be within the defined audit message range. */
    if (type < AUDIT_NLMSG_BASE || type > AUDIT_EVENT_USER)
        return -EINVAL;

    /* If a record is already in progress, finalize it first so a
     * half-built record is never overwritten/lost. */
    if (audit_record_active != -1)
        audit_log_end();

    audit_record_active = type;
    audit_record_pos = 0;
    audit_record_aux_active = 0;
    audit_record_aux[0] = '\0';
    audit_record_buf[0] = '\0';
    return 0;
}

void audit_log_format(const char *fmt, ...) {
    __builtin_va_list args;
    int n;

    if (audit_record_active == -1 || !fmt)
        return;

    __builtin_va_start(args, fmt);
    n = vsnprintf(audit_record_buf + audit_record_pos, AUDIT_RECORD_SIZE - (size_t)audit_record_pos,
                  fmt, args);
    __builtin_va_end(args);

    if (n > 0) {
        int new_pos = audit_record_pos + n;
        if (new_pos > AUDIT_RECORD_SIZE - 1)
            new_pos = AUDIT_RECORD_SIZE - 1;
        audit_record_pos = new_pos;
    }
}

int audit_log_add_aux(const char *aux_type, const char *fmt, ...) {
    __builtin_va_list args;
    char tmp[AUDIT_AUX_SIZE];
    int n;

    if (audit_record_active == -1 || !aux_type || !fmt)
        return -EINVAL;

    __builtin_va_start(args, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    __builtin_va_end(args);
    if (n <= 0)
        return 0;

    /* Format: "type=<AUX> msg=audit(<seq>): <body>\n" */
    char line[AUDIT_AUX_SIZE + 64];
    int line_len = snprintf(line, sizeof(line), "type=%s msg=audit(%u): %s\n", aux_type,
                            audit_sequence + 1, tmp);
    if (line_len <= 0)
        return -EINVAL;

    /* Concatenate onto the aux stash (best-effort). */
    size_t cur = strlen(audit_record_aux);
    if (cur + (size_t)line_len >= sizeof(audit_record_aux))
        return -ENOSPC;
    memcpy(audit_record_aux + cur, line, (size_t)line_len);
    audit_record_aux[cur + (size_t)line_len] = '\0';
    audit_record_aux_active = 1;
    return 0;
}

void audit_log_end(void) {
    if (audit_record_active == -1)
        return;

    int type = audit_record_active;

    /* Emit the main record, then any attached AUX records. */
    if (audit_record_pos > 0) {
        int i;
        /* 1) Ring buffer */
        for (i = 0; i < audit_record_pos && audit_pos < AUDIT_BUF_SIZE - 1; i++)
            audit_buf[audit_pos++] = audit_record_buf[i];
        if (audit_pos >= AUDIT_BUF_SIZE)
            audit_pos = 0;
        /* 2) Netlink multicast */
        audit_sequence++;
        audit_netlink_send(type, audit_record_buf, audit_record_pos);
    }
    if (audit_record_aux_active) {
        int aux_len = (int)strlen(audit_record_aux);
        int i;
        for (i = 0; i < aux_len && audit_pos < AUDIT_BUF_SIZE - 1; i++)
            audit_buf[audit_pos++] = audit_record_aux[i];
        if (audit_pos >= AUDIT_BUF_SIZE)
            audit_pos = 0;
        /* Deliver AUX lines as individual LOG netlink messages so the
         * sequence is preserved for correlation. */
        char *p = audit_record_aux;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (!nl)
                break;
            *nl = '\0';
            audit_sequence++;
            audit_netlink_send(AUDIT_EVENT_LOG, p, (int)strlen(p));
            p = nl + 1;
        }
    }

    audit_record_reset();
}

/* Single-call generic audit log: start a LOG record, format the message,
 * end and emit. */
void audit_log(const char *msg) {
    if (!audit_enabled || !msg)
        return;

    if (audit_log_start(AUDIT_EVENT_LOG) < 0)
        return;
    audit_log_format("msg='%s'", msg);
    audit_log_end();
}

/* USER_CMD record: log a command executed by a user (via exec). */
void audit_log_user_command(const char *cmdline) {
    struct process *p;
    char buf[512];
    int n;

    if (!audit_enabled || !cmdline)
        return;

    /* Sanitize: replace CR/LF so the record stays a single line. */
    n = 0;
    while (cmdline[n] && n < (int)sizeof(buf) - 16) {
        char c = cmdline[n];
        if (c == '\n' || c == '\r')
            c = ' ';
        buf[n++] = c;
    }
    buf[n] = '\0';

    if (audit_log_start(AUDIT_EVENT_USER_CMD) < 0)
        return;

    p = process_get_current();
    audit_log_format("cmd='%s' pid=%u uid=%u", buf, p ? p->pid : 0, p ? p->uid : 0);
    audit_log_end();
}

/* ── Stub: audit_send_reply ─────────────────────────────────────────── */
static int audit_send_reply(void *skb, int type, int done, int seq, const void *data, int len)
{
    (void)skb; (void)type; (void)done; (void)seq; (void)data; (void)len;
    kprintf("[AUDIT] audit_send_reply: not yet implemented\n");
    return 0;
}
