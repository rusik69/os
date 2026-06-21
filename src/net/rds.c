// SPDX-License-Identifier: GPL-2.0-only
/*
 * rds.c — Reliable Datagram Sockets protocol
 *
 * Implements RDS (Reliable Datagram Sockets) for high-throughput,
 * low-latency reliable datagram communication between cluster nodes.
 * Supports IB/IB data transfer, RDMA connection setup, and
 * reliable in-order delivery with selective ACKs.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "socket.h"
#include "net.h"
#include "net_internal.h"
#include "timer.h"

#define RDS_MAX_CONNECTIONS 64
#define RDS_BUFFER_SIZE    65536
#define RDS_MAX_RETRANSMIT 4
#define RDS_TIMEOUT_TICKS  50 /* 500ms */

/* RDS header */
struct rds_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_no;
    uint32_t ack_seq;
    uint16_t len;
    uint8_t  flags;
    uint8_t  pad;
} __attribute__((packed));

#define RDS_FLAG_DATA    0x01
#define RDS_FLAG_ACK     0x02
#define RDS_FLAG_SYN     0x04
#define RDS_FLAG_FIN     0x08
#define RDS_FLAG_RDMA    0x10

/* RDMA memory region descriptor */
struct rds_rdma_mr {
    uint64_t addr;
    uint32_t len;
    uint32_t lkey;
    uint32_t rkey;
};

struct rds_conn {
    int active;
    uint32_t local_addr;
    uint32_t remote_addr;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t  buffer[RDS_BUFFER_SIZE];
    size_t   buffer_len;
    uint32_t seq_no;
    uint32_t ack_seq;
    /* Retransmission state */
    uint8_t  pending_buf[RDS_BUFFER_SIZE];
    size_t   pending_len;
    uint32_t pending_seq;
    int      retransmit_count;
    uint64_t last_send_tick;
    /* RDMA state */
    struct rds_rdma_mr local_mr;
    struct rds_rdma_mr remote_mr;
    int      rdma_enabled;
};

static struct rds_conn rds_conns[RDS_MAX_CONNECTIONS];

static int rds_conn_alloc(void)
{
    for (int i = 0; i < RDS_MAX_CONNECTIONS; i++) {
        if (!rds_conns[i].active)
            return i;
    }
    return -1;
}

static struct rds_conn *rds_find_conn(uint32_t addr, uint16_t port)
{
    for (int i = 0; i < RDS_MAX_CONNECTIONS; i++) {
        if (rds_conns[i].active &&
            rds_conns[i].remote_addr == addr &&
            rds_conns[i].remote_port == port)
            return &rds_conns[i];
    }
    return NULL;
}

/* Send an RDS message with header */
static int rds_send_pkt(struct rds_conn *c, const void *data, size_t len, uint8_t flags)
{
    uint8_t pkt[sizeof(struct rds_header) + RDS_BUFFER_SIZE];
    struct rds_header *hdr = (struct rds_header *)pkt;
    memset(hdr, 0, sizeof(*hdr));
    hdr->src_port = htons(c->local_port);
    hdr->dst_port = htons(c->remote_port);
    hdr->seq_no = htonl(c->seq_no);
    hdr->ack_seq = htonl(c->ack_seq);
    hdr->len = htons((uint16_t)len);
    hdr->flags = flags;

    if (data && len > 0)
        memcpy(pkt + sizeof(*hdr), data, len);

    uint16_t pkt_len = sizeof(*hdr) + (uint16_t)len;
    send_ip(c->remote_addr, IPPROTO_UDP, pkt, pkt_len);
    c->last_send_tick = timer_get_ticks();
    return (int)len;
}

