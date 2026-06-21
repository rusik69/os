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
    if (slot < 0 || slot >= NETLINK_MAX_SOCKETS) return -1;
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
    return -1;
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
}

/* ── Socket lifecycle ──────────────────────────────────────────────── */

int netlink_create(int fd, int protocol) {
    if (!netlink_initialized) return -1;
    if (protocol < 0 || protocol > 255) return -1;

    spinlock_acquire(&netlink_lock);
    int slot = netlink_alloc_slot();
    if (slot < 0) {
        spinlock_release(&netlink_lock);
        return -1;
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
        return -1;
    }
    memset(nl->recv_buf, 0, NETLINK_RECV_BUF);
    nl->recv_size = NETLINK_RECV_BUF;
    nl->recv_used = 0;
    nl->recv_pos = 0;

    spinlock_release(&netlink_lock);
    return 0;
}

int netlink_bind(int fd, const struct sockaddr_nl *addr) {
    if (!netlink_initialized || !addr) return -1;
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -1;

    spinlock_acquire(&netlink_lock);

    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used) {
        spinlock_release(&netlink_lock);
        return -1;
    }

    /* If already bound, unbind first */
    if (nl->bound) {
        spinlock_release(&netlink_lock);
        return -1; /* EINVAL: already bound */
    }

    uint32_t new_pid = addr->nl_pid;

    /* Validate groups mask */
    if (addr->nl_groups & ~((1ULL << NETLINK_MAX_GROUPS) - 1)) {
        spinlock_release(&netlink_lock);
        return -1; /* EINVAL */
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
            return -1; /* EADDRINUSE */
        }
    }

    nl->portid = new_pid;
    nl->groups = addr->nl_groups;
    nl->bound = 1;

    spinlock_release(&netlink_lock);
    return 0;
}

int netlink_send(int fd, const void *buf, int len) {
    if (!netlink_initialized || !buf || len < (int)sizeof(struct nlmsghdr)) return -1;
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -1;

    spinlock_acquire(&netlink_lock);

    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used || !nl->bound) {
        spinlock_release(&netlink_lock);
        return -1;
    }

    const struct nlmsghdr *hdr = (const struct nlmsghdr *)buf;
    if ((unsigned int)len < hdr->nlmsg_len || hdr->nlmsg_len < sizeof(struct nlmsghdr)) {
        spinlock_release(&netlink_lock);
        return -1; /* EINVAL */
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

    return -1;
}

