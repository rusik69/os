/*
 * can.h — CAN bus (SocketCAN) protocol family definitions (Item 352)
 *
 * Implements AF_CAN (domain 29) with RAW protocol for sending and
 * receiving CAN bus frames.  Follows Linux SocketCAN conventions.
 *
 * Reference: Linux Documentation/networking/can.rst, linux/can.h
 */

#ifndef CAN_H
#define CAN_H

#include "types.h"

/* ── Protocol family constants ──────────────────────────────────── */

#define AF_CAN              29
#define PF_CAN              29

/* ── CAN protocol types ─────────────────────────────────────────── */

#define CAN_RAW             1       /* RAW protocol */
#define CAN_BCM             2       /* Broadcast Manager (future) */
#define CAN_ISOTP           3       /* ISO 15765-2 (future) */

/* ── CAN frame format ───────────────────────────────────────────── */

/*
 * CAN Identifier flags (stored in can_id)
 * These match Linux <linux/can.h> for compatibility.
 */
#define CAN_EFF_FLAG        0x80000000U  /* Extended frame format */
#define CAN_RTR_FLAG        0x40000000U  /* Remote transmission request */
#define CAN_ERR_FLAG        0x20000000U  /* Error frame */

/* Valid bits in CAN ID (standard: 11-bit, extended: 29-bit) */
#define CAN_SFF_MASK        0x000007FFU  /* Standard frame format (11 bit) */
#define CAN_EFF_MASK        0x1FFFFFFFU  /* Extended frame format (29 bit) */
#define CAN_ERR_MASK        0x1FFFFFFFU  /* Omit EFF, RTR, ERR flags */

/* Maximum data length */
#define CAN_MAX_DLC         8
#define CAN_MAX_DLEN        8

/* ── CAN frame structure (matching Linux struct can_frame) ──────── */

struct can_frame {
    uint32_t can_id;            /* 32-bit CAN_ID + EFF/RTR/ERR flags */
    uint8_t  can_dlc;           /* data length code (0..8) */
    uint8_t  __pad;             /* padding */
    uint8_t  __res0;            /* reserved / padding */
    uint8_t  __res1;            /* reserved / padding */
    uint8_t  data[CAN_MAX_DLEN] __attribute__((aligned(8)));
} __attribute__((packed));

_Static_assert(sizeof(struct can_frame) == 16,
               "can_frame must be 16 bytes (Linux compatible)");

/* ── CAN socket address ─────────────────────────────────────────── */

struct can_addr {
    uint32_t can_ifindex;       /* network interface index (0 = any) */
    uint32_t can_tx_id;         /* TX identifier override (0 = use frame can_id) */
    uint32_t can_rx_id_mask;    /* RX filter: match mask (0 = all) */
    uint32_t can_rx_id;         /* RX filter: match value */
};

struct sockaddr_can {
    uint16_t can_family;        /* AF_CAN (29) */
    int      can_ifindex;       /* interface index */
    struct   can_addr can_addr; /* CAN-specific addressing */
};

/* ── Raw CAN filter (for setsockopt CAN_RAW_FILTER) ─────────────── */

struct can_filter {
    uint32_t can_id;            /* CAN identifier to match */
    uint32_t can_mask;          /* mask: 0 = don't care, ~0 = exact match */
};

/* ── Socket options for CAN_RAW (matching Linux) ────────────────── */

#define CAN_RAW_FILTER          1   /* set filter(s) */
#define CAN_RAW_ERR_FILTER      2   /* set error filter */
#define CAN_RAW_LOOPBACK        3   /* local loopback (default on) */
#define CAN_RAW_RECV_OWN_MSGS   4   /* receive own messages */
#define CAN_RAW_FD_FRAMES       5   /* allow CAN FD frames */

/* ── CAN controller state (for virtual CAN / driver interface) ──── */

/* CAN bus error states */
#define CAN_STATE_ERROR_ACTIVE      0
#define CAN_STATE_ERROR_WARNING     1
#define CAN_STATE_ERROR_PASSIVE     2
#define CAN_STATE_BUS_OFF           3
#define CAN_STATE_STOPPED           4
#define CAN_STATE_SLEEPING          5

/* ── Error frame type bits (Linux compatibility) ────────────────── */

#define CAN_ERR_TX_TIMEOUT     0x00000001U  /* TX timeout (by netdevice) */
#define CAN_ERR_LOSTARB        0x00000002U  /* lost arbitration */
#define CAN_ERR_CRTL           0x00000004U  /* controller problems */
#define CAN_ERR_PROT           0x00000008U  /* protocol violations */
#define CAN_ERR_TRX            0x00000010U  /* transceiver status */
#define CAN_ERR_ACK            0x00000020U  /* received no ACK on transmission */
#define CAN_ERR_BUSOFF         0x00000040U  /* bus off */
#define CAN_ERR_BUSERROR       0x00000080U  /* bus error (may flood!) */
#define CAN_ERR_RESTARTED      0x00000100U  /* controller restarted */

/* ── CAN bus virtual interface ──────────────────────────────────── */

/* Maximum number of CAN sockets */
#define CAN_MAX_SOCKETS        16

/* CAN receive queue size per socket (ring buffer) */
#define CAN_RX_QUEUE_SIZE      64

/* CAN socket instance (internal to can.c) */
struct can_socket {
    int      used;                  /* slot in-use */
    int      fd;                    /* file descriptor index */
    int      protocol;              /* CAN_RAW, CAN_BCM, etc. */
    uint32_t ifindex;               /* bound interface */

    /* RX filter */
    int      have_filter;           /* 1 = use filter */
    struct can_filter filter;       /* single filter (extendable) */

    /* Loopback / receive own */
    int      loopback;              /* 1 = loopback enabled (default) */
    int      recv_own_msgs;         /* 1 = receive own messages */

    /* RX ring buffer */
    volatile int rx_head;
    volatile int rx_tail;
    struct can_frame rx_queue[CAN_RX_QUEUE_SIZE];
};

/* ── Public API ─────────────────────────────────────────────────── */

/* Initialize the CAN subsystem */
void can_init(void);

/* Create a CAN socket.  Returns socket fd or -errno. */
int can_create(int protocol);

/* Bind a CAN socket to an interface index.  Returns 0 or -errno. */
int can_bind(int sock_fd, const struct sockaddr_can *addr);

/* Send a CAN frame.  Returns bytes sent or -errno. */
int can_send(int sock_fd, const struct can_frame *frame);

/* Receive a CAN frame (non-blocking: returns -EAGAIN if empty). */
int can_recv(int sock_fd, struct can_frame *frame);

/* Set a socket option on a CAN socket. */
int can_setsockopt(int sock_fd, int level, int optname,
                   const void *optval, uint32_t optlen);

/* Poll a CAN socket for readiness.  Returns POLLIN/POLLOUT flags. */
int can_poll(int sock_fd);

/* Close a CAN socket. */
void can_close(int sock_fd);

/* Get socket name (for getsockname).  Returns 0 or -errno. */
int can_getsockname(int sock_fd, struct sockaddr_can *addr);

/* Deliver an incoming CAN frame to all matching sockets (loopback). */
int can_deliver(const struct can_frame *frame, uint32_t ifindex);

#endif /* CAN_H */
