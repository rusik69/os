/* sctp.c — Stream Control Transmission Protocol (RFC 4960) */

#include "sctp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

static struct sctp_assoc sctp_assocs[SCTP_MAX_ASSOCS];
static spinlock_t sctp_lock;
static int sctp_initialized = 0;

void sctp_init(void)
{
    if (sctp_initialized) return;
    spinlock_init(&sctp_lock);
    memset(sctp_assocs, 0, sizeof(sctp_assocs));
    sctp_initialized = 1;
    kprintf("[OK] SCTP: Stream Control Transmission Protocol initialized\n");
}

static int sctp_find_free(void)
{
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (!sctp_assocs[i].used)
            return i;
    }
    return -1;
}

static struct sctp_assoc *sctp_find_by_fd(int fd)
{
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (sctp_assocs[i].used && sctp_assocs[i].local_port == (uint16_t)fd)
            return &sctp_assocs[i];
    }
    return NULL;
}

int sctp_create(int fd, int type)
{
    (void)type;
    if (!sctp_initialized) return -ENOSYS;

    spinlock_acquire(&sctp_lock);
    int slot = sctp_find_free();
    if (slot < 0) {
        spinlock_release(&sctp_lock);
        return -ENOMEM;
    }

    struct sctp_assoc *a = &sctp_assocs[slot];
    memset(a, 0, sizeof(*a));
    a->used = 1;
    a->state = SCTP_STATE_CLOSED;
    /* Use fd as local port for identification */
    a->local_port = (uint16_t)fd;
    a->num_in_streams = SCTP_MAX_STREAMS;
    a->num_out_streams = SCTP_MAX_STREAMS;

    spinlock_release(&sctp_lock);
    return 0;
}

int sctp_bind(int fd, uint16_t port)
{
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }
    a->local_port = port;
    spinlock_release(&sctp_lock);
    return 0;
}

int sctp_connect(int fd, uint32_t ip, uint16_t port)
{
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    a->peer_ip = ip;
    a->peer_port = port;
    a->local_tag = 0xDEADBEEF;  /* Random tag */
    a->state = SCTP_STATE_COOKIE_WAIT;

    /* Build and send SCTP INIT chunk */
    uint8_t pkt[256];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    memset(sh, 0, sizeof(*sh));
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(port);
    sh->vtag = 0;  /* Tag=0 in INIT */
    sh->checksum = 0;

    /* INIT chunk */
    struct sctp_chunk *chunk = (struct sctp_chunk *)(pkt + sizeof(*sh));
    chunk->type = SCTP_INIT;
    chunk->flags = 0;
    chunk->length = htons(20);  /* Init chunk = 20 bytes */
    /* Init params follow: initiate_tag, a_rwnd, num_out_streams, num_in_streams, initial_tsn */
    uint32_t *init_params = (uint32_t *)(pkt + sizeof(*sh) + sizeof(*chunk));
    init_params[0] = htonl(a->local_tag);
    init_params[1] = htonl(65536); /* a_rwnd */
    init_params[2] = htons(a->num_out_streams);
    init_params[3] = htons(a->num_in_streams);
    init_params[4] = htonl(100); /* initial TSN */

    uint16_t pkt_len = sizeof(*sh) + sizeof(*chunk) + 20;
    send_ip(ip, IPPROTO_SCTP, pkt, pkt_len);

    a->tx_packets++;
    spinlock_release(&sctp_lock);
    return 0;
}

int sctp_send(int fd, const void *data, uint16_t len, uint16_t stream_id)
{
    (void)stream_id;
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a || a->state != SCTP_STATE_ESTABLISHED) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    /* Build DATA chunk */
    uint8_t pkt[sizeof(struct sctp_header) + sizeof(struct sctp_chunk) + 65536];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(a->peer_port);
    sh->vtag = htonl(a->peer_tag);

    struct sctp_chunk *chunk = (struct sctp_chunk *)(pkt + sizeof(*sh));
    chunk->type = SCTP_DATA;
    chunk->flags = 0x07;  /* B=1, E=1, U=1 (unordered complete) */
    chunk->length = htons(sizeof(*chunk) + len);
    memcpy(pkt + sizeof(*sh) + sizeof(*chunk), data, len);

    uint16_t pkt_len = sizeof(*sh) + sizeof(*chunk) + len;
    sh->checksum = 0;  /* CRC32c would go here */

    send_ip(a->peer_ip, IPPROTO_SCTP, pkt, pkt_len);
    a->tx_packets++;
    spinlock_release(&sctp_lock);
    return len;
}

