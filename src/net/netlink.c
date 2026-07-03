/*
 * netlink.c — AF_NETLINK socket family (Item 384)
 *
 * Provides AF_NETLINK sockets for kernel-userspace communication.
 * Supports:
 *   - NETLINK_GENERIC: extensible family multiplexer
 *   - NETLINK_KOBJECT_UEVENT: device event notification
 *   - NETLINK_ROUTE: routing/netlink (future)
 *   - Unicast delivery by port ID
 *   - Multicast group delivery
 *   - Generic netlink family registration
 *
 * Reference: netlink(7) man page, Linux net/netlink/af_netlink.c
 *            Linux include/net/genetlink.h
 */
#define KERNEL_INTERNAL
#include "types.h"
#include "netlink.h"
#include "socket.h"
#include "process.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "heap.h"
#include "scheduler.h"
#include "waitqueue.h"
#include "timer.h"
#include "pkt_sched.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/* Starting netlink socket fd range */
#define NETLINK_FD_BASE     1100

/* Receive buffer size per socket */
#define NETLINK_RECV_BUF    8192

/* ── Netlink socket table ──────────────────────────────────────────── */

static struct netlink_sock netlink_table[NETLINK_MAX_SOCKETS];
static int netlink_initialized = 0;
static spinlock_t netlink_lock;

/* ── Generic netlink family table ──────────────────────────────────── */

static struct genl_family genl_families[GENL_MAX_FAMILIES];
static int next_genl_family_id = GENL_ID_CTRL + 1;  /* Next available ID */

/* ── Forward declarations ──────────────────────────────────────────── */
static int genl_handle_ctrl(const void *buf, int len, uint32_t src_pid);
static int netlink_handle_kernel(int protocol, const void *buf, int len,
                                  uint32_t src_pid);

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Convert an fd to a slot index */
static int netlink_fd_to_slot(int fd) {
    int slot = fd - NETLINK_FD_BASE;
    if (slot < 0 || slot >= NETLINK_MAX_SOCKETS) return -EINVAL;
    return slot;
}

/* Find a free socket slot (caller must hold netlink_lock) */
static int netlink_alloc_slot(void) {
    for (int i = 0; i < NETLINK_MAX_SOCKETS; i++) {
        if (!netlink_table[i].used) {
            memset(&netlink_table[i], 0, sizeof(struct netlink_sock));
            netlink_table[i].used = 1;
            return i;
        }
    }
    return -EINVAL;
}

/* Find a socket by port ID and protocol (caller must NOT hold lock) */
static __attribute__((unused)) struct netlink_sock *netlink_find_by_portid(int protocol, uint32_t portid) {
    spinlock_acquire(&netlink_lock);
    for (int i = 0; i < NETLINK_MAX_SOCKETS; i++) {
        if (netlink_table[i].used &&
            netlink_table[i].protocol == protocol &&
            netlink_table[i].bound &&
            netlink_table[i].portid == portid) {
            spinlock_release(&netlink_lock);
            return &netlink_table[i];
        }
    }
    spinlock_release(&netlink_lock);
    return NULL;
}

/* ── Initialization ────────────────────────────────────────────────── */

void af_netlink_init(void) {
    if (netlink_initialized) return;
    memset(netlink_table, 0, sizeof(netlink_table));
    memset(genl_families, 0, sizeof(genl_families));
    spinlock_init(&netlink_lock);
    netlink_initialized = 1;

    /* Register the controller family (GENL_ID_CTRL = 0x10).
     * The controller is the generic netlink control family used
     * for family discovery. */
    genl_families[0].id = GENL_ID_CTRL;
    strncpy(genl_families[0].name, "ctrl", GENL_NAMSIZ);
    genl_families[0].version = 1;
    genl_families[0].maxattr = CTRL_ATTR_MAXATTR;
    genl_families[0].registered = 1;
    next_genl_family_id = GENL_ID_CTRL + 2;  /* Skip 0x10, 0x11 */

    kprintf("[OK] af_netlink: initialized (domain=%d, max %d sockets)\n",
            AF_NETLINK, NETLINK_MAX_SOCKETS);

    /* Initialise RTNETLINK handlers */
    netlink_rtnl_init();
}

/* ── Socket lifecycle ──────────────────────────────────────────────── */

int netlink_create(int fd, int protocol) {
    if (!netlink_initialized) return -EINVAL;
    if (protocol < 0 || protocol > 255) return -EINVAL;

    spinlock_acquire(&netlink_lock);
    int slot = netlink_alloc_slot();
    if (slot < 0) {
        spinlock_release(&netlink_lock);
        return -EINVAL;
    }

    struct netlink_sock *nl = &netlink_table[slot];
    nl->fd = fd;
    nl->protocol = protocol;
    nl->portid = 0;         /* Will be assigned on bind() */
    nl->groups = 0;
    nl->bound = 0;

    /* Allocate receive buffer */
    nl->recv_buf = (struct nlmsghdr *)kmalloc(NETLINK_RECV_BUF);
    if (!nl->recv_buf) {
        nl->used = 0;
        spinlock_release(&netlink_lock);
        return -ENOMEM;
    }
    memset(nl->recv_buf, 0, NETLINK_RECV_BUF);
    nl->recv_size = NETLINK_RECV_BUF;
    nl->recv_used = 0;
    nl->recv_pos = 0;

    spinlock_release(&netlink_lock);
    return 0;
}

int netlink_bind(int fd, const struct sockaddr_nl *addr) {
    if (!netlink_initialized || !addr) return -EINVAL;
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used) {
        spinlock_release(&netlink_lock);
        return -EBUSY;
    }

    /* If already bound, unbind first */
    if (nl->bound) {
        spinlock_release(&netlink_lock);
        return -EBUSY; /* EINVAL: already bound */
    }

    uint32_t new_pid = addr->nl_pid;

    /* Validate groups mask */
    if (addr->nl_groups & ~((1ULL << NETLINK_MAX_GROUPS) - 1)) {
        spinlock_release(&netlink_lock);
        return -EINVAL; /* EINVAL */
    }

    /* If port ID is 0, auto-assign one based on current process PID */
    if (new_pid == 0) {
        struct process *proc = process_get_current();
        if (proc) {
            new_pid = (uint32_t)(proc->pid & 0xFFFFFFFF);
        } else {
            new_pid = (uint32_t)(timer_get_ticks() & 0x7FFFFFFF);
        }
    }

    /* Check for duplicate port ID on this protocol */
    for (int i = 0; i < NETLINK_MAX_SOCKETS; i++) {
        if (i != slot && netlink_table[i].used &&
            netlink_table[i].protocol == nl->protocol &&
            netlink_table[i].bound &&
            netlink_table[i].portid == new_pid) {
            spinlock_release(&netlink_lock);
            return -EINVAL; /* EADDRINUSE */
        }
    }

    nl->portid = new_pid;
    nl->groups = addr->nl_groups;
    nl->bound = 1;

    spinlock_release(&netlink_lock);
    return 0;
}

