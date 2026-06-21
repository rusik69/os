#ifndef NETLINK_H
#define NETLINK_H

#include "types.h"

/*
 * ── AF_NETLINK — Kernel-Userspace Communication Sockets ────────
 *
 * Provides AF_NETLINK / PF_NETLINK (domain=16) for bidirectional
 * communication between kernel and userspace processes.  Each
 * netlink socket is identified by a port ID and bound to a netlink
 * protocol family (e.g. NETLINK_GENERIC, NETLINK_ROUTE).
 *
 * Reference: netlink(7) man page, Linux include/linux/netlink.h
 *
 * Socket address structure (struct sockaddr_nl):
 *   nl_family   = AF_NETLINK (16)
 *   nl_pad      = 0 (reserved)
 *   nl_pid      = Port ID (0 = kernel, otherwise userspace)
 *   nl_groups   = Multicast group membership bitmask
 */

/* ── Constants ──────────────────────────────────────────────────── */

#define AF_NETLINK          16
#define PF_NETLINK          16

/* Netlink protocol families */
#define NETLINK_ROUTE       0       /* Routing/device hook */
#define NETLINK_FIREWALL    3       /* Firewall hooks */
#define NETLINK_INET_DIAG   4       /* Socket monitoring */
#define NETLINK_GENERIC     16      /* Generic netlink (family multiplexer) */
#define NETLINK_KOBJECT_UEVENT   15  /* Kernel uevent notification */
#define NETLINK_AUDIT       9       /* Audit subsystem */
#define NETLINK_SELINUX     7       /* SELinux events */
#define NETLINK_CRYPTO      21      /* Crypto API */

/* Maximum number of netlink sockets */
#define NETLINK_MAX_SOCKETS 16

/* Maximum payload size per message */
#define NETLINK_MAX_PAYLOAD 65536

/* Max multicast groups */
#define NETLINK_MAX_GROUPS  32

/* ── Netlink message header (struct nlmsghdr) ───────────────────── */

struct nlmsghdr {
    uint32_t nlmsg_len;     /* Length of message including header */
    uint16_t nlmsg_type;    /* Message type (subsystem-specific) */
    uint16_t nlmsg_flags;   /* Flags */
    uint32_t nlmsg_seq;     /* Sequence number */
    uint32_t nlmsg_pid;     /* Sending port ID */
} __attribute__((packed));

/* Message flags */
#define NLM_F_REQUEST       1       /* Request message */
#define NLM_F_MULTI         2       /* Multipart message (terminated by NLMSG_DONE) */
#define NLM_F_ACK           4       /* Reply with ack */
#define NLM_F_ECHO          8       /* Echo request */

/* Additional flags for GET requests */
#define NLM_F_ROOT          0x100   /* Return complete table */
#define NLM_F_MATCH         0x200   /* Return only matching entries */
#define NLM_F_ATOMIC        0x400   /* Atomic operation */
#define NLM_F_DUMP          (NLM_F_ROOT|NLM_F_MATCH)

/* Additional flags for NEW requests */
#define NLM_F_REPLACE       0x100   /* Replace existing */
#define NLM_F_EXCL          0x200   /* Don't replace if exists */
#define NLM_F_CREATE        0x400   /* Create if doesn't exist */
#define NLM_F_APPEND        0x800   /* Add to end of list */

/* Standard message types */
#define NLMSG_NOOP          1       /* No operation */
#define NLMSG_ERROR         2       /* Error */
#define NLMSG_DONE          3       /* End of multipart message */
#define NLMSG_OVERRUN       4       /* Data lost (overrun) */
#define NLMSG_MIN_TYPE      0x10    /* Start of subsystem-specific types */

/* Netlink socket address (struct sockaddr_nl) */
struct sockaddr_nl {
    uint16_t nl_family;     /* AF_NETLINK (16) */
    uint16_t nl_pad;        /* Reserved (zero) */
    uint32_t nl_pid;        /* Port ID */
    uint32_t nl_groups;     /* Multicast groups mask */
} __attribute__((packed));

/* ── Netlink message helper macros ───────────────────────────────── */

/* Length of a netlink message header (aligned) */
#define NLMSG_HDRLEN        ((int)sizeof(struct nlmsghdr))

/* Align length to NLMSG_ALIGNTO (4 bytes) */
#define NLMSG_ALIGNTO       4
#define NLMSG_ALIGN(len)    (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))