int netlink_recv(int fd, void *buf, int max_len) {
    if (!netlink_initialized || !buf || max_len <= 0) return -1;
    int slot = netlink_fd_to_slot(fd);
    if (slot < 0) return -1;

    spinlock_acquire(&netlink_lock);

    struct netlink_sock *nl = &netlink_table[slot];
    if (!nl->used || !nl->bound) {
        spinlock_release(&netlink_lock);
        return -1;
    }

    /* No data available */
    if (nl->recv_used <= 0) {
        spinlock_release(&netlink_lock);
        return -1; /* EAGAIN */
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
    if (!nl || !nl->used) return -1;

    /* Check if message fits in the buffer */
    if (nl->recv_used + len > nl->recv_size) {
        nl->msgs_dropped++;
        return -1; /* ENOSPC */
    }

    memcpy((uint8_t *)nl->recv_buf + nl->recv_used, data, len);
    nl->recv_used += len;

    return 0;
}

/* Handle a message destined for the kernel (generic netlink commands, etc.) */
static int netlink_handle_kernel(int protocol, const void *buf, int len,
                                  uint32_t src_pid) {
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;

    /* NETLINK_ROUTE: route table management */
    if (protocol == NETLINK_ROUTE) {
        switch (nlh->nlmsg_type) {
        case RTM_NEWROUTE: {
            /* Add a route entry */
            if ((unsigned int)len < NLMSG_LENGTH(sizeof(struct rtmsg)))
                return -1;
            const struct rtmsg *rtm = (const struct rtmsg *)NLMSG_DATA(nlh);
            struct rtattr *rta = RTM_RTA(rtm);
            int rta_len = RTM_PAYLOAD(nlh);

            uint32_t dst = 0, gw = 0, mask = 0;
            int ifindex = 0;

            while (RTA_OK(rta, rta_len)) {
                switch (rta->rta_type) {
                case RTA_DST:
                    if (rta->rta_len >= RTA_LENGTH(4))
                        memcpy(&dst, RTA_DATA(rta), 4);
                    break;
                case RTA_GATEWAY:
                    if (rta->rta_len >= RTA_LENGTH(4))
                        memcpy(&gw, RTA_DATA(rta), 4);
                    break;
                case RTA_OIF:
                    if (rta->rta_len >= RTA_LENGTH(4))
                        ifindex = *(int *)RTA_DATA(rta);
                    break;
                case RTA_PREFSRC:
                    /* Preferred source — skip for now */
                    break;
                }
                rta = RTA_NEXT(rta, rta_len);
            }

            /* Add to the routing table */
            dst = ntohl(dst);
            gw = ntohl(gw);
            if (rtm->rtm_dst_len > 0)
                mask = htonl(0xFFFFFFFF << (32 - rtm->rtm_dst_len));
            else
                mask = 0;

            /* Update routing table */
            int rt_idx = -1;
            for (int i = 0; i < RT_MAX_ENTRIES; i++) {
                if (!rt_table[i].in_use) {
                    rt_idx = i;
                    break;
                }
            }
            if (rt_idx >= 0) {
                rt_table[rt_idx].in_use = 1;
                rt_table[rt_idx].dst = dst;
                rt_table[rt_idx].gateway = gw;
                rt_table[rt_idx].mask = mask;
                rt_table[rt_idx].ifindex = ifindex;
                rt_table[rt_idx].metric = rtm->rtm_priority;
                if (rt_num_entries < rt_idx + 1)
                    rt_num_entries = rt_idx + 1;
                kprintf("[RTNL] Added route: dst=%d.%d.%d.%d/%d gw=%d.%d.%d.%d\n",
                        (int)((dst >> 24) & 0xFF), (int)((dst >> 16) & 0xFF),
                        (int)((dst >> 8) & 0xFF), (int)(dst & 0xFF),
                        rtm->rtm_dst_len,
                        (int)((gw >> 24) & 0xFF), (int)((gw >> 16) & 0xFF),
                        (int)((gw >> 8) & 0xFF), (int)(gw & 0xFF));
            }

            /* Acknowledge */
            if (nlh->nlmsg_flags & NLM_F_ACK) {
                struct {
                    struct nlmsghdr hdr;
                    struct nlmsgerr err;
                } __attribute__((packed)) ack;
                memset(&ack, 0, sizeof(ack));
                ack.hdr.nlmsg_len = sizeof(ack);
                ack.hdr.nlmsg_type = NLMSG_ERROR;
                ack.hdr.nlmsg_seq = nlh->nlmsg_seq;
                ack.hdr.nlmsg_pid = 0;
                ack.err.error = 0;
                ack.err.msg = *nlh;
                netlink_unicast(protocol, src_pid, &ack, sizeof(ack), 0);
            }
            return 0;
        }

        case RTM_DELROUTE: {
            /* Delete a route entry */
            if ((unsigned int)len < NLMSG_LENGTH(sizeof(struct rtmsg)))
                return -1;
            const struct rtmsg *rtm = (const struct rtmsg *)NLMSG_DATA(nlh);
            struct rtattr *rta = RTM_RTA(rtm);
            int rta_len = RTM_PAYLOAD(nlh);

            uint32_t dst = 0;

            while (RTA_OK(rta, rta_len)) {
                if (rta->rta_type == RTA_DST && rta->rta_len >= RTA_LENGTH(4))
                    memcpy(&dst, RTA_DATA(rta), 4);
                rta = RTA_NEXT(rta, rta_len);
            }

            dst = ntohl(dst);
            for (int i = 0; i < RT_MAX_ENTRIES; i++) {
                if (rt_table[i].in_use && rt_table[i].dst == dst) {
                    rt_table[i].in_use = 0;
                    kprintf("[RTNL] Deleted route: dst=%d.%d.%d.%d\n",
                            (int)((dst >> 24) & 0xFF), (int)((dst >> 16) & 0xFF),
                            (int)((dst >> 8) & 0xFF), (int)(dst & 0xFF));
                    break;
                }
            }

            if (nlh->nlmsg_flags & NLM_F_ACK) {
                struct {
                    struct nlmsghdr hdr;
                    struct nlmsgerr err;
                } __attribute__((packed)) ack;
                memset(&ack, 0, sizeof(ack));
                ack.hdr.nlmsg_len = sizeof(ack);
                ack.hdr.nlmsg_type = NLMSG_ERROR;
                ack.hdr.nlmsg_seq = nlh->nlmsg_seq;
                ack.hdr.nlmsg_pid = 0;
                ack.err.error = 0;
                ack.err.msg = *nlh;
                netlink_unicast(protocol, src_pid, &ack, sizeof(ack), 0);
            }
            return 0;
        }

        case RTM_GETROUTE: {
            /* Dump route table */
            int entries = 0;
            for (int i = 0; i < RT_MAX_ENTRIES; i++) {
                if (!rt_table[i].in_use) continue;

                uint8_t resp[256];
                struct nlmsghdr *resp_nlh = (struct nlmsghdr *)resp;
                struct rtmsg *resp_rtm = (struct rtmsg *)(resp + NLMSG_HDRLEN);

                memset(resp, 0, sizeof(resp));
                resp_nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
                resp_nlh->nlmsg_type = RTM_NEWROUTE;
                resp_nlh->nlmsg_flags = NLM_F_MULTI;
                resp_nlh->nlmsg_seq = nlh->nlmsg_seq;
                resp_nlh->nlmsg_pid = 0;

                resp_rtm->rtm_family = AF_INET;
                resp_rtm->rtm_dst_len = (rt_table[i].mask) ?
                    32 - __builtin_ctz(ntohl(rt_table[i].mask)) : 0;
                resp_rtm->rtm_table = RT_TABLE_MAIN;
                resp_rtm->rtm_protocol = RTPROT_BOOT;
                resp_rtm->rtm_scope = RT_SCOPE_UNIVERSE;
                resp_rtm->rtm_type = RTN_UNICAST;

                struct rtattr *rta = (struct rtattr *)(resp + NLMSG_HDRLEN + sizeof(struct rtmsg));
                if (rt_table[i].dst) {
                    uint32_t net_dst = htonl(rt_table[i].dst);
                    rta->rta_len = RTA_LENGTH(4);
                    rta->rta_type = RTA_DST;
                    memcpy(RTA_DATA(rta), &net_dst, 4);
                    rta = (struct rtattr *)((char *)rta + RTA_ALIGN(rta->rta_len));
                    resp_nlh->nlmsg_len += RTA_ALIGN(RTA_LENGTH(4));
                }
                if (rt_table[i].gateway) {
                    uint32_t net_gw = htonl(rt_table[i].gateway);
                    rta->rta_len = RTA_LENGTH(4);
                    rta->rta_type = RTA_GATEWAY;
                    memcpy(RTA_DATA(rta), &net_gw, 4);
                    resp_nlh->nlmsg_len += RTA_ALIGN(RTA_LENGTH(4));
                }

                netlink_unicast(protocol, src_pid, resp,
                               (int)resp_nlh->nlmsg_len, 0);
                entries++;
            }

            /* Send DONE */
            {
                uint8_t done[sizeof(struct nlmsghdr)];
                struct nlmsghdr *done_nlh = (struct nlmsghdr *)done;
                memset(done, 0, sizeof(done));
                done_nlh->nlmsg_len = sizeof(struct nlmsghdr);
                done_nlh->nlmsg_type = NLMSG_DONE;
                done_nlh->nlmsg_seq = nlh->nlmsg_seq;
                done_nlh->nlmsg_pid = 0;
                netlink_unicast(protocol, src_pid, done,
                               (int)done_nlh->nlmsg_len, 0);
            }

            kprintf("[RTNL] Dumped %d route entries\n", entries);
            return 0;
        }

        default:
            return -1;
        }
    }

    /* Only process generic netlink messages for now */
    if (nlh->nlmsg_type == GENL_ID_CTRL) {
        return genl_handle_ctrl(buf, len, src_pid);
    }

    /* Check if there's a registered family for this message type */
    int family_id = (int)nlh->nlmsg_type;
    if (family_id >= GENL_ID_CTRL && family_id < GENL_ID_CTRL + GENL_MAX_FAMILIES) {
        /* Message for a registered generic netlink family.
         * For now, just acknowledge. Future: dispatch to family handler. */
        if (nlh->nlmsg_flags & NLM_F_ACK) {
            /* Send ACK back to sender */
            struct {
                struct nlmsghdr hdr;
                struct nlmsgerr err;
            } __attribute__((packed)) ack;

            memset(&ack, 0, sizeof(ack));
            ack.hdr.nlmsg_len = sizeof(ack);
            ack.hdr.nlmsg_type = NLMSG_ERROR;
            ack.hdr.nlmsg_flags = 0;
            ack.hdr.nlmsg_seq = nlh->nlmsg_seq;
            ack.hdr.nlmsg_pid = 0; /* From kernel */
            ack.err.error = 0;     /* Success */
            ack.err.msg = *nlh;

            netlink_unicast(protocol, src_pid, &ack, sizeof(ack), 0);
        }
        return 0;
    }

    return -1;
}

/* ── Message delivery ──────────────────────────────────────────────── */

int netlink_unicast(int protocol, uint32_t dst_portid,
                     const void *data, int len, uint32_t src_pid) {
    if (!netlink_initialized || !data || len <= 0) return -1;

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
            return -1;
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
    return -1;
}

int netlink_broadcast(int protocol, uint32_t group_mask,
                       const void *data, int len, uint32_t src_pid) {
    if (!netlink_initialized || !data || len <= 0 || group_mask == 0) return -1;

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
    if (!netlink_initialized || !name) return -1;

    spinlock_acquire(&netlink_lock);

    /* Check for duplicate name */
    for (int i = 0; i < GENL_MAX_FAMILIES; i++) {
        if (genl_families[i].registered &&
            strcmp(genl_families[i].name, name) == 0) {
            spinlock_release(&netlink_lock);
            return -1; /* EEXIST */
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
        return -1; /* ENOSPC */
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
    if (!netlink_initialized) return -1;

    spinlock_acquire(&netlink_lock);

    for (int i = 0; i < GENL_MAX_FAMILIES; i++) {
        if (genl_families[i].registered && genl_families[i].id == family_id) {
            genl_families[i].registered = 0;
            spinlock_release(&netlink_lock);
            return 0;
        }
    }

    spinlock_release(&netlink_lock);
    return -1;
}

int genl_find_family(const char *name) {
    if (!netlink_initialized || !name) return -1;

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
    return -1;
}

/* Handle a generic netlink controller command (family discovery) */
static int genl_handle_ctrl(const void *buf, int len, uint32_t src_pid) {
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
    if ((unsigned int)len < sizeof(struct nlmsghdr) + GENL_HDRLEN) return -1;

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
        return -1;
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
    if (slot < 0) return -1;
    if (!netlink_table[slot].used) return -1;
    return netlink_table[slot].protocol;
}
#include "module.h"
module_init(af_netlink_init);

/* ── Stub: netlink_register ─────────────────────────────── */
int netlink_register(int proto)
{
    (void)proto;
    kprintf("[netlink] netlink_register: not yet implemented\n");
    return -ENOSYS;
}