int netlink_send(int fd, const void *buf, int len) {
    if (!netlink_initialized || !buf || len < (int)sizeof(struct nlmsghdr)) return -EINVAL;
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used || !nl->bound) {
        spinlock_release(&netlink_lock);
        return -EINVAL;
    }

    const struct nlmsghdr *hdr = (const struct nlmsghdr *)buf;
    if ((unsigned int)len < hdr->nlmsg_len || hdr->nlmsg_len < sizeof(struct nlmsghdr)) {
        spinlock_release(&netlink_lock);
        return -EINVAL; /* EINVAL */
    }

    uint32_t dst_portid = hdr->nlmsg_pid; /* Destination port ID (from header) */
    uint32_t src_portid = nl->portid;

    /* If destination is 0, it goes to the kernel */
    if (dst_portid == 0) {
        /* Kernel destination — handle generic netlink commands */
        spinlock_release(&netlink_lock);
        return netlink_handle_kernel(nl->protocol, buf, len, src_portid);
    }

    /* Unicast to another netlink socket */
    spinlock_release(&netlink_lock);
    int ret = netlink_unicast(nl->protocol, dst_portid, buf, len, src_portid);
    if (ret == 0) {
        spinlock_acquire(&netlink_lock);
        nl->msgs_sent++;
        spinlock_release(&netlink_lock);
        return len;
    }

    return -EINVAL;
}

int netlink_recv(int fd, void *buf, int max_len) {
    if (!netlink_initialized || !buf || max_len <= 0) return -EINVAL;
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used || !nl->bound) {
        spinlock_release(&netlink_lock);
        return -EINVAL;
    }

    /* No data available */
    if (nl->recv_used <= 0) {
        spinlock_release(&netlink_lock);
        return -EAGAIN; /* EAGAIN */
    }

    /* Copy out as much as we can */
    int to_copy = nl->recv_used;
    if (to_copy > max_len) to_copy = max_len;

    memcpy(buf, nl->recv_buf, to_copy);

    /* Remove consumed data from buffer (shift remaining) */
    int remaining = nl->recv_used - to_copy;
    if (remaining > 0) {
        memmove(nl->recv_buf, (uint8_t *)nl->recv_buf + to_copy, remaining);
    }
    nl->recv_used = remaining;
    nl->recv_pos = 0;

    nl->msgs_recv++;

    spinlock_release(&netlink_lock);
    return to_copy;
}

/* ── msghdr-based sendmsg ──────────────────────────────────────────── */

/*
 * netlink_sendmsg — Send a netlink message from a struct msghdr.
 *
 * Flattens all iovecs into a single netlink message buffer, validates
 * the nlmsghdr, and honors msg_name / msg_namelen for an explicit
 * destination sockaddr_nl (overriding nlmsg_pid from the header).
 *
 * Returns the total number of bytes sent on success, or a negative errno.
 */
int netlink_sendmsg(int fd, const struct msghdr *msg, int flags)
{
    (void)flags;

    if (!netlink_initialized || !msg) return -EINVAL;
    if (msg->msg_iovlen < 1 || !msg->msg_iov) return -EINVAL;

    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -EINVAL;

    spinlock_acquire(&netlink_lock);
    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used || !nl->bound) {
        spinlock_release(&netlink_lock);
        return -EINVAL;
    }
    uint32_t src_portid = nl->portid;
    int protocol = nl->protocol;
    spinlock_release(&netlink_lock);

    /* Calculate total payload length from all iovecs */
    uint64_t total = 0;
    for (uint32_t i = 0; i < msg->msg_iovlen; i++) {
        total += msg->msg_iov[i].iov_len;
    }

    if (total < sizeof(struct nlmsghdr))
        return -EINVAL;
    if (total > NETLINK_MAX_PAYLOAD)
        return -EMSGSIZE;

    /* Allocate a contiguous buffer and flatten the iovecs */
    struct nlmsghdr *buf = (struct nlmsghdr *)kmalloc(total);
    if (!buf)
        return -ENOMEM;

    uint64_t offset = 0;
    for (uint32_t i = 0; i < msg->msg_iovlen; i++) {
        uint64_t len = msg->msg_iov[i].iov_len;
        if (len == 0) continue;
        memcpy((uint8_t *)buf + offset, msg->msg_iov[i].iov_base, len);
        offset += len;
    }

    /* Validate the netlink message header */
    if (buf->nlmsg_len < sizeof(struct nlmsghdr) ||
        buf->nlmsg_len > total) {
        kfree(buf);
        return -EINVAL;
    }

    /* Determine destination port ID from msg_name if provided,
     * otherwise fall back to nlmsg_pid in the header. */
    uint32_t dst_portid = buf->nlmsg_pid;
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_nl)) {
        const struct sockaddr_nl *snl =
            (const struct sockaddr_nl *)msg->msg_name;
        if (snl->nl_family == AF_NETLINK) {
            dst_portid = snl->nl_pid;
        }
    }

    int ret;
    if (dst_portid == 0) {
        /* Kernel destination — dispatch internally */
        ret = netlink_handle_kernel(protocol, buf, (int)total, src_portid);
        if (ret == 0)
            ret = (int)total;
    } else {
        /* Unicast to another netlink socket */
        ret = netlink_unicast(protocol, dst_portid, buf, (int)total,
                              src_portid);
        if (ret == 0) {
            spinlock_acquire(&netlink_lock);
            nl->msgs_sent++;
            spinlock_release(&netlink_lock);
            ret = (int)total;
        } else {
            ret = -EINVAL;
        }
    }

    kfree(buf);
    return ret;
}

/* ── msghdr-based recvmsg ──────────────────────────────────────────── */

/*
 * netlink_recvmsg — Receive a netlink message into a struct msghdr.
 *
 * Reads a pending netlink message into the caller's iovec buffers,
 * fills msg_name with the source sockaddr_nl (kernel as sender),
 * and sets msg_flags.
 *
 * Returns the number of bytes received on success, or a negative errno.
 */