/* Length of a complete message (header + payload, aligned) */
#define NLMSG_LENGTH(payload)   (NLMSG_HDRLEN + NLMSG_ALIGN(payload))

/* Size of a message payload given total message length */
#define NLMSG_SPACE(payload)    NLMSG_ALIGN(NLMSG_LENGTH(payload))

/* Pointer to message payload (after header) */
#define NLMSG_DATA(nlh)     ((void *)(((char *)nlh) + NLMSG_HDRLEN))

/* Next message in a multipart sequence */
#define NLMSG_NEXT(nlh, len)    do {                \
    (len) -= NLMSG_ALIGN((nlh)->nlmsg_len);         \
    (nlh)  = (struct nlmsghdr *)((char *)(nlh) +    \
              NLMSG_ALIGN((nlh)->nlmsg_len));       \
} while (0)

/* Check if message is valid (minimum length check) */
#define NLMSG_OK(nlh, len)  ((len) >= (int)sizeof(struct nlmsghdr) && \
                             (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && \
                             (nlh)->nlmsg_len <= (len))

/* Payload of a netlink message */
#define NLMSG_PAYLOAD(nlh, hdrlen)  ((nlh)->nlmsg_len - NLMSG_HDRLEN)

/* ── Netlink error message (embedded in NLMSG_ERROR) ───────────── */

struct nlmsgerr {
    int         error;          /* Negative errno value */
    struct nlmsghdr msg;        /* Message that caused the error */
} __attribute__((packed));

/* ── Generic netlink family ─────────────────────────────────────── */

/* Generic netlink header */
struct genlmsghdr {
    uint8_t  cmd;           /* Command */
    uint8_t  version;       /* Version */
    uint16_t reserved;      /* Reserved */
} __attribute__((packed));

#define GENL_HDRLEN         ((int)sizeof(struct genlmsghdr))

/* Generic netlink commands */
#define GENL_ID_CTRL        0x10    /* Controller (family registration) */
#define CTRL_CMD_UNSPEC     0
#define CTRL_CMD_NEWFAMILY  1       /* Notify of new family */
#define CTRL_CMD_DELFAMILY  2       /* Notify of family removal */
#define CTRL_CMD_GETFAMILY  3       /* Query family info */
#define CTRL_CMD_NEWOPS     4
#define CTRL_CMD_DELOPS     5
#define CTRL_CMD_GETOPS     6

/* Generic netlink controller attributes */
#define CTRL_ATTR_UNSPEC    0
#define CTRL_ATTR_FAMILY_ID 1       /* uint16_t */
#define CTRL_ATTR_FAMILY_NAME 2     /* string */
#define CTRL_ATTR_VERSION   3       /* uint32_t */
#define CTRL_ATTR_HDRSIZE   4       /* uint32_t */
#define CTRL_ATTR_MAXATTR   5       /* uint32_t */
#define CTRL_ATTR_OPS       6       /* nested */
#define CTRL_ATTR_MCAST_GROUPS 7    /* nested */

/* ── Netlink attribute (TLV: type-len-value) ────────────────────── */

struct nlattr {
    uint16_t nla_len;       /* Length of attribute (including header) */
    uint16_t nla_type;      /* Attribute type (subsystem-specific) */
} __attribute__((packed));

#define NLA_HDRLEN          ((int)sizeof(struct nlattr))
#define NLA_ALIGNTO         4
#define NLA_ALIGN(len)      (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_DATA(nla)       ((void *)(((char *)nla) + NLA_HDRLEN))
#define NLA_PAYLOAD(nla)    ((nla)->nla_len - NLA_HDRLEN)
#define NLA_NEXT(nla, len)  do {                                    \
    (len) -= NLA_ALIGN((nla)->nla_len);                              \
    (nla)  = (struct nlattr *)((char *)(nla) + NLA_ALIGN((nla)->nla_len)); \
} while (0)
#define NLA_OK(nla, len)    ((len) >= (int)sizeof(struct nlattr) && \
                             (nla)->nla_len >= sizeof(struct nlattr) && \
                             (nla)->nla_len <= (len))

/* ── Registered generic netlink family ──────────────────────────── */

#define GENL_MAX_FAMILIES   32
#define GENL_NAMSIZ         16