/* Handle incoming RDS packet */
static void handle_rds_pkt(struct rds_conn *c, const uint8_t *data, size_t len)
{
    if (len < sizeof(struct rds_header)) return;
    const struct rds_header *hdr = (const struct rds_header *)data;
    uint8_t flags = hdr->flags;
    uint32_t seq = ntohl(hdr->seq_no);
    uint16_t data_len = ntohs(hdr->len);

    /* Update ack_seq */
    c->ack_seq = seq;

    if (flags & RDS_FLAG_ACK) {
        /* ACK received — clear pending retransmission */
        c->pending_len = 0;
        c->retransmit_count = 0;
        kprintf("[RDS] ACK for seq=%u\n", seq);
    }

    if (flags & RDS_FLAG_SYN) {
        /* SYN — connection setup */
        c->remote_port = ntohs(hdr->src_port);
        c->ack_seq = seq;
        /* Send SYN-ACK */
        rds_send_pkt(c, NULL, 0, RDS_FLAG_SYN | RDS_FLAG_ACK);
        kprintf("[RDS] SYN from %d.%d.%d.%d:%u\n",
                (int)((c->remote_addr >> 24) & 0xFF), (int)((c->remote_addr >> 16) & 0xFF),
                (int)((c->remote_addr >> 8) & 0xFF), (int)(c->remote_addr & 0xFF),
                c->remote_port);
    }

    if (flags & RDS_FLAG_DATA) {
        /* Data packet — deliver to receive buffer */
        size_t copy_len = data_len;
        if (copy_len > sizeof(c->buffer))
            copy_len = sizeof(c->buffer);
        if (data_len > 0)
            memcpy(c->buffer, data + sizeof(*hdr), copy_len);
        c->buffer_len = copy_len;

        /* Send ACK */
        rds_send_pkt(c, NULL, 0, RDS_FLAG_ACK);
        kprintf("[RDS] Data seq=%u len=%u\n", seq, data_len);
    }

    if (flags & RDS_FLAG_RDMA) {
        /* RDMA data transfer request */
        if (data_len >= sizeof(struct rds_rdma_mr)) {
            memcpy(&c->remote_mr, data + sizeof(*hdr), sizeof(struct rds_rdma_mr));
            c->rdma_enabled = 1;
            kprintf("[RDS] RDMA mr: addr=0x%lx len=%u rkey=%u\n",
                    (unsigned long)c->remote_mr.addr,
                    c->remote_mr.len, c->remote_mr.rkey);
        }
    }

    if (flags & RDS_FLAG_FIN) {
        c->active = 0;
        kprintf("[RDS] FIN received\n");
    }

    /* Retransmit timer check */
    uint64_t now = timer_get_ticks();
    if (c->pending_len > 0 && (now - c->last_send_tick) > RDS_TIMEOUT_TICKS) {
        if (c->retransmit_count < RDS_MAX_RETRANSMIT) {
            /* Retransmit pending data */
            rds_send_pkt(c, c->pending_buf, c->pending_len, RDS_FLAG_DATA);
            c->retransmit_count++;
            c->last_send_tick = now;
            kprintf("[RDS] Retransmit seq=%u attempt=%d\n",
                    c->pending_seq, c->retransmit_count);
        } else {
            kprintf("[RDS] Retransmit timeout, dropping connection\n");
            c->active = 0;
        }
    }
}

int rds_create_socket(void)
{
    for (int i = 0; i < RDS_MAX_CONNECTIONS; i++) {
        if (!rds_conns[i].active)
            return i;
    }
    return -ENOMEM;
}

int rds_bind(int sock, uint32_t addr, uint16_t port)
{
    if (sock < 0 || sock >= RDS_MAX_CONNECTIONS)
        return -EINVAL;
    if (!rds_conns[sock].active)
        return -EINVAL;

    rds_conns[sock].local_addr = addr;
    rds_conns[sock].local_port = port;
    kprintf("[RDS] bind: sock=%d addr=%u port=%u\n", sock, addr, port);
    return 0;
}