int netlink_recvmsg(int fd, struct msghdr *msg, int flags)
{
    (void)flags;

    if (!netlink_initialized || !msg) return -EINVAL;
    if (msg->msg_iovlen < 1 || !msg->msg_iov) return -EINVAL;

    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -EINVAL;

    spinlock_acquire(&netlink_lock);
    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used || !nl->bound) {
        spinlock_release(&netlink_lock);
        return -EINVAL;
    }
    int avail = nl->recv_used;
    spinlock_release(&netlink_lock);

    if (avail <= 0)
        return -EAGAIN;

    /* Calculate total receive capacity */
    uint64_t bufcap = 0;
    for (uint32_t i = 0; i < msg->msg_iovlen; i++)
        bufcap += msg->msg_iov[i].iov_len;

    if (bufcap == 0)
        return -EINVAL;

    /* Read into the first iovec buffer (netlink messages are small) */
    void *buf = msg->msg_iov[0].iov_base;
    uint64_t first_len = msg->msg_iov[0].iov_len;
    uint64_t read_len = (uint64_t)avail;
    if (read_len > first_len)
        read_len = first_len;
    if (read_len > NETLINK_MAX_PAYLOAD)
        read_len = NETLINK_MAX_PAYLOAD;

    int n = netlink_recv(fd, buf, (int)read_len);
    if (n < 0) return n;

    /* Fill source address (kernel is the sender for received messages) */
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_nl)) {
        struct sockaddr_nl *snl = (struct sockaddr_nl *)msg->msg_name;
        memset(snl, 0, sizeof(struct sockaddr_nl));
        snl->nl_family = AF_NETLINK;
        snl->nl_pid = 0;   /* From kernel */
        snl->nl_groups = 0;
        msg->msg_namelen = sizeof(struct sockaddr_nl);
    }

    /* Set flags */
    msg->msg_flags = 0;
    if ((unsigned int)n < (unsigned int)avail && msg->msg_iovlen == 1)
        msg->msg_flags |= MSG_TRUNC;

    return n;
}

void netlink_close(int fd) {
    if (!netlink_initialized) return;
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return;

    spinlock_acquire(&netlink_lock);

    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used) {
        spinlock_release(&netlink_lock);
        return;
    }

    /* Free receive buffer */
    if (nl->recv_buf) {
        kfree(nl->recv_buf);
        nl->recv_buf = NULL;
    }

    nl->used = 0;
    nl->bound = 0;

    spinlock_release(&netlink_lock);
}

/* ── Kernel message handling ───────────────────────────────────────── */

/* Forward into the receive buffer of a matching socket.
 * Returns 0 on success, -1 if no matching socket or buffer full. */
static int netlink_deliver_internal(struct netlink_sock *nl,
                                     const void *data, int len) {
    if (!nl || !nl->used) return -ENOSPC;

    /* Check if message fits in the buffer */
    if (nl->recv_used + len > nl->recv_size) {
        nl->msgs_dropped++;
        return -EINVAL; /* ENOSPC */
    }

    memcpy((uint8_t *)nl->recv_buf + nl->recv_used, data, len);
    nl->recv_used += len;

    return 0;
}

/* ── Handler registration table ──────────────────────────────────── */

static struct netlink_handler nl_handlers[NETLINK_MAX_HANDLERS];
static int nl_handlers_initialized = 0;

/* Initialise the handler table */
static void nl_handler_init(void)
{
    if (nl_handlers_initialized) return;
    memset(nl_handlers, 0, sizeof(nl_handlers));
    nl_handlers_initialized = 1;
}

/* Find a free handler slot */
static int nl_handler_alloc_slot(void)
{
    for (int i = 0; i < NETLINK_MAX_HANDLERS; i++) {
        if (!nl_handlers[i].in_use)
            return i;
    }
    return -ENOSPC;
}

int netlink_register_handler(int protocol, uint16_t msg_type,
                              nlmsg_handler_t handler, const char *name)
{
    return netlink_register_handler_range(protocol, msg_type, msg_type,
                                           handler, name);
}

int netlink_register_handler_range(int protocol,
                                    uint16_t msg_type_min,
                                    uint16_t msg_type_max,
                                    nlmsg_handler_t handler,
                                    const char *name)
{
    if (!nl_handlers_initialized) nl_handler_init();
    if (!handler) return -EINVAL;
    if (msg_type_min > msg_type_max) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    /* Check for duplicate (same protocol + overlapping range) */
    for (int i = 0; i < NETLINK_MAX_HANDLERS; i++) {
        if (!nl_handlers[i].in_use) continue;
        if (nl_handlers[i].protocol != protocol) continue;
        if (nl_handlers[i].handler == handler) {
            spinlock_release(&netlink_lock);
            return -EEXIST;
        }
        if (msg_type_min <= nl_handlers[i].msg_type_max &&
            msg_type_max >= nl_handlers[i].msg_type_min) {
            spinlock_release(&netlink_lock);
            return -EBUSY;
        }
    }

    int slot = nl_handler_alloc_slot();
    if (slot < 0) {
        spinlock_release(&netlink_lock);
        return -ENOSPC;
    }

    nl_handlers[slot].in_use = 1;
    nl_handlers[slot].protocol = protocol;
    nl_handlers[slot].msg_type_min = msg_type_min;
    nl_handlers[slot].msg_type_max = msg_type_max;
    nl_handlers[slot].handler = handler;
    nl_handlers[slot].name = name;

    kprintf("[netlink] register handler \"%s\" proto=%d type=[%u,%u]\\n",
            name, protocol, msg_type_min, msg_type_max);

    spinlock_release(&netlink_lock);
    return 0;
}

int netlink_unregister_handler(int protocol, nlmsg_handler_t handler)
{
    if (!nl_handlers_initialized) return -EINVAL;
    if (!handler) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    for (int i = 0; i < NETLINK_MAX_HANDLERS; i++) {
        if (nl_handlers[i].in_use &&
            nl_handlers[i].protocol == protocol &&
            nl_handlers[i].handler == handler) {
            nl_handlers[i].in_use = 0;
            spinlock_release(&netlink_lock);
            return 0;
        }
    }

    spinlock_release(&netlink_lock);
    return -EINVAL;
}

/* Find a handler matching protocol + message type.
 * Caller must hold netlink_lock.  Returns pointer or NULL. */
static struct netlink_handler *nl_handler_find(int protocol,
                                                uint16_t msg_type)
{
    for (int i = 0; i < NETLINK_MAX_HANDLERS; i++) {
        if (!nl_handlers[i].in_use) continue;
        if (nl_handlers[i].protocol != protocol) continue;
        if (msg_type >= nl_handlers[i].msg_type_min &&
            msg_type <= nl_handlers[i].msg_type_max) {
            return &nl_handlers[i];
        }
    }
    return NULL;
}

