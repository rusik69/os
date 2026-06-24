/* dccp.c — Datagram Congestion Control Protocol (RFC 4340) */

#include "dccp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

#define DCCP_MAX_SOCKS 8

static struct dccp_sock dccp_socks[DCCP_MAX_SOCKS];
static spinlock_t dccp_lock;
static int dccp_initialized = 0;

void dccp_init(void)
{
    if (dccp_initialized) return;
    spinlock_init(&dccp_lock);
    memset(dccp_socks, 0, sizeof(dccp_socks));
    dccp_initialized = 1;
    kprintf("[OK] DCCP: Datagram Congestion Control Protocol initialized\n");
}

static int dccp_find_free(void)
{
    for (int i = 0; i < DCCP_MAX_SOCKS; i++) {
        if (!dccp_socks[i].used)
            return i;
    }
    return -1;
}

static struct dccp_sock *dccp_find_by_fd(int fd)
{
    for (int i = 0; i < DCCP_MAX_SOCKS; i++) {
        if (dccp_socks[i].used && dccp_socks[i].fd == fd)
            return &dccp_socks[i];
    }
    return NULL;
}

int dccp_create(int fd, int type)
{
    (void)type;
    if (!dccp_initialized) return -ENOSYS;

    spinlock_acquire(&dccp_lock);
    int slot = dccp_find_free();
    if (slot < 0) {
        spinlock_release(&dccp_lock);
        return -ENOMEM;
    }

    struct dccp_sock *ds = &dccp_socks[slot];
    memset(ds, 0, sizeof(*ds));
    ds->used = 1;
    ds->fd = fd;
    ds->ccid = DCCP_CCID_3;  /* Default: TFRC */
    ds->cwnd = 2;
    ds->ssthresh = 16;
    ds->rtt = 100;  /* Initial RTT estimate: 100ms */

    spinlock_release(&dccp_lock);
    return 0;
}

int dccp_bind(int fd, uint16_t port)
{
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }
    ds->local_port = port;
    spinlock_release(&dccp_lock);
    return 0;
}

int dccp_connect(int fd, uint32_t ip, uint16_t port)
{
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    ds->peer_ip = ip;
    ds->peer_port = port;
    ds->seq = 1;

    /* Send DCCP-Request */
    uint8_t pkt[sizeof(struct dccp_header)];
    struct dccp_header *dh = (struct dccp_header *)pkt;
    memset(dh, 0, sizeof(*dh));
    dh->src_port = htons(ds->local_port);
    dh->dst_port = htons(port);
    dh->data_offset = (sizeof(*dh) / 4) << 4;  /* Header length in 32-bit words */
    dh->type_reset = (DCCP_PKT_REQUEST << 4);
    dh->seq_low = htonl(ds->seq);

    send_ip(ip, IPPROTO_DCCP, pkt, sizeof(*dh));
    ds->connected = 1;
    spinlock_release(&dccp_lock);
    return 0;
}

int dccp_send(int fd, const void *data, uint16_t len)
{
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds || !ds->connected) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    /* Build DCCP-Data packet */
    uint8_t pkt[sizeof(struct dccp_header) + 2048];
    struct dccp_header *dh = (struct dccp_header *)pkt;
    memset(dh, 0, sizeof(*dh));
    dh->src_port = htons(ds->local_port);
    dh->dst_port = htons(ds->peer_port);
    dh->data_offset = (sizeof(*dh) / 4) << 4;
    dh->type_reset = (DCCP_PKT_DATA << 4);
    dh->seq_low = htonl(++ds->seq);

    uint16_t data_len = len > 2048 ? 2048 : len;
    memcpy(pkt + sizeof(*dh), data, data_len);

    send_ip(ds->peer_ip, IPPROTO_DCCP, pkt, sizeof(*dh) + data_len);
    spinlock_release(&dccp_lock);
    return data_len;
}

int dccp_recv(int fd, void *buf, uint16_t maxlen)
{
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }
    if (ds->rcvlen == 0) {
        spinlock_release(&dccp_lock);
        return -EAGAIN;
    }

    int copy_len = (ds->rcvlen < maxlen) ? ds->rcvlen : maxlen;
    memcpy(buf, ds->rcvbuf, copy_len);
    ds->rcvlen = 0;
    spinlock_release(&dccp_lock);
    return copy_len;
}

