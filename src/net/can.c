/*
 * can.c — CAN bus (SocketCAN) protocol family implementation (Item 352)
 *
 * Provides AF_CAN sockets with RAW protocol support for sending and
 * receiving CAN frames.  This is a software-only implementation using
 * an internal virtual CAN bus with loopback delivery.
 *
 * Integration:
 *   - Socket creation: sys_socket_impl() in socket.c dispatches to can_create()
 *   - Socket operations: bind, send, recv, setsockopt, poll, close
 *   - Frame delivery: can_deliver() is called from the virtual CAN bus driver
 *
 * Reference: Linux SocketCAN (Documentation/networking/can.rst)
 */

#include "can.h"
#include "socket.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "process.h"

/* ── Static CAN socket table ────────────────────────────────────── */

static struct can_socket can_sockets[CAN_MAX_SOCKETS];
static int can_initialized = 0;
static spinlock_t can_lock;

/* ── Utility functions ──────────────────────────────────────────── */

/* Map a file descriptor to a CAN socket pointer */
static struct can_socket *can_fd_to_sock(int fd)
{
    if (fd < 0 || fd >= CAN_MAX_SOCKETS)
        return NULL;
    struct can_socket *s = &can_sockets[fd];
    if (!s->used)
        return NULL;
    return s;
}

/* Check if a CAN frame matches a filter */
static int can_match_filter(const struct can_frame *frame,
                            const struct can_filter *filter)
{
    uint32_t frame_id = frame->can_id & CAN_ERR_MASK;
    uint32_t filter_id = filter->can_id & CAN_ERR_MASK;
    uint32_t mask = filter->can_mask & CAN_ERR_MASK;

    /* No mask = match all */
    if (mask == 0)
        return 1;

    /* Check ID match */
    return (frame_id & mask) == (filter_id & mask);
}

/* ── Initialization ─────────────────────────────────────────────── */

void can_init(void)
{
    if (can_initialized)
        return;

    memset(can_sockets, 0, sizeof(can_sockets));
    spinlock_init(&can_lock);
    can_initialized = 1;

    kprintf("[OK] CAN bus: SocketCAN initialized (AF_CAN=29, %d sockets)\n",
            CAN_MAX_SOCKETS);
}

/* ── Socket lifecycle ───────────────────────────────────────────── */

int can_create(int protocol)
{
    if (!can_initialized)
        return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&can_lock, &irq_flags);

    /* Find a free slot */
    int fd = -1;
    for (int i = 0; i < CAN_MAX_SOCKETS; i++) {
        if (!can_sockets[i].used) {
            fd = i;
            break;
        }
    }

    if (fd < 0) {
        spinlock_irqsave_release(&can_lock, irq_flags);
        return -ENFILE;
    }

    struct can_socket *s = &can_sockets[fd];
    memset(s, 0, sizeof(*s));
    s->used     = 1;
    s->fd       = fd;
    s->protocol = protocol;
    s->loopback = 1;    /* loopback on by default (Linux convention) */
    s->rx_head  = 0;
    s->rx_tail  = 0;

    spinlock_irqsave_release(&can_lock, irq_flags);

    return fd;
}

int can_bind(int sock_fd, const struct sockaddr_can *addr)
{
    if (!can_initialized)
        return -ENODEV;

    struct can_socket *s = can_fd_to_sock(sock_fd);
    if (!s)
        return -EBADF;

    if (!addr)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&can_lock, &irq_flags);

    /* Bind to interface index (0 = any interface) */
    s->ifindex = (uint32_t)(addr->can_ifindex >= 0 ? addr->can_ifindex : 0);

    spinlock_irqsave_release(&can_lock, irq_flags);

    return 0;
}

void can_close(int sock_fd)
{
    if (!can_initialized)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&can_lock, &irq_flags);

    struct can_socket *s = can_fd_to_sock(sock_fd);
    if (s) {
        memset(s, 0, sizeof(*s));
    }

    spinlock_irqsave_release(&can_lock, irq_flags);
}

/* ── Send / Receive ─────────────────────────────────────────────── */

int can_send(int sock_fd, const struct can_frame *frame)
{
    if (!can_initialized || !frame)
        return -EINVAL;

    struct can_socket *s = can_fd_to_sock(sock_fd);
    if (!s)
        return -EBADF;

    /* Validate frame */
    if (frame->can_dlc > CAN_MAX_DLC)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&can_lock, &irq_flags);

    /* Local loopback: deliver to all listening sockets on this interface */
    if (s->loopback) {
        for (int i = 0; i < CAN_MAX_SOCKETS; i++) {
            struct can_socket *target = &can_sockets[i];
            if (!target->used || target->fd == sock_fd)
                continue;
            if (target->ifindex != 0 && target->ifindex != s->ifindex)
                continue;

            /* Apply filter if set */
            if (target->have_filter &&
                !can_match_filter(frame, &target->filter))
                continue;

            /* Queue frame in target's RX buffer */
            int next_head = (target->rx_head + 1) % CAN_RX_QUEUE_SIZE;
            if (next_head != target->rx_tail) {
                memcpy(&target->rx_queue[target->rx_head], frame,
                       sizeof(struct can_frame));
                target->rx_head = next_head;
            }
        }
    }

    /* Loopback to self if recv_own_msgs is set */
    if (s->recv_own_msgs) {
        int next_head = (s->rx_head + 1) % CAN_RX_QUEUE_SIZE;
        if (next_head != s->rx_tail) {
            memcpy(&s->rx_queue[s->rx_head], frame,
                   sizeof(struct can_frame));
            s->rx_head = next_head;
        }
    }

    spinlock_irqsave_release(&can_lock, irq_flags);

    /* Return number of bytes sent (Linux convention: frame size) */
    return (int)sizeof(struct can_frame);
}