/* ── Attribute parsing ───────────────────────────────────────────── */

/*
 * nla_parse — Parse netlink attributes according to a policy.
 *
 * Walks the attribute stream, validates each attribute against the
 * policy, and stores a pointer for each type in the tb[] output array.
 * Attributes with type > maxtype are silently skipped (forward-compat).
 */
int nla_parse(const struct nlattr *data, size_t len, int maxtype,
              const struct nla_policy *policy, struct nlattr **tb)
{
    const struct nlattr *nla;
    size_t remaining;

    if (!tb) return -EINVAL;

    /* Clear output array */
    memset(tb, 0, (size_t)(maxtype + 1) * sizeof(struct nlattr *));

    if (!data || len == 0)
        return 0;

    nla = data;
    remaining = len;

    while (NLA_OK(nla, (int)remaining)) {
        uint16_t attr_type = nla->nla_type & NLA_TYPE_MASK;
        uint16_t attr_len = nla->nla_len;

        /* Silently skip types > maxtype */
        if (attr_type > (uint16_t)maxtype) {
            NLA_NEXT(nla, remaining);
            continue;
        }

        /* Validate against policy if provided */
        if (policy && attr_type <= (uint16_t)maxtype) {
            uint16_t min_len = policy[attr_type].minlen;
            uint16_t max_len = policy[attr_type].maxlen;

            if (attr_len < NLA_HDRLEN)
                return -EBADMSG;

            int payload_len = (int)attr_len - NLA_HDRLEN;

            if (min_len > 0 && payload_len < (int)min_len)
                return -ERANGE;
            if (max_len > 0 && payload_len > (int)max_len)
                return -ERANGE;
        }

        /* Skip duplicate attributes */
        if (tb[attr_type]) {
            NLA_NEXT(nla, remaining);
            continue;
        }

        tb[attr_type] = (struct nlattr *)(uintptr_t)nla;

        NLA_NEXT(nla, remaining);
    }

    return 0;
}

/*
 * nlmsg_parse — Parse attributes from a netlink message payload.
 *
 * Accounts for a fixed-size protocol header (e.g. struct genlmsghdr)
 * between the nlmsghdr and the attribute area.
 */
int nlmsg_parse(const struct nlmsghdr *nlh, int hdrlen, int maxtype,
                const struct nla_policy *policy, struct nlattr **tb)
{
    if (!nlh) return -EINVAL;
    if (hdrlen < 0) return -EINVAL;

    if (nlh->nlmsg_len < NLMSG_HDRLEN + (uint32_t)hdrlen)
        return -EBADMSG;

    int payload_len = (int)nlh->nlmsg_len - NLMSG_HDRLEN - hdrlen;
    if (payload_len < 0)
        payload_len = 0;

    const struct nlattr *attrs = NULL;
    if (payload_len > 0) {
        attrs = (const struct nlattr *)((const char *)nlh
                + NLMSG_HDRLEN + hdrlen);
    }

    return nla_parse(attrs, (size_t)payload_len, maxtype, policy, tb);
}

/*
 * nla_find — Linear search for an attribute by type.
 */
struct nlattr *nla_find(const struct nlattr *nla, size_t nla_len,
                         uint16_t type)
{
    const struct nlattr *cur = nla;
    size_t remaining = nla_len;

    while (NLA_OK(cur, (int)remaining)) {
        if ((cur->nla_type & NLA_TYPE_MASK) == type)
            return (struct nlattr *)(uintptr_t)cur;
        NLA_NEXT(cur, remaining);
    }

    return NULL;
}

/* ── Typed attribute getters ─────────────────────────────────────── */

int nla_get_u8(const struct nlattr *nla, uint8_t *val)
{
    if (!nla || !val) return -EINVAL;
    if (nla->nla_len < NLA_HDRLEN + sizeof(uint8_t)) return -ERANGE;
    *val = *(const uint8_t *)nla_data(nla);
    return 0;
}

int nla_get_u16(const struct nlattr *nla, uint16_t *val)
{
    if (!nla || !val) return -EINVAL;
    if (nla->nla_len < NLA_HDRLEN + sizeof(uint16_t)) return -ERANGE;
    *val = *(const uint16_t *)nla_data(nla);
    return 0;
}

int nla_get_u32(const struct nlattr *nla, uint32_t *val)
{
    if (!nla || !val) return -EINVAL;
    if (nla->nla_len < NLA_HDRLEN + sizeof(uint32_t)) return -ERANGE;
    *val = *(const uint32_t *)nla_data(nla);
    return 0;
}

int nla_get_u64(const struct nlattr *nla, uint64_t *val)
{
    if (!nla || !val) return -EINVAL;
    if (nla->nla_len < NLA_HDRLEN + sizeof(uint64_t)) return -ERANGE;
    *val = *(const uint64_t *)nla_data(nla);
    return 0;
}

int nla_get_string(const struct nlattr *nla, const char **str)
{
    if (!nla || !str) return -EINVAL;
    if (nla->nla_len <= NLA_HDRLEN) return -ERANGE;
    *str = (const char *)nla_data(nla);
    if ((*str)[nla_len(nla) - 1] != '\0')
        return -ERANGE;
    return 0;
}

/* ── Enhanced kernel message dispatch ────────────────────────────── */

/* Dispatch a netlink message to registered handlers for the given
 * protocol.  Returns 0 if a handler claimed the message, negative
 * errno otherwise. */
static int nl_dispatch_to_handlers(int protocol,
                                    const struct nlmsghdr *nlh,
                                    uint32_t src_pid)
{
    spinlock_acquire(&netlink_lock);
    struct netlink_handler *h = nl_handler_find(protocol,
                                                 nlh->nlmsg_type);
    if (!h) {
        spinlock_release(&netlink_lock);
        return -ENOENT;
    }
    nlmsg_handler_t handler_fn = h->handler;
    spinlock_release(&netlink_lock);

    return handler_fn(protocol, nlh, NULL, src_pid);
}

/* ── RTNETLINK (NETLINK_ROUTE) handler ───────────────────────────── */