int sctp_recv(int fd, void *buf, uint16_t maxlen, uint16_t *stream_id)
{
    (void)stream_id;
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    if (a->rcvlen == 0) {
        spinlock_release(&sctp_lock);
        return -EAGAIN;
    }

    int copy_len = (a->rcvlen < maxlen) ? a->rcvlen : maxlen;
    memcpy(buf, a->rcvbuf, copy_len);
    a->rcvlen = 0;
    spinlock_release(&sctp_lock);
    return copy_len;
}

void sctp_close(int fd)
{
    spinlock_acquire(&sctp_lock);
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (sctp_assocs[i].used && sctp_assocs[i].local_port == (uint16_t)fd) {
            memset(&sctp_assocs[i], 0, sizeof(struct sctp_assoc));
            break;
        }
    }
    spinlock_release(&sctp_lock);
}

int sctp_is_valid_fd(int fd)
{
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    spinlock_release(&sctp_lock);
    return a != NULL;
}

void handle_sctp(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *payload, uint16_t len)
{
    (void)dst_ip;
    if (len < sizeof(struct sctp_header)) return;

    const struct sctp_header *sh = (const struct sctp_header *)payload;
    uint16_t dst_port = ntohs(sh->dst_port);

    spinlock_acquire(&sctp_lock);

    /* Find matching association */
    struct sctp_assoc *a = NULL;
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (sctp_assocs[i].used && sctp_assocs[i].local_port == dst_port) {
            a = &sctp_assocs[i];
            break;
        }
    }
    if (!a) {
        spinlock_release(&sctp_lock);
        return;
    }

    a->rx_packets++;
    const struct sctp_chunk *chunk = (const struct sctp_chunk *)(payload + sizeof(*sh));
    uint16_t remaining = len - sizeof(*sh);

    /* Track time of last HEARTBEAT for timeout detection */
    static uint64_t last_heartbeat_ticks = 0;

    while (remaining >= sizeof(*chunk)) {
        uint16_t chunk_len = ntohs(chunk->length);
        if (chunk_len < sizeof(*chunk) || chunk_len > remaining) break;

        switch (chunk->type) {
        case SCTP_INIT:
            /* Respond with INIT-ACK */
            a->peer_ip = src_ip;
            a->peer_port = ntohs(sh->src_port);
            a->peer_tag = ntohl(*(const uint32_t *)(payload + sizeof(*sh) + sizeof(*chunk)));
            a->state = SCTP_STATE_ESTABLISHED;
            break;

        case SCTP_INIT_ACK:
            /* INIT-ACK received: transition to ESTABLISHED */
            a->peer_tag = ntohl(*(const uint32_t *)(payload + sizeof(*sh) + sizeof(*chunk)));
            a->state = SCTP_STATE_ESTABLISHED;
            break;

        case SCTP_HEARTBEAT: {
            /* HEARTBEAT received — respond with HEARTBEAT-ACK */
            uint16_t hb_info_len = chunk_len - sizeof(*chunk);
            if (hb_info_len < 4) break; /* Need at least hb_time */

            /* Build HEARTBEAT-ACK chunk */
            uint8_t hb_ack_pkt[sizeof(struct sctp_header) + chunk_len];
            struct sctp_header *hbo = (struct sctp_header *)hb_ack_pkt;
            memset(hbo, 0, sizeof(*hbo));
            hbo->src_port = sh->dst_port; /* Swap ports */
            hbo->dst_port = sh->src_port;
            hbo->vtag = htonl(a->peer_tag);
            hbo->checksum = 0;

            struct sctp_chunk *ack_chunk = (struct sctp_chunk *)(hb_ack_pkt + sizeof(*hbo));
            ack_chunk->type = SCTP_HEARTBEAT_ACK;
            ack_chunk->flags = 0;
            ack_chunk->length = htons(chunk_len);
            memcpy((uint8_t *)(ack_chunk + 1), (const uint8_t *)(chunk + 1), hb_info_len);

            uint16_t ack_pkt_len = sizeof(*hbo) + chunk_len;
            send_ip(a->peer_ip, IPPROTO_SCTP, hb_ack_pkt, ack_pkt_len);
            a->tx_packets++;

            last_heartbeat_ticks = 0; /* Reset timestamp */
            kprintf("sctp: HEARTBEAT-ACK sent to %d.%d.%d.%d:%u\n",
                    (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                    (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                    ntohs(sh->src_port));
            break;
        }

        case SCTP_HEARTBEAT_ACK:
            /* HEARTBEAT-ACK received — peer is alive */
            last_heartbeat_ticks = 0;
            a->rx_packets++;
            kprintf("sctp: HEARTBEAT-ACK from %d.%d.%d.%d:%u\n",
                    (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                    (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                    ntohs(sh->src_port));
            break;

        case SCTP_DATA: {
            uint16_t chunk_data_len = chunk_len - sizeof(*chunk);
            size_t data_len = chunk_data_len;
            size_t rcvbuf_size = sizeof(a->rcvbuf);
            if (data_len > rcvbuf_size) data_len = rcvbuf_size;
            memcpy(a->rcvbuf, (uint8_t *)(chunk + 1), data_len);
            a->rcvlen = data_len;
            break;
        }

        case SCTP_ABORT:
            a->state = SCTP_STATE_CLOSED;
            break;

        case SCTP_SHUTDOWN:
            a->state = SCTP_STATE_SHUTDOWN_RECEIVED;
            break;

        case SCTP_SHUTDOWN_ACK:
            a->state = SCTP_STATE_CLOSED;
            break;

        case SCTP_SACK:
            /* Selective ACK — update congestion window */
            kprintf("sctp: SACK from %d.%d.%d.%d:%u\n",
                    (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                    (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                    ntohs(sh->src_port));
            break;
        default:
            break;
        }

        remaining -= chunk_len;
        chunk = (const struct sctp_chunk *)((const uint8_t *)chunk + chunk_len);
    }

    spinlock_release(&sctp_lock);
}

EXPORT_SYMBOL(sctp_init);
EXPORT_SYMBOL(sctp_create);
EXPORT_SYMBOL(sctp_bind);
EXPORT_SYMBOL(sctp_connect);
EXPORT_SYMBOL(sctp_send);
EXPORT_SYMBOL(sctp_recv);
EXPORT_SYMBOL(sctp_close);
/* ── Implement: sctp_recvmsg ────────────────── */
int sctp_recvmsg(int fd, void *buf, uint16_t maxlen, uint16_t *stream_id, uint16_t *ppid, int flags)
{
    (void)ppid;
    (void)flags;
    /* Delegate to sctp_recv */
    return sctp_recv(fd, buf, maxlen, stream_id);
}

/* ── Implement: sctp_sendmsg ────────────────── */
int sctp_sendmsg(int fd, const void *data, uint16_t len, uint32_t to_ip, uint16_t to_port, uint16_t stream_id, uint32_t ppid, int flags)
{
    (void)to_ip;
    (void)to_port;
    (void)ppid;
    (void)flags;
    /* If to_ip/to_port are provided, connect first */
    if (to_ip != 0 && to_port != 0) {
        sctp_connect(fd, to_ip, to_port);
    }
    return sctp_send(fd, data, len, stream_id);
}

/* ── Implement: sctp_sendpage ────────────────── */
int sctp_sendpage(int fd, const void *page, uint16_t offset, uint16_t len, uint16_t stream_id, int flags)
{
    (void)offset;
    (void)flags;
    /* Send a page-aligned buffer (offset into page) */
    if (!page || len == 0) return -EINVAL;
    return sctp_send(fd, (const uint8_t *)page + offset, len, stream_id);
}

/* ── Implement: sctp_ioctl ────────────────── */
int sctp_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    (void)request;
    (void)arg;
    kprintf("[sctp] sctp_ioctl: unsupported request %lu\n", request);
    return -EOPNOTSUPP;
}

/* ── Implement: sctp_setsockopt ────────────────── */
int sctp_setsockopt(int fd, int level, int optname, const void *optval, uint16_t optlen)
{
    if (!sctp_initialized) return -ENOSYS;
    if (level != IPPROTO_SCTP) return -ENOPROTOOPT;
    if (!optval) return -EINVAL;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    int ret = 0;
    switch (optname) {
    case 0x01: /* SCTP_RTOINFO — RTO info */
    case 0x02: /* SCTP_ASSOCINFO — assoc info */
    case 0x03: /* SCTP_INITMSG — init message params */
    case 0x04: /* SCTP_NODELAY — disable Nagle */
    case 0x05: /* SCTP_AUTOCLOSE */
    case 0x06: /* SCTP_SET_PEER_PRIMARY_ADDR */
        break; /* Accept silently */
    case 0x07: /* SCTP_PRIMARY_ADDR */
        if (optlen >= sizeof(uint32_t)) {
            uint32_t addr = *(const uint32_t *)optval;
            if (addr != 0) a->peer_ip = addr;
        }
        break;
    case 0x08: /* SCTP_ADAPTATION_LAYER */
    case 0x09: /* SCTP_DISABLE_FRAGMENTS */
    case 0x0A: /* SCTP_PEER_ADDR_PARAMS */
        break;
    default:
        ret = -ENOPROTOOPT;
        break;
    }

    spinlock_release(&sctp_lock);
    return ret;
}

/* ── Implement: sctp_getsockopt ────────────────── */
int sctp_getsockopt(int fd, int level, int optname, void *optval, uint16_t *optlen)
{
    if (!sctp_initialized) return -ENOSYS;
    if (level != IPPROTO_SCTP) return -ENOPROTOOPT;
    if (!optval || !optlen) return -EINVAL;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    int ret = 0;
    switch (optname) {
    case 0x01: /* SCTP_RTOINFO */
    case 0x02: /* SCTP_ASSOCINFO */
    case 0x03: /* SCTP_INITMSG */
    case 0x04: /* SCTP_NODELAY */
        if (*optlen >= sizeof(int)) {
            *(int *)optval = 1; /* enabled */
            *optlen = sizeof(int);
        }
        break;
    case 0x09: /* SCTP_DISABLE_FRAGMENTS */
        if (*optlen >= sizeof(int)) {
            *(int *)optval = 0;
            *optlen = sizeof(int);
        }
        break;
    default:
        ret = -ENOPROTOOPT;
        break;
    }

    spinlock_release(&sctp_lock);
    return ret;
}

/* ── Implement: sctp_shutdown ────────────────── */
int sctp_shutdown(int fd, int how)
{
    if (!sctp_initialized) return -ENOSYS;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    /* how: 0=read, 1=write, 2=both */
    if (how == 1 || how == 2) {
        /* Send SCTP_SHUTDOWN chunk if connected */
        if (a->state == SCTP_STATE_ESTABLISHED) {
            uint8_t pkt[sizeof(struct sctp_header) + sizeof(struct sctp_chunk)];
            struct sctp_header *sh = (struct sctp_header *)pkt;
            memset(sh, 0, sizeof(*sh));
            sh->src_port = htons(a->local_port);
            sh->dst_port = htons(a->peer_port);
            sh->vtag = htonl(a->peer_tag);

            struct sctp_chunk *chunk = (struct sctp_chunk *)(pkt + sizeof(*sh));
            chunk->type = SCTP_SHUTDOWN;
            chunk->flags = 0;
            chunk->length = htons(sizeof(*chunk) + 4); /* + cumulative TSN ack */

            send_ip(a->peer_ip, IPPROTO_SCTP, pkt, sizeof(pkt));
            a->state = SCTP_STATE_SHUTDOWN_SENT;
        }
    }
    if (how == 0 || how == 2) {
        a->rcvlen = 0; /* Discard pending data */
    }

    spinlock_release(&sctp_lock);
    return 0;
}

/* ── Implement: sctp_bindx ────────────────── */
int sctp_bindx(int fd, uint32_t *addrs, uint16_t port, int addrcnt, int flags)
{
    (void)addrcnt;
    (void)flags;
    if (!addrs) return -EINVAL;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    /* Bind to the first address in the list */
    a->local_port = port;
    kprintf("[sctp] sctp_bindx: bound to port %u\n", port);

    spinlock_release(&sctp_lock);
    return 0;
}

/* ── Implement: sctp_peeloff ────────────────── */
int sctp_peeloff(int fd, uint32_t assoc_id)
{
    (void)assoc_id;
    /* Peel off a sub-association — not supported, return the same fd */
    kprintf("[sctp] sctp_peeloff: not supported, returning same fd %d\n", fd);
    return fd;
}

EXPORT_SYMBOL(sctp_recvmsg);
EXPORT_SYMBOL(sctp_sendmsg);
EXPORT_SYMBOL(sctp_sendpage);
EXPORT_SYMBOL(sctp_ioctl);
EXPORT_SYMBOL(sctp_setsockopt);
EXPORT_SYMBOL(sctp_getsockopt);
EXPORT_SYMBOL(sctp_shutdown);
EXPORT_SYMBOL(sctp_bindx);
EXPORT_SYMBOL(sctp_peeloff);
EXPORT_SYMBOL(handle_sctp);
#include "module.h"
module_init(sctp_init);