void dccp_close(int fd)
{
    spinlock_acquire(&dccp_lock);
    for (int i = 0; i < DCCP_MAX_SOCKS; i++) {
        if (dccp_socks[i].used && dccp_socks[i].fd == fd) {
            memset(&dccp_socks[i], 0, sizeof(struct dccp_sock));
            break;
        }
    }
    spinlock_release(&dccp_lock);
}

int dccp_is_valid_fd(int fd)
{
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    spinlock_release(&dccp_lock);
    return ds != NULL;
}

void handle_dccp(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *payload, uint16_t len)
{
    (void)dst_ip;
    if (len < sizeof(struct dccp_header)) return;

    const struct dccp_header *dh = (const struct dccp_header *)payload;
    uint16_t dst_port = ntohs(dh->dst_port);
    uint8_t pkt_type = (dh->type_reset >> 4) & 0x0F;

    spinlock_acquire(&dccp_lock);

    struct dccp_sock *ds = NULL;
    for (int i = 0; i < DCCP_MAX_SOCKS; i++) {
        if (dccp_socks[i].used && dccp_socks[i].local_port == dst_port) {
            ds = &dccp_socks[i];
            break;
        }
    }
    if (!ds) {
        spinlock_release(&dccp_lock);
        return;
    }

    switch (pkt_type) {
    case DCCP_PKT_DATA:
    case DCCP_PKT_DATAACK: {
        uint8_t data_offset = (dh->data_offset >> 4) * 4;
        if (data_offset < sizeof(*dh)) data_offset = sizeof(*dh);
        uint16_t data_len = len - data_offset;
        if (data_len > sizeof(ds->rcvbuf)) data_len = sizeof(ds->rcvbuf);
        memcpy(ds->rcvbuf, payload + data_offset, data_len);
        ds->rcvlen = data_len;
        ds->ack_seq = ntohl(dh->seq_low);
        break;
    }
    case DCCP_PKT_REQUEST:
        /* DCCP-Request: respond with DCCP-Response */
        ds->peer_ip = src_ip;
        ds->peer_port = ntohs(dh->src_port);
        ds->connected = 1;
        {
            uint8_t resp[sizeof(struct dccp_header)];
            struct dccp_header *rh = (struct dccp_header *)resp;
            memset(rh, 0, sizeof(*rh));
            rh->src_port = dh->dst_port;
            rh->dst_port = dh->src_port;
            rh->data_offset = (sizeof(*rh) / 4) << 4;
            rh->type_reset = (DCCP_PKT_RESPONSE << 4);
            rh->seq_low = htonl(++ds->seq);
            send_ip(src_ip, IPPROTO_DCCP, resp, sizeof(*rh));
        }
        break;
    case DCCP_PKT_RESPONSE:
        ds->peer_ip = src_ip;
        ds->peer_port = ntohs(dh->src_port);
        ds->connected = 1;
        break;
    case DCCP_PKT_CLOSEREQ: {
        /* DCCP-CloseReq: peer wants to close — send DCCP-Close */
        uint8_t close_pkt[sizeof(struct dccp_header)];
        struct dccp_header *ch = (struct dccp_header *)close_pkt;
        memset(ch, 0, sizeof(*ch));
        ch->src_port = dh->dst_port;
        ch->dst_port = dh->src_port;
        ch->data_offset = (sizeof(*ch) / 4) << 4;
        ch->type_reset = (DCCP_PKT_CLOSE << 4);
        ch->seq_low = htonl(++ds->seq);
        send_ip(src_ip, IPPROTO_DCCP, close_pkt, sizeof(*ch));
        ds->connected = 0;
        kprintf("dccp: CloseReq received, sending Close\n");
        break;
    }
    case DCCP_PKT_CLOSE:
    case DCCP_PKT_RESET:
        ds->connected = 0;
        break;
    default:
        break;
    }

    spinlock_release(&dccp_lock);
}

EXPORT_SYMBOL(dccp_init);
EXPORT_SYMBOL(dccp_create);
EXPORT_SYMBOL(dccp_bind);
EXPORT_SYMBOL(dccp_connect);
EXPORT_SYMBOL(dccp_send);
EXPORT_SYMBOL(dccp_recv);
EXPORT_SYMBOL(dccp_close);
/* ── Implement: dccp_recvmsg ────────────────── */
int dccp_recvmsg(int fd, void *buf, uint16_t maxlen, int flags)
{
    (void)flags;
    /* Delegate to dccp_recv */
    return dccp_recv(fd, buf, maxlen);
}