/* Handle RTM_GETROUTE — return route table dump (stub: empty) */
static int nl_rt_getroute(int protocol, const struct nlmsghdr *nlh,
                           const struct nlattr **attr, uint32_t src_pid)
{
    (void)attr;
    struct {
        struct nlmsghdr nlh;
        struct rtmsg rtm;
    } __attribute__((packed)) resp;

    memset(&resp, 0, sizeof(resp));
    resp.nlh.nlmsg_len = sizeof(resp);
    resp.nlh.nlmsg_type = RTM_NEWROUTE;
    resp.nlh.nlmsg_flags = NLM_F_MULTI;
    resp.nlh.nlmsg_seq = nlh->nlmsg_seq;
    resp.nlh.nlmsg_pid = 0;
    resp.rtm.rtm_family = AF_UNSPEC;
    resp.rtm.rtm_table = RT_TABLE_MAIN;
    resp.rtm.rtm_type = RTN_UNICAST;
    resp.rtm.rtm_scope = RT_SCOPE_NOWHERE;
    resp.rtm.rtm_protocol = RTPROT_KERNEL;

    netlink_unicast(protocol, src_pid, &resp, sizeof(resp), 0);

    /* Send DONE to terminate multipart sequence */
    {
        struct nlmsghdr done;
        memset(&done, 0, sizeof(done));
        done.nlmsg_len = sizeof(done);
        done.nlmsg_type = NLMSG_DONE;
        done.nlmsg_flags = 0;
        done.nlmsg_seq = nlh->nlmsg_seq;
        done.nlmsg_pid = 0;
        netlink_unicast(protocol, src_pid, &done, sizeof(done), 0);
    }

    return 0;
}

/* Handle RTM_NEWROUTE — accept route addition (stub) */
static int nl_rt_newroute(int protocol, const struct nlmsghdr *nlh,
                           const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)nlh;
    (void)attr;
    (void)src_pid;
    return 0;
}

/* Handle RTM_DELROUTE — accept route deletion (stub) */
static int nl_rt_delroute(int protocol, const struct nlmsghdr *nlh,
                           const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)nlh;
    (void)attr;
    (void)src_pid;
    return 0;
}

/* ── TC (Traffic Control) netlink handlers ────────────────────────── */

/* Qdisc kind name strings */
static const char *tc_kind_name(int type)
{
    switch (type) {
        case QDISC_PFIFO_FAST: return "pfifo_fast";
        case QDISC_FQ_CODEL:   return "fq_codel";
        case QDISC_HTB:        return "htb";
        case QDISC_TBF:        return "tbf";
        case QDISC_FQ:         return "fq";
        case QDISC_RED:        return "red";
        case QDISC_CAKE:       return "cake";
        default:               return "unknown";
    }
}

/* Map a qdisc kind string to QDISC_* value, or -1 if unknown */
static int tc_kind_to_type(const char *kind)
{
    if (!kind) return -1;
    if (strcmp(kind, "pfifo_fast") == 0) return QDISC_PFIFO_FAST;
    if (strcmp(kind, "fq_codel") == 0)   return QDISC_FQ_CODEL;
    if (strcmp(kind, "htb") == 0)        return QDISC_HTB;
    if (strcmp(kind, "tbf") == 0)        return QDISC_TBF;
    if (strcmp(kind, "fq") == 0)         return QDISC_FQ;
    if (strcmp(kind, "red") == 0)        return QDISC_RED;
    if (strcmp(kind, "cake") == 0)       return QDISC_CAKE;
    return -1;
}

/* Send a NLMSG_DONE message to terminate a multipart RTNETLINK dump.
 * Helper used by several TC handlers below. */
static void tc_send_done(int protocol, uint32_t src_pid, uint32_t seq)
{
    struct nlmsghdr done;
    memset(&done, 0, sizeof(done));
    done.nlmsg_len = sizeof(done);
    done.nlmsg_type = NLMSG_DONE;
    done.nlmsg_seq = seq;
    done.nlmsg_pid = 0;
    netlink_unicast(protocol, src_pid, &done, sizeof(done), 0);
}

/* Handle RTM_GETQDISC — dump all registered qdiscs.
 * Supports NLM_F_ROOT (dump all) and handles individual qdisc lookup
 * via tcm_handle.  Returns multipart NEWQDISC messages + NLMSG_DONE. */
static int nl_tc_getqdisc(int protocol, const struct nlmsghdr *nlh,
                           const struct nlattr **attr, uint32_t src_pid)
{
    (void)attr;
    int dump_all = (nlh->nlmsg_flags & NLM_F_ROOT) || (nlh->nlmsg_flags & NLM_F_DUMP);

    if (nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(struct tcmsg))
        return -EINVAL;

    const struct tcmsg *tcm = (const struct tcmsg *)((const char *)nlh + NLMSG_HDRLEN);

    if (!dump_all && tcm->tcm_handle != 0) {
        /* Single qdisc lookup requested — scan by handle.
         * The handle is our encoding: maj=1, min=index+1 */
        int idx = TC_H_MIN(tcm->tcm_handle);
        if (idx < 1) return -ENOENT;
        idx--;

        char devname[32];
        struct qdisc *q = tc_get_qdisc_by_index(idx, devname, sizeof(devname));
        if (!q) return -ENOENT;

        uint8_t buf[256];
        struct nlmsghdr *resp = (struct nlmsghdr *)buf;
        struct tcmsg *rtcm = (struct tcmsg *)(buf + NLMSG_HDRLEN);
        struct nlattr *nla = (struct nlattr *)(buf + NLMSG_HDRLEN + sizeof(struct tcmsg));

        memset(buf, 0, sizeof(buf));
        resp->nlmsg_len = NLMSG_HDRLEN + sizeof(struct tcmsg);
        resp->nlmsg_type = RTM_NEWQDISC;
        resp->nlmsg_flags = 0;
        resp->nlmsg_seq = nlh->nlmsg_seq;
        resp->nlmsg_pid = 0;

        rtcm->tcm_family = AF_UNSPEC;
        rtcm->tcm_ifindex = 1;
        rtcm->tcm_handle = TC_H_MAKE(1, idx + 1);
        rtcm->tcm_parent = TC_H_ROOT;
        rtcm->tcm_info = (uint32_t)q->type << 16;

        const char *kind = tc_kind_name(q->type);
        int kind_len = strlen(kind) + 1;
        nla->nla_len = NLA_HDRLEN + (uint16_t)kind_len;
        nla->nla_type = TCA_KIND;
        memcpy((uint8_t *)(nla + 1), kind, (size_t)kind_len);

        resp->nlmsg_len += NLA_ALIGN(nla->nla_len);
        netlink_unicast(protocol, src_pid, buf, resp->nlmsg_len, 0);
        return 0;
    }

    /* Dump all qdiscs */
    int count = tc_get_qdisc_count();
    for (int i = 0; i < count; i++) {
        char devname[32];
        struct qdisc *q = tc_get_qdisc_by_index(i, devname, sizeof(devname));
        if (!q) continue;

        uint8_t buf[256];
        struct nlmsghdr *resp = (struct nlmsghdr *)buf;
        struct tcmsg *rtcm = (struct tcmsg *)(buf + NLMSG_HDRLEN);
        struct nlattr *nla = (struct nlattr *)(buf + NLMSG_HDRLEN + sizeof(struct tcmsg));

        memset(buf, 0, sizeof(buf));
        resp->nlmsg_len = NLMSG_HDRLEN + sizeof(struct tcmsg);
        resp->nlmsg_type = RTM_NEWQDISC;
        resp->nlmsg_flags = NLM_F_MULTI;
        resp->nlmsg_seq = nlh->nlmsg_seq;
        resp->nlmsg_pid = 0;

        rtcm->tcm_family = AF_UNSPEC;
        rtcm->tcm_ifindex = 1;  /* dummy ifindex — no real device binding yet */
        rtcm->tcm_handle = TC_H_MAKE(1, i + 1);
        rtcm->tcm_parent = TC_H_ROOT;
        rtcm->tcm_info = (uint32_t)q->type << 16;

        const char *kind = tc_kind_name(q->type);
        int kind_len = strlen(kind) + 1;
        nla->nla_len = NLA_HDRLEN + (uint16_t)kind_len;
        nla->nla_type = TCA_KIND;
        memcpy((uint8_t *)(nla + 1), kind, (size_t)kind_len);

        resp->nlmsg_len += NLA_ALIGN(nla->nla_len);
        netlink_unicast(protocol, src_pid, buf, resp->nlmsg_len, 0);
    }

    tc_send_done(protocol, src_pid, nlh->nlmsg_seq);
    return 0;
}

