#define KERNEL_INTERNAL
#include "ras_netlink.h"
#include "netlink.h"
#include "timer.h"
#include "timers.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "debugfs.h"
#include "errno.h"

/*
 * ── RAS Netlink Implementation ───────────────────────────────────────
 *
 * Provides hardware error event reporting to userspace via AF_NETLINK
 * sockets using the NETLINK_RAS (18) protocol family.
 *
 * Events are delivered via:
 *   1. Netlink multicast to all userspace listeners (group 1)
 *   2. Ring buffer (last 64 events) accessible via debugfs
 *
 * Usage from userspace:
 *   sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_RAS);
 *   addr.nl_family = AF_NETLINK;
 *   addr.nl_pid    = getpid();
 *   addr.nl_groups = 1;  // multicast group for RAS events
 *   bind(sock, (struct sockaddr *)&addr, sizeof(addr));
 *   // now recv() will receive ras_event structs
 */

/* ── Ring buffer ──────────────────────────────────────────────────── */

static struct ras_event g_ras_ring[RAS_RING_SIZE];
static int g_ras_ring_head = 0;   /* next slot to write */
static int g_ras_ring_count = 0;  /* number of valid entries */
static int g_ras_initialized = 0;
static spinlock_t g_ras_lock = SPINLOCK_INIT;

/* ── Ring buffer helpers ──────────────────────────────────────────── */

static void ras_ring_add(const struct ras_event *ev)
{
    g_ras_ring[g_ras_ring_head] = *ev;
    g_ras_ring_head = (g_ras_ring_head + 1) % RAS_RING_SIZE;
    if (g_ras_ring_count < RAS_RING_SIZE)
        g_ras_ring_count++;
}

/* ── Report an event ───────────────────────────────────────────────── */

void ras_netlink_report_event(uint32_t type, uint32_t severity,
                               uint32_t cpu, const char *details)
{
    if (!g_ras_initialized) return;

    struct ras_event ev;
    memset(&ev, 0, sizeof(ev));

    /* Fill event */
    if (timer_available())
        ev.timestamp = timer_get_ticks();
    else
        ev.timestamp = 0;

    ev.type     = type;
    ev.severity = severity;
    ev.cpu      = cpu;

    if (details) {
        size_t dlen = strlen(details);
        if (dlen >= RAS_DETAILS_LEN)
            dlen = RAS_DETAILS_LEN - 1;
        memcpy(ev.details, details, dlen);
        ev.details[dlen] = '\0';
    }

    /* Add to ring buffer */
    spinlock_acquire(&g_ras_lock);
    ras_ring_add(&ev);
    spinlock_release(&g_ras_lock);

    /* Build netlink message */
    struct {
        struct nlmsghdr hdr;
        struct ras_event event;
    } __attribute__((packed)) nl_msg;

    memset(&nl_msg, 0, sizeof(nl_msg));
    nl_msg.hdr.nlmsg_len   = sizeof(nl_msg);
    nl_msg.hdr.nlmsg_type  = RAS_NL_UEVENT;
    nl_msg.hdr.nlmsg_flags = 0;
    nl_msg.hdr.nlmsg_seq   = 0;
    nl_msg.hdr.nlmsg_pid   = 0; /* kernel */
    nl_msg.event = ev;

    /* Deliver via netlink multicast (group 1) */
    netlink_broadcast(NETLINK_RAS, 1, &nl_msg, sizeof(nl_msg), 0);

    /* Also log to kernel log */
    static const char *sev_names[] = { "INFO", "WARN", "CRIT", "FATAL" };
    const char *sev_str = (severity < 4) ? sev_names[severity] : "?";
    static const char *type_names[] = { "", "UE", "CE", "FATAL", "INFO" };
    const char *type_str = (type <= 4) ? type_names[type] : "?";

    kprintf("[RAS] %s/%s cpu=%u: %s\n",
            sev_str, type_str, cpu,
            ev.details[0] ? ev.details : "(no details)");
}

/* ── Debugfs: /sys/kernel/debug/ras/events ──────────────────────────── */

/* Read callback: dump ring buffer contents as text */
static void debugfs_events_read(char *buf, int *len)
{
    if (!buf || !len) return;

    int pos = 0;
    int max = 2048; /* buffer size limit */

    spinlock_acquire(&g_ras_lock);

    int count = g_ras_ring_count;
    int start = 0;
    if (count == RAS_RING_SIZE)
        start = g_ras_ring_head; /* oldest event is at head when full */
    else
        start = 0;

    for (int i = 0; i < count && pos < max - 80; i++) {
        int idx = (start + i) % RAS_RING_SIZE;
        struct ras_event *ev = &g_ras_ring[idx];

        static const char *sev_names[] = { "INFO", "WARN", "CRIT", "FATAL" };
        const char *sev_str = (ev->severity < 4) ? sev_names[ev->severity] : "?";
        static const char *type_names[] = { "", "UE", "CE", "FATAL", "INFO" };
        const char *type_str = (ev->type <= 4) ? type_names[ev->type] : "?";

        int n = snprintf(buf + pos, (size_t)(max - pos),
                         "[%llu] %s/%s cpu=%u: %s\n",
                         (unsigned long long)ev->timestamp,
                         sev_str, type_str,
                         ev->cpu,
                         ev->details[0] ? ev->details : "(none)");
        if (n > 0) pos += n;
    }

    spinlock_release(&g_ras_lock);

    *len = pos;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void ras_netlink_init(void)
{
    if (g_ras_initialized) return;

    memset(g_ras_ring, 0, sizeof(g_ras_ring));
    g_ras_ring_head = 0;
    g_ras_ring_count = 0;

    /* Create debugfs events file at /sys/kernel/debug/ras/events.
     * debugfs uses a flat namespace internally — the '/' in the name
     * is cosmetic, allowing ls /sys/kernel/debug/ to show "ras/events". */
    debugfs_create_file("ras/events", debugfs_events_read);

    g_ras_initialized = 1;
    kprintf("[OK] RAS netlink initialized (NETLINK_RAS=%d, ring=%d events)\n",
            NETLINK_RAS, RAS_RING_SIZE);
}

/* ── Stub: ras_netlink_send ─────────────────────────────── */
int ras_netlink_send(const void *msg, size_t len)
{
    (void)msg;
    (void)len;
    kprintf("[ras] ras_netlink_send: not yet implemented\n");
    return 0;
}
/* ── Stub: ras_netlink_recv ─────────────────────────────── */
int ras_netlink_recv(void *buf, size_t *len)
{
    (void)buf;
    (void)len;
    kprintf("[ras] ras_netlink_recv: not yet implemented\n");
    return 0;
}