/* ── Implement: dccp_sendmsg ────────────────── */
int dccp_sendmsg(int fd, const void *data, uint16_t len, int flags)
{
    (void)flags;
    /* Delegate to dccp_send */
    return dccp_send(fd, data, len);
}

/* ── Implement: dccp_ioctl ────────────────── */
int dccp_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    (void)request;
    (void)arg;
    kprintf("[dccp] dccp_ioctl: unsupported request %lu\n", request);
    return -EOPNOTSUPP;
}

/* ── Implement: dccp_setsockopt ────────────────── */
int dccp_setsockopt(int fd, int level, int optname, const void *optval, uint16_t optlen)
{
    if (!dccp_initialized) return -ENOSYS;
    if (level != IPPROTO_DCCP) return -ENOPROTOOPT;

    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    int ret = 0;
    switch (optname) {
    case 0x01: /* DCCP_SOCKOPT_SERVICE — set service code (dummy) */
        if (optlen >= sizeof(uint32_t))
            ds->service_code = *(const uint32_t *)optval;
        break;
    case 0x02: /* DCCP_SOCKOPT_CCID — set CCID */
        if (optlen >= sizeof(int)) {
            int ccid = *(const int *)optval;
            if (ccid == DCCP_CCID_2 || ccid == DCCP_CCID_3)
                ds->ccid = ccid;
            else
                ret = -EINVAL;
        } else {
            ret = -EINVAL;
        }
        break;
    case 0x03: /* DCCP_SOCKOPT_TX_CCID — set TX CCID */
    case 0x04: /* DCCP_SOCKOPT_RX_CCID — set RX CCID */
        break; /* accept silently */
    default:
        ret = -ENOPROTOOPT;
        break;
    }

    spinlock_release(&dccp_lock);
    return ret;
}

/* ── Implement: dccp_getsockopt ────────────────── */
int dccp_getsockopt(int fd, int level, int optname, void *optval, uint16_t *optlen)
{
    if (!dccp_initialized) return -ENOSYS;
    if (level != IPPROTO_DCCP) return -ENOPROTOOPT;
    if (!optval || !optlen) return -EINVAL;

    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    int ret = 0;
    switch (optname) {
    case 0x01: /* DCCP_SOCKOPT_SERVICE */
        if (*optlen >= sizeof(uint32_t)) {
            *(uint32_t *)optval = ds->service_code;
            *optlen = sizeof(uint32_t);
        } else {
            ret = -EINVAL;
        }
        break;
    case 0x02: /* DCCP_SOCKOPT_CCID */
        if (*optlen >= sizeof(int)) {
            *(int *)optval = ds->ccid;
            *optlen = sizeof(int);
        } else {
            ret = -EINVAL;
        }
        break;
    default:
        ret = -ENOPROTOOPT;
        break;
    }

    spinlock_release(&dccp_lock);
    return ret;
}

/* ── Implement: dccp_shutdown ────────────────── */
int dccp_shutdown(int fd, int how)
{
    if (!dccp_initialized) return -ENOSYS;

    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    /* how: 0=read, 1=write, 2=both */
    if (how == 1 || how == 2) {
        /* Send DCCP-Close */
        if (ds->connected) {
            uint8_t close_pkt[sizeof(struct dccp_header)];
            struct dccp_header *ch = (struct dccp_header *)close_pkt;
            memset(ch, 0, sizeof(*ch));
            ch->src_port = htons(ds->local_port);
            ch->dst_port = htons(ds->peer_port);
            ch->data_offset = (sizeof(*ch) / 4) << 4;
            ch->type_reset = (DCCP_PKT_CLOSE << 4);
            ch->seq_low = htonl(++ds->seq);
            send_ip(ds->peer_ip, IPPROTO_DCCP, close_pkt, sizeof(*ch));
        }
    }
    if (how == 0 || how == 2) {
        ds->rcvlen = 0; /* Discard pending data */
    }

    spinlock_release(&dccp_lock);
    return 0;
}