/* Handle RTM_NEWQDISC — create a new qdisc.
 * Parses TCA_KIND to determine qdisc type and calls tc_add_qdisc.
 * Uses the ifindex from tcmsg as the device name (devN). */
static int nl_tc_newqdisc(int protocol, const struct nlmsghdr *nlh,
                           const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)attr;
    (void)src_pid;

    if (nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(struct tcmsg))
        return -EINVAL;

    const struct tcmsg *tcm = (const struct tcmsg *)((const char *)nlh + NLMSG_HDRLEN);

    /* Parse TCA attributes following the tcmsg header */
    struct nlattr *tca[16];
    memset(tca, 0, sizeof(tca));
    int tca_payload = (int)nlh->nlmsg_len - (int)NLMSG_HDRLEN - (int)sizeof(struct tcmsg);
    if (tca_payload > 0) {
        nla_parse((const struct nlattr *)(tcm + 1), (size_t)tca_payload,
                  15, NULL, tca);
    }

    if (!tca[TCA_KIND])
        return -EINVAL;

    const char *kind;
    if (nla_get_string(tca[TCA_KIND], &kind) < 0)
        return -EINVAL;

    int qdisc_type = tc_kind_to_type(kind);
    if (qdisc_type < 0)
        return -EOPNOTSUPP;

    /* Build a device name from the ifindex.
     * In a full implementation, this would resolve the net_device. */
    char devname[32];
    snprintf(devname, sizeof(devname), "dev%u", tcm->tcm_ifindex);

    /* Check NLM_F_EXCL / NLM_F_CREATE flags. */
    int exists = (tc_get_qdisc(devname) != NULL);
    if (nlh->nlmsg_flags & NLM_F_EXCL) {
        if (exists)
            return -EEXIST;
    }
    if (!(nlh->nlmsg_flags & NLM_F_CREATE)) {
        if (!exists)
            return -ENOENT;
    }

    /* Delete existing if replacing */
    if (nlh->nlmsg_flags & NLM_F_REPLACE) {
        if (exists)
            tc_del_qdisc(devname);
    }

    int ret = tc_add_qdisc(devname, qdisc_type, NULL);
    if (ret < 0)
        return -EINVAL;

    return 0;
}

/* Handle RTM_DELQDISC — delete a qdisc.
 * Parses the tcmsg header to find the ifindex and removes the qdisc. */
static int nl_tc_delqdisc(int protocol, const struct nlmsghdr *nlh,
                           const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)attr;
    (void)src_pid;

    if (nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(struct tcmsg))
        return -EINVAL;

    const struct tcmsg *tcm = (const struct tcmsg *)((const char *)nlh + NLMSG_HDRLEN);

    char devname[32];
    snprintf(devname, sizeof(devname), "dev%u", tcm->tcm_ifindex);

    int ret = tc_del_qdisc(devname);
    if (ret < 0)
        return -ENOENT;

    return 0;
}

/* Handle RTM_GETTCLASS — dump traffic classes (stub).
 * In a full implementation, returns HTB class hierarchy for classful qdiscs. */
static int nl_tc_gettclass(int protocol, const struct nlmsghdr *nlh,
                            const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)nlh;
    (void)attr;
    (void)src_pid;

    /* No TC classes registered yet — just send DONE */
    tc_send_done(protocol, src_pid, nlh->nlmsg_seq);
    return 0;
}

/* Handle RTM_GETACTION — list TC actions (stub). */
static int nl_tc_getaction(int protocol, const struct nlmsghdr *nlh,
                            const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)nlh;
    (void)attr;
    (void)src_pid;

    tc_send_done(protocol, src_pid, nlh->nlmsg_seq);
    return 0;
}

/* Handle RTM_NEWACTION — add a TC action (stub). */
static int nl_tc_newaction(int protocol, const struct nlmsghdr *nlh,
                            const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)nlh;
    (void)attr;
    (void)src_pid;
    return 0;
}

/* Handle RTM_DELACTION — delete a TC action (stub). */
static int nl_tc_delaction(int protocol, const struct nlmsghdr *nlh,
                            const struct nlattr **attr, uint32_t src_pid)
{
    (void)protocol;
    (void)nlh;
    (void)attr;
    (void)src_pid;
    return 0;
}

/* ── Enhanced kernel message handler with dispatch ──────────────── */

/*
 * netlink_handle_kernel — Dispatch kernel-bound netlink messages.
 *
 * First tries registered handlers (via nl_dispatch_to_handlers), and
 * if no handler claims the message, falls back to built-in handling
 * for generic netlink controller commands and family ACK responses.
 */