int can_recv(int sock_fd, struct can_frame *frame)
{
    if (!can_initialized || !frame)
        return -EINVAL;

    struct can_socket *s = can_fd_to_sock(sock_fd);
    if (!s)
        return -EBADF;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&can_lock, &irq_flags);

    /* Check if data is available */
    if (s->rx_tail == s->rx_head) {
        spinlock_irqsave_release(&can_lock, irq_flags);
        return -EAGAIN;
    }

    /* Dequeue a frame */
    memcpy(frame, &s->rx_queue[s->rx_tail], sizeof(struct can_frame));
    s->rx_tail = (s->rx_tail + 1) % CAN_RX_QUEUE_SIZE;

    spinlock_irqsave_release(&can_lock, irq_flags);

    return (int)sizeof(struct can_frame);
}

/* ── Socket options ─────────────────────────────────────────────── */

int can_setsockopt(int sock_fd, int level, int optname,
                   const void *optval, uint32_t optlen)
{
    if (!can_initialized)
        return -ENODEV;

    struct can_socket *s = can_fd_to_sock(sock_fd);
    if (!s)
        return -EBADF;

    if (level != SOL_CAN_RAW && level != SOL_CAN_BASE)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&can_lock, &irq_flags);
    int ret = 0;

    switch (optname) {
    case CAN_RAW_FILTER: {
        if (optlen < sizeof(struct can_filter)) {
            ret = -EINVAL;
            break;
        }
        const struct can_filter *f = (const struct can_filter *)optval;
        s->have_filter = 1;
        s->filter.can_id  = f->can_id;
        s->filter.can_mask = f->can_mask;
        break;
    }

    case CAN_RAW_LOOPBACK:
        if (optlen >= sizeof(int)) {
            s->loopback = *(const int *)optval ? 1 : 0;
        } else {
            ret = -EINVAL;
        }
        break;

    case CAN_RAW_RECV_OWN_MSGS:
        if (optlen >= sizeof(int)) {
            s->recv_own_msgs = *(const int *)optval ? 1 : 0;
        } else {
            ret = -EINVAL;
        }
        break;

    default:
        ret = -ENOPROTOOPT;
        break;
    }

    spinlock_irqsave_release(&can_lock, irq_flags);
    return ret;
}

/* ── Poll support ───────────────────────────────────────────────── */

int can_poll(int sock_fd)
{
    if (!can_initialized)
        return 0;

    struct can_socket *s = can_fd_to_sock(sock_fd);
    if (!s)
        return 0;

    int events = 0;

    /* Always writable (CAN send is always possible in our vCAN) */
    events |= POLLOUT;

    /* Readable if RX queue has data */
    if (s->rx_tail != s->rx_head)
        events |= POLLIN;

    return events;
}

/* ── External frame delivery (from virtual CAN driver) ──────────── */

int can_deliver(const struct can_frame *frame, uint32_t ifindex)
{
    if (!can_initialized || !frame)
        return -EINVAL;

    if (frame->can_dlc > CAN_MAX_DLC)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&can_lock, &irq_flags);

    int delivered = 0;

    for (int i = 0; i < CAN_MAX_SOCKETS; i++) {
        struct can_socket *s = &can_sockets[i];
        if (!s->used)
            continue;

        /* Check interface match */
        if (s->ifindex != 0 && s->ifindex != ifindex)
            continue;

        /* Apply filter if set */
        if (s->have_filter && !can_match_filter(frame, &s->filter))
            continue;

        /* Queue frame */
        int next_head = (s->rx_head + 1) % CAN_RX_QUEUE_SIZE;
        if (next_head != s->rx_tail) {
            memcpy(&s->rx_queue[s->rx_head], frame,
                   sizeof(struct can_frame));
            s->rx_head = next_head;
            delivered++;
        }
    }

    spinlock_irqsave_release(&can_lock, irq_flags);
    return delivered;
}

/* ── Utility: get socket address (for getsockname) ──────────────── */

int can_getsockname(int sock_fd, struct sockaddr_can *addr)
{
    if (!can_initialized || !addr)
        return -EINVAL;

    struct can_socket *s = can_fd_to_sock(sock_fd);
    if (!s)
        return -EBADF;

    addr->can_family  = AF_CAN;
    addr->can_ifindex = (int)s->ifindex;
    addr->can_addr.can_ifindex  = s->ifindex;
    addr->can_addr.can_tx_id    = 0;
    addr->can_addr.can_rx_id    = 0;
    addr->can_addr.can_rx_id_mask = 0;

    return 0;
}