/* ── Implement: dccp_listen ────────────────── */
int dccp_listen(int fd, int backlog)
{
    if (!dccp_initialized) return -ENOSYS;
    (void)backlog;

    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    /* Mark socket as passive (listening) — accept will create new sockets */
    ds->state = 1; /* LISTENING */
    ds->backlog = (backlog > 0 && backlog <= 10) ? backlog : 5;

    spinlock_release(&dccp_lock);
    return 0;
}

/* ── Implement: dccp_accept ────────────────── */
int dccp_accept(int fd, uint32_t *peer_ip, uint16_t *peer_port)
{
    if (!dccp_initialized) return -ENOSYS;
    if (!peer_ip || !peer_port) return -EINVAL;

    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds || !ds->state) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    /* Return the most recent connecting peer's info */
    *peer_ip = ds->peer_ip;
    *peer_port = ds->peer_port;

    spinlock_release(&dccp_lock);
    kprintf("[dccp] accept on fd %d: peer %d.%d.%d.%d:%u\n",
            fd,
            (*peer_ip >> 24) & 0xFF, (*peer_ip >> 16) & 0xFF,
            (*peer_ip >> 8) & 0xFF, *peer_ip & 0xFF,
            (unsigned)*peer_port);
    return 0;
}

EXPORT_SYMBOL(dccp_recvmsg);
EXPORT_SYMBOL(dccp_sendmsg);
EXPORT_SYMBOL(dccp_ioctl);
EXPORT_SYMBOL(dccp_setsockopt);
EXPORT_SYMBOL(dccp_getsockopt);
EXPORT_SYMBOL(dccp_shutdown);
EXPORT_SYMBOL(dccp_listen);
EXPORT_SYMBOL(dccp_accept);
EXPORT_SYMBOL(handle_dccp);

/* ── dccp_disconnect: disconnect a DCCP socket ── */
int dccp_disconnect(int fd)
{
    if (!dccp_initialized) return -ENOSYS;
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }
    if (!ds->connected) {
        spinlock_release(&dccp_lock);
        return -ENOTCONN;
    }
    /* Send DCCP-Close if connected */
    {
        uint8_t close_pkt[sizeof(struct dccp_header)];
        struct dccp_header *ch = (struct dccp_header *)close_pkt;
        memset(ch, 0, sizeof(*ch));
        ch->src_port = htons(ds->local_port);
        ch->dst_port = htons(ds->peer_port);
        ch->data_offset = (sizeof(*ch) / 4) << 4;
        ch->type_reset = (DCCP_PKT_CLOSE << 4);
        ch->seq_low = htonl(++ds->seq);
        send_ip(ds->peer_ip, IPPROTO_DCCP, close_pkt, sizeof(*ch));
    }
    ds->connected = 0;
    ds->state = 0;
    ds->rcvlen = 0;
    spinlock_release(&dccp_lock);
    return 0;
}

/* ── dccp_send_request: send explicit DCCP-Request on a connected socket ── */
int dccp_send_request(int fd)
{
    if (!dccp_initialized) return -ENOSYS;
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }
    /* Re-send DCCP-Request */
    ds->seq++;
    uint8_t pkt[sizeof(struct dccp_header)];
    struct dccp_header *dh = (struct dccp_header *)pkt;
    memset(dh, 0, sizeof(*dh));
    dh->src_port = htons(ds->local_port);
    dh->dst_port = htons(ds->peer_port);
    dh->data_offset = (sizeof(*dh) / 4) << 4;
    dh->type_reset = (DCCP_PKT_REQUEST << 4);
    dh->seq_low = htonl(ds->seq);
    send_ip(ds->peer_ip, IPPROTO_DCCP, pkt, sizeof(*dh));
    spinlock_release(&dccp_lock);
    return 0;
}

/* ── dccp_rcv_response: process received DCCP-Response packet ── */
int dccp_rcv_response(int fd, uint32_t src_ip, uint16_t src_port)
{
    if (!dccp_initialized) return -ENOSYS;
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }
    ds->peer_ip = src_ip;
    ds->peer_port = src_port;
    ds->connected = 1;
    ds->state = 1; /* ESTABLISHED */
    kprintf("[dccp] dccp_rcv_response: fd=%d connected to %d.%d.%d.%d:%u\n",
            fd,
            (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
            (src_ip >> 8) & 0xFF, src_ip & 0xFF,
            (unsigned int)src_port);
    spinlock_release(&dccp_lock);
    return 0;
}

#include "module.h"
module_init(dccp_init);