static int netlink_handle_kernel(int protocol, const void *buf, int len,
                                  uint32_t src_pid)
{
    if (!buf || len < (int)sizeof(struct nlmsghdr))
        return -EINVAL;

    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;

    /* First try registered handlers */
    int ret = nl_dispatch_to_handlers(protocol, nlh, src_pid);
    if (ret != -ENOENT)
        return ret;

    /* ── Fallback: built-in handling ──────────────────────────────── */

    if (protocol == NETLINK_ROUTE) {
        kprintf("[netlink] RTNETLINK: unhandled type %u (seq=%u)\\n",
                nlh->nlmsg_type, nlh->nlmsg_seq);
        return 0;
    }

    /* Generic netlink: handle controller commands */
    if (nlh->nlmsg_type == GENL_ID_CTRL)
        return genl_handle_ctrl(buf, len, src_pid);

    /* Check for registered generic netlink families */
    int family_id = (int)nlh->nlmsg_type;
    if (family_id >= GENL_ID_CTRL &&
        family_id < GENL_ID_CTRL + GENL_MAX_FAMILIES) {
        if (nlh->nlmsg_flags & NLM_F_ACK) {
            struct {
                struct nlmsghdr hdr;
                struct nlmsgerr err;
            } __attribute__((packed)) ack;

            memset(&ack, 0, sizeof(ack));
            ack.hdr.nlmsg_len = sizeof(ack);
            ack.hdr.nlmsg_type = NLMSG_ERROR;
            ack.hdr.nlmsg_flags = 0;
            ack.hdr.nlmsg_seq = nlh->nlmsg_seq;
            ack.hdr.nlmsg_pid = 0;
            ack.err.error = 0;
            ack.err.msg = *nlh;

            netlink_unicast(protocol, src_pid, &ack, sizeof(ack), 0);
        }
        return 0;
    }

    return -EINVAL;
}

/* ── Initialise RTNETLINK handlers ───────────────────────────────── */

/* Register built-in RTNETLINK handlers.  Called from af_netlink_init. */
void __init netlink_rtnl_init(void)
{
    if (!nl_handlers_initialized) nl_handler_init();

    netlink_register_handler(NETLINK_ROUTE, RTM_GETROUTE,
                              nl_rt_getroute, "rtnl_getroute");
    netlink_register_handler(NETLINK_ROUTE, RTM_NEWROUTE,
                              nl_rt_newroute, "rtnl_newroute");
    netlink_register_handler(NETLINK_ROUTE, RTM_DELROUTE,
                              nl_rt_delroute, "rtnl_delroute");

    /* Register TC (Traffic Control) handlers */
    netlink_register_handler(NETLINK_ROUTE, RTM_GETQDISC,
                              nl_tc_getqdisc, "rtnl_getqdisc");
    netlink_register_handler(NETLINK_ROUTE, RTM_NEWQDISC,
                              nl_tc_newqdisc, "rtnl_newqdisc");
    netlink_register_handler(NETLINK_ROUTE, RTM_DELQDISC,
                              nl_tc_delqdisc, "rtnl_delqdisc");
    netlink_register_handler(NETLINK_ROUTE, RTM_GETTCLASS,
                              nl_tc_gettclass, "rtnl_gettclass");
    netlink_register_handler(NETLINK_ROUTE, RTM_GETACTION,
                              nl_tc_getaction, "rtnl_getaction");
    netlink_register_handler(NETLINK_ROUTE, RTM_NEWACTION,
                              nl_tc_newaction, "rtnl_newaction");
    netlink_register_handler(NETLINK_ROUTE, RTM_DELACTION,
                              nl_tc_delaction, "rtnl_delaction");

    kprintf("[OK] netlink: RTNETLINK handlers registered\\n");
}

/* ── Message delivery ──────────────────────────────────────────────── */

int netlink_unicast(int protocol, uint32_t dst_portid,
                     const void *data, int len, uint32_t src_pid) {
    if (!netlink_initialized || !data || len <= 0) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    /* Find socket with matching protocol and port ID */
    for (int i = 0; i < NETLINK_MAX_SOCKETS; i++) {
        struct netlink_sock *nl = &netlink_table[i];
        if (!nl->used || !nl->bound) continue;
        if (nl->protocol != protocol) continue;
        if (nl->portid != dst_portid) continue;

        /* Put message in receive buffer */
        uint8_t *copy_buf = (uint8_t *)kmalloc((size_t)len);
        if (!copy_buf) {
            spinlock_release(&netlink_lock);
            return -ENOMEM;
        }
        memcpy(copy_buf, data, (size_t)len);

        /* Update the nlmsg_pid to reflect actual sender */
        struct nlmsghdr *hdr = (struct nlmsghdr *)copy_buf;
        hdr->nlmsg_pid = src_pid;

        int ret = netlink_deliver_internal(nl, copy_buf, len);
        kfree(copy_buf);

        spinlock_release(&netlink_lock);
        return ret;
    }

    spinlock_release(&netlink_lock);
    return -EINVAL;
}

int netlink_broadcast(int protocol, uint32_t group_mask,
                       const void *data, int len, uint32_t src_pid) {
    if (!netlink_initialized || !data || len <= 0 || group_mask == 0) return -EINVAL;

    int deliveries = 0;

    spinlock_acquire(&netlink_lock);

    for (int i = 0; i < NETLINK_MAX_SOCKETS; i++) {
        struct netlink_sock *nl = &netlink_table[i];
        if (!nl->used || !nl->bound) continue;
        if (nl->protocol != protocol) continue;
        if (!(nl->groups & group_mask)) continue;

        /* Clone message and deliver */
        uint8_t *copy_buf = (uint8_t *)kmalloc((size_t)len);
        if (!copy_buf) continue;

        memcpy(copy_buf, data, (size_t)len);
        struct nlmsghdr *hdr = (struct nlmsghdr *)copy_buf;
        hdr->nlmsg_pid = src_pid;

        if (netlink_deliver_internal(nl, copy_buf, len) == 0) {
            deliveries++;
        }
        kfree(copy_buf);
    }

    spinlock_release(&netlink_lock);
    return deliveries;
}

/* ── Generic netlink family management ─────────────────────────────── */

int genl_register_family(const char *name, uint8_t version, uint32_t maxattr) {
    if (!netlink_initialized || !name) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    /* Check for duplicate name */
    for (int i = 0; i < GENL_MAX_FAMILIES; i++) {
        if (genl_families[i].registered &&
            strcmp(genl_families[i].name, name) == 0) {
            spinlock_release(&netlink_lock);
            return -EINVAL; /* EEXIST */
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < GENL_MAX_FAMILIES; i++) {
        if (!genl_families[i].registered) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&netlink_lock);
        return -EINVAL; /* ENOSPC */
    }

    int family_id = next_genl_family_id++;
    genl_families[slot].id = family_id;
    strncpy(genl_families[slot].name, name, GENL_NAMSIZ - 1);
    genl_families[slot].name[GENL_NAMSIZ - 1] = '\0';
    genl_families[slot].version = version;
    genl_families[slot].maxattr = maxattr;
    genl_families[slot].registered = 1;

    kprintf("[genl] registered family \"%s\" id=%d ver=%d\n",
            name, family_id, version);

    spinlock_release(&netlink_lock);
    return family_id;
}