struct genl_family {
    int         id;             /* Family ID (assigned at registration) */
    char        name[GENL_NAMSIZ]; /* Family name */
    uint8_t     version;        /* Protocol version */
    uint32_t    maxattr;        /* Maximum attribute type */
    int         registered;     /* 1 if in use */
};

/* ── Netlink socket state ────────────────────────────────────────── */

struct netlink_sock {
    int         used;           /* 1 = slot in use */
    int         fd;             /* Associated socket fd */
    int         protocol;       /* NETLINK_ROUTE, NETLINK_GENERIC, etc. */
    uint32_t    portid;         /* Local port ID (0 = kernel assigned) */
    uint32_t    groups;         /* Multicast group membership */
    int         bound;          /* 1 = bound */

    /* Incoming message queue (ring buffer) */
    struct nlmsghdr *recv_buf;  /* Dynamically allocated receive buffer */
    int         recv_size;      /* Total buffer size */
    int         recv_used;      /* Bytes used */
    int         recv_pos;       /* Read position */

    /* Statistics */
    uint64_t    msgs_recv;
    uint64_t    msgs_sent;
    uint64_t    msgs_dropped;
};

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the AF_NETLINK subsystem */
void af_netlink_init(void);

/* Create a netlink socket. Returns fd on success, -1 on error. */
int  netlink_create(int fd, int protocol);

/* Bind netlink socket to a port ID. Returns 0 on success, -1 on error. */
int  netlink_bind(int fd, const struct sockaddr_nl *addr);

/* Send a netlink message (kernel→userspace or userspace→kernel).
 * Returns bytes sent on success, -1 on error. */
int  netlink_send(int fd, const void *buf, int len);

/* Receive a netlink message. Returns bytes received on success, -1 on error. */
int  netlink_recv(int fd, void *buf, int max_len);

/* Close a netlink socket. */
void netlink_close(int fd);

/* Send a message to a specific port ID (cross-socket delivery).
 * Used by kernel subsystems to deliver events to userspace.
 * Returns 0 on success, -1 if no socket with that portid. */
int  netlink_unicast(int protocol, uint32_t dst_portid,
                     const void *data, int len, uint32_t src_pid);

/* Broadcast a message to all sockets in a multicast group.
 * Returns number of deliveries. */
int  netlink_broadcast(int protocol, uint32_t group_mask,
                       const void *data, int len, uint32_t src_pid);

/* Register a generic netlink family.
 * Returns family ID on success, -1 on error. */
int  genl_register_family(const char *name, uint8_t version, uint32_t maxattr);

/* Unregister a generic netlink family. */
int  genl_unregister_family(int family_id);

/* Look up a generic netlink family by name. Returns ID or -1. */
int  genl_find_family(const char *name);

/* Check if an fd is a netlink socket. */
int  netlink_is_valid_fd(int fd);

/* Get the protocol for an fd. */
int  netlink_get_protocol(int fd);

/* ── RTNETLINK (NETLINK_ROUTE) ───────────────────────────────────── */
#define RTM_NEWROUTE    24
#define RTM_DELROUTE    25
#define RTM_GETROUTE    26

struct rtmsg {
    uint8_t  rtm_family;
    uint8_t  rtm_dst_len;
    uint8_t  rtm_src_len;
    uint8_t  rtm_tos;
    uint8_t  rtm_table;
    uint8_t  rtm_protocol;
    uint8_t  rtm_scope;
    uint8_t  rtm_type;
    uint32_t rtm_flags;
};

/* rtattr is the same as nlattr */
#define RTM_RTA(r)       ((struct nlattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct rtmsg))))
#define RTM_PAYLOAD(n)   NLMSG_PAYLOAD(n, sizeof(struct rtmsg))

/* RTA attribute constants */
#define RTA_DST          1
#define RTA_GATEWAY      5
#define RTA_OIF          4
#define RTA_PREFSRC      7

/* RTA attribute macros (using nlattr as rtattr) */
#define RTA_LENGTH(len)  (NLA_HDRLEN + (len))
#define RTA_DATA(rta)    NLA_DATA(rta)
#define RTA_NEXT(rta, remaining) NLA_NEXT(rta, remaining)
#define RTA_OK(rta, remaining)   NLA_OK(rta, remaining)

#endif /* NETLINK_H */