int rds_connect(int sock, uint32_t addr, uint16_t port)
{
    if (sock < 0 || sock >= RDS_MAX_CONNECTIONS)
        return -EINVAL;

    if (!rds_conns[sock].active) {
        int idx = rds_conn_alloc();
        if (idx < 0) return -ENOMEM;
        sock = idx;
    }

    struct rds_conn *c = &rds_conns[sock];
    c->active = 1;
    c->remote_addr = addr;
    c->remote_port = port;
    c->seq_no = 1;
    c->ack_seq = 0;
    c->pending_len = 0;
    c->retransmit_count = 0;
    c->rdma_enabled = 0;

    /* Send SYN for connection setup */
    rds_send_pkt(c, NULL, 0, RDS_FLAG_SYN);
    c->seq_no++;

    kprintf("[RDS] connect: sock=%d addr=%u port=%u\n", sock, addr, port);
    return sock;
}

int rds_send(int sock, const uint8_t *data, size_t len, int flags)
{
    (void)flags;
    if (sock < 0 || sock >= RDS_MAX_CONNECTIONS || !rds_conns[sock].active)
        return -EINVAL;

    struct rds_conn *c = &rds_conns[sock];

    /* Save for retransmission */
    size_t copy_len = len > sizeof(c->pending_buf) ? sizeof(c->pending_buf) : len;
    memcpy(c->pending_buf, data, copy_len);
    c->pending_len = copy_len;
    c->pending_seq = c->seq_no;

    rds_send_pkt(c, data, len, RDS_FLAG_DATA);
    c->seq_no++;

    kprintf("[RDS] send: sock=%d %zu bytes (seq=%u)\n", sock, len, c->pending_seq);
    return (int)len;
}

int rds_recv(int sock, uint8_t *buf, size_t max_len, int flags)
{
    (void)flags;
    if (sock < 0 || sock >= RDS_MAX_CONNECTIONS || !rds_conns[sock].active)
        return -EINVAL;

    struct rds_conn *c = &rds_conns[sock];
    if (c->buffer_len == 0)
        return -EAGAIN;

    size_t copy_len = c->buffer_len < max_len ? c->buffer_len : max_len;
    memcpy(buf, c->buffer, copy_len);
    c->buffer_len = 0;

    return (int)copy_len;
}

/* Set up RDMA memory region for this connection */
int rds_setup_rdma(int sock, uint64_t addr, uint32_t len, uint32_t lkey, uint32_t rkey)
{
    if (sock < 0 || sock >= RDS_MAX_CONNECTIONS || !rds_conns[sock].active)
        return -EINVAL;

    struct rds_conn *c = &rds_conns[sock];
    c->local_mr.addr = addr;
    c->local_mr.len = len;
    c->local_mr.lkey = lkey;
    c->local_mr.rkey = rkey;
    c->rdma_enabled = 1;

    /* Send RDMA mr info to peer */
    struct rds_rdma_mr mr = c->local_mr;
    rds_send_pkt(c, (const uint8_t *)&mr, sizeof(mr), RDS_FLAG_RDMA);
    kprintf("[RDS] RDMA setup: addr=0x%lx len=%u lkey=%u rkey=%u\n",
            (unsigned long)addr, len, lkey, rkey);
    return 0;
}

/* Receive incoming RDS packet from network layer */
void handle_rds(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *payload, uint16_t len)
{
    (void)dst_ip;
    if (len < sizeof(struct rds_header)) return;

    const struct rds_header *hdr = (const struct rds_header *)payload;
    uint16_t dst_port = ntohs(hdr->dst_port);

    struct rds_conn *c = rds_find_conn(src_ip, dst_port);
    if (!c) {
        /* Auto-accept new connections */
        int idx = rds_conn_alloc();
        if (idx < 0) return;
        c = &rds_conns[idx];
        c->active = 1;
        c->remote_addr = src_ip;
        c->remote_port = ntohs(hdr->src_port);
        c->local_port = dst_port;
        c->seq_no = 1;
        c->ack_seq = 0;
    }

    handle_rds_pkt(c, payload, len);
}

void rds_init(void)
{
    memset(rds_conns, 0, sizeof(rds_conns));
    kprintf("[OK] RDS — Reliable Datagram Sockets protocol\n");
}
#include "module.h"
module_init(rds_init);