int genl_unregister_family(int family_id) {
    if (!netlink_initialized) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    for (int i = 0; i < GENL_MAX_FAMILIES; i++) {
        if (genl_families[i].registered && genl_families[i].id == family_id) {
            genl_families[i].registered = 0;
            spinlock_release(&netlink_lock);
            return 0;
        }
    }

    spinlock_release(&netlink_lock);
    return -EINVAL;
}

int genl_find_family(const char *name) {
    if (!netlink_initialized || !name) return -EINVAL;

    spinlock_acquire(&netlink_lock);

    for (int i = 0; i < GENL_MAX_FAMILIES; i++) {
        if (genl_families[i].registered &&
            strcmp(genl_families[i].name, name) == 0) {
            int id = genl_families[i].id;
            spinlock_release(&netlink_lock);
            return id;
        }
    }

    spinlock_release(&netlink_lock);
    return -EINVAL;
}

/* Handle a generic netlink controller command (family discovery) */
static int genl_handle_ctrl(const void *buf, int len, uint32_t src_pid) {
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
    if ((unsigned int)len < sizeof(struct nlmsghdr) + GENL_HDRLEN) return -EINVAL;

    const struct genlmsghdr *genlh =
        (const struct genlmsghdr *)((const char *)nlh + NLMSG_HDRLEN);

    switch (genlh->cmd) {
    case CTRL_CMD_GETFAMILY: {
        /* Query family info — respond with family details */
        spinlock_acquire(&netlink_lock);

        for (int i = 0; i < GENL_MAX_FAMILIES; i++) {
            if (!genl_families[i].registered) continue;

            /* Build response message */
            char resp_buf[512];
            struct nlmsghdr *resp = (struct nlmsghdr *)resp_buf;

            memset(resp, 0, sizeof(resp_buf));

            /* Encode family info as attributes */
            uint8_t *attr_ptr = (uint8_t *)resp + NLMSG_HDRLEN + GENL_HDRLEN;
            int attr_len = 0;

            /* Family ID (uint16_t) */
            struct nlattr *id_attr = (struct nlattr *)attr_ptr;
            id_attr->nla_len = NLA_HDRLEN + sizeof(uint16_t);
            id_attr->nla_type = CTRL_ATTR_FAMILY_ID;
            *(uint16_t *)NLA_DATA(id_attr) = (uint16_t)genl_families[i].id;
            attr_len += NLA_ALIGN(id_attr->nla_len);
            attr_ptr = (uint8_t *)resp + NLMSG_HDRLEN + GENL_HDRLEN + attr_len;

            /* Family name (string) */
            struct nlattr *name_attr = (struct nlattr *)attr_ptr;
            int name_len = strlen(genl_families[i].name) + 1;
            name_attr->nla_len = NLA_HDRLEN + name_len;
            name_attr->nla_type = CTRL_ATTR_FAMILY_NAME;
            memcpy(NLA_DATA(name_attr), genl_families[i].name, name_len);
            attr_len += NLA_ALIGN(name_attr->nla_len);
            attr_ptr = (uint8_t *)resp + NLMSG_HDRLEN + GENL_HDRLEN + attr_len;

            /* Version */
            struct nlattr *ver_attr = (struct nlattr *)attr_ptr;
            ver_attr->nla_len = NLA_HDRLEN + sizeof(uint32_t);
            ver_attr->nla_type = CTRL_ATTR_VERSION;
            *(uint32_t *)NLA_DATA(ver_attr) = genl_families[i].version;
            attr_len += NLA_ALIGN(ver_attr->nla_len);
            attr_ptr = (uint8_t *)resp + NLMSG_HDRLEN + GENL_HDRLEN + attr_len;

            /* Max attribute type */
            struct nlattr *max_attr = (struct nlattr *)attr_ptr;
            max_attr->nla_len = NLA_HDRLEN + sizeof(uint32_t);
            max_attr->nla_type = CTRL_ATTR_MAXATTR;
            *(uint32_t *)NLA_DATA(max_attr) = genl_families[i].maxattr;
            attr_len += NLA_ALIGN(max_attr->nla_len);

            int payload_len = GENL_HDRLEN + attr_len;
            resp->nlmsg_len = NLMSG_LENGTH(payload_len);
            resp->nlmsg_type = GENL_ID_CTRL;
            resp->nlmsg_flags = NLM_F_MULTI;
            resp->nlmsg_seq = nlh->nlmsg_seq;
            resp->nlmsg_pid = 0; /* From kernel */

            /* Fill in genl header */
            struct genlmsghdr *rgenl = (struct genlmsghdr *)((char *)resp + NLMSG_HDRLEN);
            rgenl->cmd = CTRL_CMD_NEWFAMILY;
            rgenl->version = 1;
            rgenl->reserved = 0;

            spinlock_release(&netlink_lock);

            netlink_unicast(NETLINK_GENERIC, src_pid, resp,
                           (int)resp->nlmsg_len, 0);

            spinlock_acquire(&netlink_lock);
        }

        /* Send DONE message to terminate the multipart sequence */
        {
            char done_buf[sizeof(struct nlmsghdr)];
            struct nlmsghdr *done = (struct nlmsghdr *)done_buf;
            memset(done, 0, sizeof(done_buf));
            done->nlmsg_len = sizeof(struct nlmsghdr);
            done->nlmsg_type = NLMSG_DONE;
            done->nlmsg_flags = 0;
            done->nlmsg_seq = nlh->nlmsg_seq;
            done->nlmsg_pid = 0;

            spinlock_release(&netlink_lock);

            netlink_unicast(NETLINK_GENERIC, src_pid, done,
                           (int)done->nlmsg_len, 0);

            spinlock_acquire(&netlink_lock);
        }

        spinlock_release(&netlink_lock);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/* ── Helper queries ────────────────────────────────────────────────── */

int netlink_is_valid_fd(int fd) {
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return 0;
    return netlink_table[slot].used ? 1 : 0;
}

int netlink_get_protocol(int fd) {
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -EINVAL;
    if (!netlink_table[slot].used) return -EINVAL;
    return netlink_table[slot].protocol;
}
#include "module.h"
module_init(af_netlink_init);

/* ── Implement: netlink_register ──────────────────────── */
int netlink_register(int proto)
{
    (void)proto;
    kprintf("[netlink] netlink_register: protocol %d registered\n", proto);
    return 0;
}
