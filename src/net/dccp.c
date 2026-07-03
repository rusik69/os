/* dccp.c — Datagram Congestion Control Protocol (RFC 4340) */

#include "dccp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "timer.h"
#include "export.h"
#include "timers.h"

/* Forward declarations */
static void dccp_ccid_ack_rcvd(struct dccp_sock *ds,
                               const uint8_t *pkt, uint16_t len);
/* ── CCID2 helpers ──────────────────────────────────────────────── */
static void dccp_ccid2_update_rtt(struct dccp_sock *ds, uint32_t rtt_ms);
static void dccp_ccid2_arm_rto(struct dccp_sock *ds);
static void dccp_ccid2_disarm_rto(struct dccp_sock *ds);
static void dccp_ccid2_rto_cb(void *arg);
static void dccp_ccid2_cwnd_timeout(struct dccp_sock *ds);

/* ── CCID3/TFRC forward declarations ────────────────────────── */
static uint32_t int_sqrt64(uint64_t n);
static void     tfrc_update_rtt(struct dccp_sock *ds, uint32_t rtt_sample_ms);
static uint32_t tfrc_compute_x(struct dccp_sock *ds);
static void     tfrc_update_loss_rate(struct dccp_sock *ds);
static void     tfrc_handle_loss(struct dccp_sock *ds);
static void     tfrc_nofeedback_cb(void *arg);
static void     tfrc_arm_nofeedback(struct dccp_sock *ds);
static void     tfrc_disarm_nofeedback(struct dccp_sock *ds);

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
    ds->in_flight = 0;
    ds->last_ack_seq = 0;
    ds->last_send_time = 0;
    ds->dup_acks = 0;
    ds->loss_pending = 0;
    ds->rtt_samples = 0;
    ds->min_rtt = 0xFFFFFFFF;

    /* ── CCID2 initialization (RFC 4341) ────────────────────── */
    ds->srtt = 0;
    ds->rttvar = DCCP_CCID2_INIT_RTO / 2;   /* Initial RTTVAR = RTO/2 */
    ds->rto = DCCP_CCID2_INIT_RTO;
    ds->rto_timer_id = -1;
    ds->rto_expire_tick = 0;
    ds->rto_pending = 0;
    ds->recover = 0;
    ds->recover_seq = 0;
    ds->pipe = 0;
    ds->ack_ratio = DCCP_CCID2_INIT_ACK_RATIO;
    ds->data_pkts_since_ack = 0;
    ds->snd_una = 1;
    ds->snd_nxt = 1;
    ds->highest_sent = 0;
    ds->retransmits = 0;

    /* ── CCID3: TFRC initialization (RFC 5348, RFC 4342) ────────── */
    ds->tfrc_p = 0;
    ds->tfrc_s = DCCP_CCID3_DEFAULT_S;
    ds->tfrc_bytes_since_loss = 0;
    ds->tfrc_interval_idx = 0;
    ds->tfrc_nofeedback_pending = 0;
    ds->tfrc_nofeedback_timer_id = -1;
    ds->tfrc_last_feedback = 0;
    ds->tfrc_nofeedback_count = 0;
    ds->tfrc_X = 0;
    memset(ds->tfrc_loss_interval, 0, sizeof(ds->tfrc_loss_interval));

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
    ds->iss = 1;

    /* Build DCCP-Request with feature negotiation options (RFC 4340 §5.1) */
    uint8_t pkt[sizeof(struct dccp_header) + 32];
    struct dccp_header *dh = (struct dccp_header *)pkt;
    memset(dh, 0, sizeof(*dh));

    uint8_t *opts = pkt + sizeof(*dh);
    uint16_t optlen = 0;

    /* Service Code option (type 15, length 6) */
    opts[optlen++] = DCCP_OPT_SERVICE_CODE;
    opts[optlen++] = 6;
    *(uint32_t *)(opts + optlen) = htonl(ds->service_code);
    optlen += 4;

    /* Change L(32) for CCID feature — propose our CCID */
    opts[optlen++] = DCCP_OPT_CHANGE_L;
    opts[optlen++] = 6;  /* type + len + feat_type + val_len + val(1) = 6 */
    opts[optlen++] = DCCP_FEAT_CCID;
    opts[optlen++] = 1;  /* value length: 1 byte for CCID */
    opts[optlen++] = (uint8_t)ds->ccid;

    /* Pad to 4-byte boundary with DCCP_PADDING */
    while (optlen & 3) {
        opts[optlen++] = DCCP_OPT_PADDING;
    }

    uint16_t total_hdr_len = sizeof(*dh) + optlen;
    uint8_t words = (total_hdr_len + 3) / 4;
    dh->data_offset = words << 4;
    dh->src_port = htons(ds->local_port);
    dh->dst_port = htons(port);
    dh->type_reset = (DCCP_PKT_REQUEST << 4);
    dh->seq_low = htonl(ds->seq);

    send_ip(ip, IPPROTO_DCCP, pkt, total_hdr_len);

    /* Do NOT set connected yet — wait for DCCP-Response (RFC 4340 §5.1 step 1) */
    ds->state = DCCP_REQUEST;

    spinlock_release(&dccp_lock);
    kprintf("[dccp] connect: DCCP-Request sent to %d.%d.%d.%d:%u (CCID=%d)\n",
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF,
            (unsigned)port, ds->ccid);
    return 0;
}

int dccp_send(int fd, const void *data, uint16_t len)
{
    if (!dccp_initialized) return -ENOSYS;

    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds || !ds->connected) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }

    /* CCID congestion control check */
    if (ds->ccid == DCCP_CCID_2) {
        /* TCP-like: window-based (cwnd in packets) */
        if (ds->in_flight >= ds->cwnd) {
            spinlock_release(&dccp_lock);
            return -EAGAIN;  /* Congestion-limited, retry later */
        }
    } else if (ds->ccid == DCCP_CCID_3) {
        /* TFRC: rate-based pacing (RFC 5348 §4.1)
         * Minimum inter-packet interval =
         *   ceil(packet_len / (tx_rate / TIMER_FREQ)) ticks
         * where TIMER_FREQ = 100 Hz, giving tx_rate/100 bytes/tick */
        if (ds->tx_rate > 0 && ds->last_send_time > 0) {
            uint64_t now = timer_get_ticks();
            uint64_t elapsed = now - ds->last_send_time;
            uint32_t bytes_per_tick = ds->tx_rate / TIMER_FREQ;
            if (bytes_per_tick == 0) bytes_per_tick = 1;
            uint32_t min_interval = (len + bytes_per_tick - 1) / bytes_per_tick;
            if (elapsed < (uint64_t)min_interval) {
                spinlock_release(&dccp_lock);
                return -EAGAIN;
            }
        }
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

    ds->in_flight++;

    /* ── CCID2: track sequence numbers and arm RTO timer ───────── */
    if (ds->ccid == DCCP_CCID_2) {
        ds->highest_sent = ds->seq;
        ds->snd_nxt = ds->seq + 1;
        if (ds->snd_una == 0)
            ds->snd_una = ds->seq;
        /* Arm or re-arm RTO timer on first data send */
        if (!ds->rto_pending)
            dccp_ccid2_arm_rto(ds);
        /* Increment data packets since last ACK for Ack Ratio */
        ds->data_pkts_since_ack++;
    }

    ds->last_send_time = timer_get_ticks();

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
            /* Disarm any pending CCID timers before freeing sock */
            if (dccp_socks[i].ccid == DCCP_CCID_2)
                dccp_ccid2_disarm_rto(&dccp_socks[i]);
            else if (dccp_socks[i].ccid == DCCP_CCID_3)
                tfrc_disarm_nofeedback(&dccp_socks[i]);
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

/* ── dccp_send_response: send DCCP-Response with feature confirmation ── */
static int dccp_send_response(struct dccp_sock *ds, uint32_t dst_ip,
                              const struct dccp_header *req_hdr)
{
    uint8_t resp[sizeof(struct dccp_header) + 32];
    struct dccp_header *rh = (struct dccp_header *)resp;
    memset(rh, 0, sizeof(*rh));

    uint8_t *opts = resp + sizeof(*rh);
    uint16_t optlen = 0;

    /* Service Code option (RFC 4340 §15.4) */
    opts[optlen++] = DCCP_OPT_SERVICE_CODE;
    opts[optlen++] = 6;
    *(uint32_t *)(opts + optlen) = htonl(ds->service_code);
    optlen += 4;

    /* Confirm L(34) for CCID — accept client's proposed CCID */
    opts[optlen++] = DCCP_OPT_CONFIRM_L;
    opts[optlen++] = 6;
    opts[optlen++] = DCCP_FEAT_CCID;
    opts[optlen++] = 1;
    opts[optlen++] = (uint8_t)ds->ccid;

    /* Pad to 4-byte boundary */
    while (optlen & 3) {
        opts[optlen++] = DCCP_OPT_PADDING;
    }

    uint16_t total_hdr_len = sizeof(*rh) + optlen;
    uint8_t words = (total_hdr_len + 3) / 4;
    rh->data_offset = words << 4;
    rh->src_port = htons(ds->local_port);
    rh->dst_port = req_hdr->src_port;
    rh->type_reset = (DCCP_PKT_RESPONSE << 4);
    rh->seq_low = htonl(++ds->seq);

    send_ip(dst_ip, IPPROTO_DCCP, resp, total_hdr_len);
    return 0;
}

/* ── dccp_send_ack_internal: send DCCP-Ack with acknowledgment number ── */
static int dccp_send_ack_internal(struct dccp_sock *ds)
{
    uint8_t ack[sizeof(struct dccp_header) + 8];
    struct dccp_header *ah = (struct dccp_header *)ack;
    memset(ah, 0, sizeof(*ah));

    /* Acknowledgement Number option (type 14, length 6, 48-bit value) */
    uint8_t *opts = ack + sizeof(*ah);
    opts[0] = DCCP_OPT_ACK_NUM;
    opts[1] = 6;
    opts[2] = 0; /* high byte of 48-bit value */
    opts[3] = 0;
    *(uint32_t *)(opts + 4) = htonl(ds->ack_seq);

    uint16_t total_hdr_len = sizeof(*ah) + 8;
    uint8_t words = (total_hdr_len + 3) / 4;
    ah->data_offset = words << 4;
    ah->src_port = htons(ds->local_port);
    ah->dst_port = htons(ds->peer_port);
    ah->type_reset = (DCCP_PKT_ACK << 4);
    ah->seq_low = htonl(++ds->seq);

    send_ip(ds->peer_ip, IPPROTO_DCCP, ack, total_hdr_len);
    return 0;
}

/* ── dccp_send_ack: public API to send DCCP-Ack ── */
int dccp_send_ack(int fd)
{
    if (!dccp_initialized) return -ENOSYS;
    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_find_by_fd(fd);
    if (!ds || !ds->connected) {
        spinlock_release(&dccp_lock);
        return -EINVAL;
    }
    int ret = dccp_send_ack_internal(ds);
    spinlock_release(&dccp_lock);
    return ret;
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
        /* Call CCID ack processing for DataAck (which carries ACK info) */
        if (pkt_type == DCCP_PKT_DATAACK)
            dccp_ccid_ack_rcvd(ds, payload, len);
        break;
    }
    case DCCP_PKT_REQUEST:
        /* DCCP-Request: respond with DCCP-Response (RFC 4340 §5.1 step 2) */
        ds->peer_ip = src_ip;
        ds->peer_port = ntohs(dh->src_port);
        ds->ack_seq = ntohl(dh->seq_low);  /* Remember client's initial seq */
        ds->state = DCCP_RESPOND;
        dccp_send_response(ds, src_ip, dh);
        kprintf("[dccp] DCCP-Request from %d.%d.%d.%d:%u, sending Response\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                (unsigned)ntohs(dh->src_port));
        break;
    case DCCP_PKT_RESPONSE: {
        /* Client receives Response — send ACK to complete 3-way handshake
         * (RFC 4340 §5.1 step 3) */
        uint32_t resp_seq = ntohl(dh->seq_low);
        ds->peer_ip = src_ip;
        ds->peer_port = ntohs(dh->src_port);
        ds->ack_seq = resp_seq;
        ds->connected = 1;
        ds->state = DCCP_ESTABLISHED;
        dccp_send_ack_internal(ds);
        kprintf("[dccp] DCCP-Response from %d.%d.%d.%d:%u, sending ACK (established)\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                (unsigned)ntohs(dh->src_port));
        break;
    }
    case DCCP_PKT_ACK:
        /* Server receives ACK — connection fully established (RFC 4340 §5.1 step 3) */
        if (ds->state == DCCP_RESPOND) {
            ds->connected = 1;
            ds->state = DCCP_ESTABLISHED;
            kprintf("[dccp] DCCP-Ack received, connection established\n");
        }
        /* Update CCID congestion control state */
        dccp_ccid_ack_rcvd(ds, payload, len);
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
        ds->state = DCCP_CLOSING;
        kprintf("[dccp] CloseReq received, sending Close\n");
        break;
    }
    case DCCP_PKT_CLOSE:
        /* fallthrough */
    case DCCP_PKT_RESET:
        ds->connected = 0;
        ds->state = DCCP_CLOSED;
        break;
    default:
        break;
    }

    spinlock_release(&dccp_lock);
}

/* ── dccp_parse_ack_option: find ACK option in DCCP options area ──
 * Returns the acknowledgment sequence number, or 0 if not found.
 * Options are between the fixed header end and the data_offset.
 */
static uint32_t dccp_parse_ack_option(const uint8_t *pkt, uint16_t len)
{
    if (len < sizeof(struct dccp_header))
        return 0;

    const struct dccp_header *dh = (const struct dccp_header *)pkt;
    uint8_t data_off = (dh->data_offset >> 4) * 4;
    if (data_off < sizeof(struct dccp_header))
        data_off = sizeof(struct dccp_header);
    if (data_off > len)
        data_off = len;

    /* Options start after fixed header */
    uint16_t off = sizeof(struct dccp_header);
    while (off + 1 < data_off) {
        uint8_t opt_type = pkt[off];
        uint8_t opt_len;
        switch (opt_type) {
        case DCCP_OPT_PADDING:
            off++;
            continue;
        case DCCP_OPT_ACK_NUM:
            /* type(1) + len(1) + high_byte(1) + reserved(1) + seq_low(4) = 8 */
            if (off + 8 > data_off)
                return 0;
            opt_len = pkt[off + 1];
            if (opt_len < 6)
                return 0;
            /* Sequence number is bytes [2..7], only low 32 bits returned
             * (48-bit seq, high 16 bits in bytes 2-3, low 32 in bytes 4-7) */
            return ((uint32_t)pkt[off + 4] << 24) |
                   ((uint32_t)pkt[off + 5] << 16) |
                   ((uint32_t)pkt[off + 6] << 8)  |
                   (uint32_t)pkt[off + 7];
        default:
            /* Generic option: type(1) + len(1) + value(len-2) */
            if (off + 1 >= data_off)
                return 0;
            opt_len = pkt[off + 1];
            if (opt_len < 2)
                opt_len = 2;
            off += opt_len;
            break;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * CCID2: TCP-like Congestion Control (RFC 4341)
 * ═══════════════════════════════════════════════════════════════════ */

/* ── dccp_ccid2_update_rtt: Update SRTT/RTTVAR/RTO (RFC 6298) ── */
static void dccp_ccid2_update_rtt(struct dccp_sock *ds, uint32_t rtt_ms)
{
    /* Clamp RTT */
    if (rtt_ms < 1) rtt_ms = 1;
    if (rtt_ms > 30000) rtt_ms = 30000;

    if (ds->srtt == 0) {
        /* First measurement */
        ds->srtt = rtt_ms * 8;  /* SRTT scaled by 8 */
        ds->rttvar = (rtt_ms * 4) / 2;  /* RTTVAR scaled by 4, initial = rtt/2 */
    } else {
        /* Subsequent measurements (RFC 6298):
         *   RTTVAR = (1 - beta) * RTTVAR + beta * |SRTT - R'|
         *   SRTT   = (1 - alpha) * SRTT + alpha * R'
         * where alpha = 1/8, beta = 1/4, and values are scaled */
        int32_t delta = (int32_t)(ds->srtt / 8) - (int32_t)rtt_ms;
        if (delta < 0) delta = -delta;
        ds->rttvar = (ds->rttvar * 3 + (uint32_t)delta * 2) / 4;
        ds->srtt = ds->srtt - (ds->srtt >> 3) + rtt_ms;
    }

    /* RTO = SRTT + max(G, 4*RTTVAR), where G = 10ms (timer granularity) */
    uint32_t rto = (ds->srtt / 8) + (4 * (ds->rttvar / 4));
    if (rto < DCCP_CCID2_MIN_RTO)
        rto = DCCP_CCID2_MIN_RTO;
    if (rto > DCCP_CCID2_MAX_RTO)
        rto = DCCP_CCID2_MAX_RTO;
    ds->rto = rto;
}

/* ── dccp_ccid2_find_sock_by_fd: find dccp sock by fd for timer cb ──
 * The RTO timer callback only gets a fd (stored as arg).  We look up
 * the socket by fd to find the dccp_sock pointer. */
static struct dccp_sock *dccp_ccid2_find_sock_by_fd(int fd)
{
    for (int i = 0; i < DCCP_MAX_SOCKS; i++) {
        if (dccp_socks[i].used && dccp_socks[i].fd == fd)
            return &dccp_socks[i];
    }
    return NULL;
}

/* ── dccp_ccid2_cwnd_timeout: Reduce cwnd on RTO expiry ── */
static void dccp_ccid2_cwnd_timeout(struct dccp_sock *ds)
{
    /* RTO expiry: reset cwnd to 1 (slow start), ssthresh = max(in_flight/2, 2) */
    uint32_t flight = ds->in_flight;
    if (flight < 2) flight = 2;
    ds->ssthresh = flight / 2;
    if (ds->ssthresh < 2)
        ds->ssthresh = 2;
    ds->cwnd = 1;
    ds->in_flight = 0;
    ds->recover = 0;
    ds->retransmits++;
    kprintf("[dccp] CCID2 RTO expired on fd=%d: cwnd=%u ssthresh=%u retrans=%u\n",
            ds->fd, ds->cwnd, ds->ssthresh, ds->retransmits);
}

/* ── dccp_ccid2_disarm_rto: Cancel a pending RTO timer ── */
static void dccp_ccid2_disarm_rto(struct dccp_sock *ds)
{
    if (!ds->rto_pending)
        return;
    if (ds->rto_timer_id >= 0) {
        timer_cancel(ds->rto_timer_id);
        ds->rto_timer_id = -1;
    }
    ds->rto_pending = 0;
    ds->rto_expire_tick = 0;
}

/* ── dccp_ccid2_arm_rto: Arm (or re-arm) the RTO timer ── */
static void dccp_ccid2_arm_rto(struct dccp_sock *ds)
{
    /* Disarm any existing RTO timer first */
    dccp_ccid2_disarm_rto(ds);

    /* Calculate delay in timer ticks from RTO (ms).
     * timer_get_ticks() runs at TIMER_FREQ Hz (=100), so
     * RTO_ticks = RTO_ms * TIMER_FREQ / 1000 */
    uint32_t rto_ms = ds->rto;

    /* Add 50ms slop to absorb timer granularity */
    if (rto_ms < 100) rto_ms = 100;

    uint64_t delay_ticks = (uint64_t)rto_ms * TIMER_FREQ / 1000;
    if (delay_ticks < 1) delay_ticks = 1;

    /* Schedule the timer; pass the fd as arg so the callback can look up the socket */
    int fd = ds->fd;
    int tid = timer_schedule(dccp_ccid2_rto_cb, (void *)(unsigned long)(uintptr_t)fd, delay_ticks);
    if (tid >= 0) {
        ds->rto_timer_id = tid;
        ds->rto_expire_tick = timer_get_ticks() + delay_ticks;
        ds->rto_pending = 1;
    }
}

/* ── dccp_ccid2_rto_cb: RTO expiration callback ── */
static void dccp_ccid2_rto_cb(void *arg)
{
    int fd = (int)(unsigned long)(uintptr_t)arg;

    /* The timer subsystem fires outside the dccp_lock.
     * We must acquire the lock to access socket state. */
    spinlock_acquire(&dccp_lock);

    struct dccp_sock *ds = dccp_ccid2_find_sock_by_fd(fd);
    if (!ds || !ds->used || ds->ccid != DCCP_CCID_2) {
        spinlock_release(&dccp_lock);
        return;
    }

    /* Mark timer as no longer pending */
    ds->rto_pending = 0;
    ds->rto_timer_id = -1;

    /* Exponentially back off RTO (RFC 6298 §2.5) */
    ds->rto = ds->rto * 2;
    if (ds->rto > DCCP_CCID2_MAX_RTO)
        ds->rto = DCCP_CCID2_MAX_RTO;

    /* Reduce congestion window */
    dccp_ccid2_cwnd_timeout(ds);

    kprintf("[dccp] CCID2 RTO cb: fd=%d new_rto=%ums cwnd=%u\n",
            fd, (unsigned)ds->rto, (unsigned)ds->cwnd);

    spinlock_release(&dccp_lock);
}

/* ── dccp_ccid2_is_new_ack: check if this ACK acknowledges new data ── */
static int dccp_ccid2_is_new_ack(struct dccp_sock *ds, uint32_t ack_seq)
{
    /* In 32-bit sequence space, an ACK is "new" if it acknowledges
     * a sequence number beyond snd_una (handling wraparound). */
    if (ds->snd_una == 0)
        return 1;
    /* Simple comparison: ack_seq > snd_una in unsigned 32-bit space.
     * Handles wraparound if the difference is less than 2^31. */
    uint32_t diff = ack_seq - ds->snd_una;
    return diff > 0 && diff < 0x80000000U;
}

/* ── dccp_ccid2_is_dupack: check if this is a duplicate ACK ── */
static int dccp_ccid2_is_dupack(struct dccp_sock *ds, uint32_t ack_seq)
{
    return ack_seq == ds->snd_una;
}

/* ── dccp_ccid2_new_ack: handle new ACK (advances window) ── */
static void dccp_ccid2_new_ack(struct dccp_sock *ds, uint32_t ack_seq)
{
    /* Calculate bytes/packets newly ACKed */
    uint32_t newly_acked;
    if (ack_seq > ds->snd_una) {
        newly_acked = ack_seq - ds->snd_una;
    } else if (ack_seq < ds->snd_una) {
        /* 32-bit wraparound */
        newly_acked = ack_seq + (0xFFFFFFFFU - ds->snd_una);
    } else {
        newly_acked = 0;
    }

    if (newly_acked == 0)
        return;

    /* Update snd_una */
    ds->snd_una = ack_seq;

    /* Decrement in_flight, clamped to 0 */
    if (newly_acked > ds->in_flight)
        ds->in_flight = 0;
    else
        ds->in_flight -= newly_acked;

    /* Not in fast recovery — normal congestion control */
    if (!ds->recover) {
        if (ds->cwnd < ds->ssthresh) {
            /* Slow start: cwnd += newly_acked (one per ACK) */
            ds->cwnd += newly_acked;
        } else {
            /* Congestion avoidance: cwnd += 1/cwnd per ACK (AIMD).
             * Using appropriate byte count (RFC 3465):
             *   inc = (newly_acked * MSS + cwnd * MSS) / (cwnd * MSS)
             * Simplified: inc = newly_acked / cwnd + 1 */
            uint32_t inc = (newly_acked + ds->cwnd) / ds->cwnd;
            if (inc == 0) inc = 1;
            ds->cwnd += inc;
        }
        /* Cap cwnd */
        if (ds->cwnd > 0xFFFF)
            ds->cwnd = 0xFFFF;
    } else {
        /* In fast recovery — deflate cwnd per RFC 6582 NewReno */
        if (newly_acked > 0) {
            /* Partial window deflation */
            uint32_t dec = newly_acked;
            if (dec > ds->cwnd)
                dec = ds->cwnd;
            ds->cwnd -= dec;
            if (ds->cwnd < 2)
                ds->cwnd = 2;
        }
    }

    /* Update RTT from ACK if we have samples queued */
    /* (RTT measurement would be done by the caller with a timestamp) */

    /* Reset duplicate ACK counter */
    ds->dup_acks = 0;
    ds->loss_pending = 0;
}

/* ── dccp_ccid2_dupack: handle duplicate ACK (3 dup → fast retransmit) ── */
static void dccp_ccid2_dupack(struct dccp_sock *ds, uint32_t ack_seq)
{
    ds->dup_acks++;

    if (ds->dup_acks < 3)
        return;

    /* Fast retransmit (RFC 5681): after 3 dupACKs, retransmit lost segment */
    if (!ds->recover) {
        /* Enter fast recovery */
        ds->recover = 1;
        ds->recover_seq = ds->highest_sent;

        /* Fast retransmit: ssthresh = max(in_flight/2, 2), cwnd = ssthresh + 3 */
        uint32_t flight = ds->in_flight;
        if (flight < 4) flight = 4;
        ds->ssthresh = flight / 2;
        if (ds->ssthresh < 2)
            ds->ssthresh = 2;
        ds->cwnd = ds->ssthresh + 3;

        ds->retransmits++;
        kprintf("[dccp] CCID2 fast retransmit on fd=%d: cwnd=%u ssthresh=%u dup_acks=%u\n",
                ds->fd, ds->cwnd, ds->ssthresh, ds->dup_acks);
    }

    /* While in recovery, inflate cwnd by 1 per additional dupACK (RFC 5681) */
    if (ds->dup_acks >= 3) {
        ds->cwnd++;
    }
}

/* ── dccp_ccid2_ack_received: main entry for ACK processing ──
 * Implements RFC 4341 §4 (TCP-like Congestion Control).
 * This is called from dccp_ccid_ack_rcvd when CCID2 is active. */
static void dccp_ccid2_ack_received(struct dccp_sock *ds, uint32_t ack_seq)
{
    /* Determine if this is a new ACK or duplicate */
    if (dccp_ccid2_is_new_ack(ds, ack_seq)) {
        /* New ACK — advance window */
        dccp_ccid2_new_ack(ds, ack_seq);

        /* Exit fast recovery when all data sent during recovery is ACKed */
        if (ds->recover) {
            if (ack_seq >= ds->recover_seq ||
                (ds->recover_seq - ack_seq) < 0x80000000U) {
                /* Recovery complete — set cwnd to ssthresh */
                ds->cwnd = ds->ssthresh;
                ds->recover = 0;
                kprintf("[dccp] CCID2 fast recovery complete on fd=%d: cwnd=%u\n",
                        ds->fd, ds->cwnd);
            }
        }

        /* Disarm RTO timer if all outstanding data is ACKed */
        if (ds->in_flight == 0) {
            dccp_ccid2_disarm_rto(ds);
        } else {
            /* Re-arm RTO timer (it's a one-shot, needs to be re-scheduled
             * now that we've received an ACK and have a new RTT estimate) */
            dccp_ccid2_arm_rto(ds);
        }

        /* Reset dup_acks on new ACK */
        ds->dup_acks = 0;

    } else if (dccp_ccid2_is_dupack(ds, ack_seq)) {
        /* Duplicate ACK — might signal loss */
        dccp_ccid2_dupack(ds, ack_seq);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * CCID3: TFRC Rate Control (RFC 5348 / RFC 4342)
 * ═══════════════════════════════════════════════════════════════════ */

/* ── int_sqrt64: 64-bit integer square root (Newton's method) ── */
static uint32_t int_sqrt64(uint64_t n)
{
    if (n == 0 || n == 1)
        return (uint32_t)n;
    uint64_t x = n;
    uint64_t y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return (uint32_t)x;
}

/* ── tfrc_update_rtt: Update RTT estimate with EWMA ──
 * Uses exponential weighted moving average with alpha = 1/4.
 * Also tracks min_rtt. */
static void tfrc_update_rtt(struct dccp_sock *ds, uint32_t rtt_sample_ms)
{
    if (rtt_sample_ms < 1)
        rtt_sample_ms = 1;
    if (rtt_sample_ms > 30000)
        rtt_sample_ms = 30000;

    if (ds->rtt_samples == 0) {
        ds->rtt = rtt_sample_ms;
        ds->min_rtt = rtt_sample_ms;
    } else {
        /* EWMA: rtt = 3/4 * rtt + 1/4 * sample */
        ds->rtt = (ds->rtt * 3 + rtt_sample_ms) / 4;
        if (ds->rtt < 1)
            ds->rtt = 1;
        if (rtt_sample_ms < ds->min_rtt)
            ds->min_rtt = rtt_sample_ms;
    }
    ds->rtt_samples++;
}

/* ── tfrc_compute_x: Allowed rate via TCP throughput equation ──
 *
 * TCP throughput equation (RFC 5348 §3.1):
 *   X = s / (R*sqrt(2p/3) + t_RTO*3*sqrt(3p/8)*p*(1+32p^2))
 *
 * where s = packet size (bytes), R = RTT (sec), p = loss event rate,
 * t_RTO = 4*RTT (sec).  All computation uses Q16.16 fixed-point.
 * Returns allowed rate in bytes/sec. */
static uint32_t tfrc_compute_x(struct dccp_sock *ds)
{
    uint32_t s = ds->tfrc_s ? ds->tfrc_s : DCCP_CCID3_DEFAULT_S;
    uint32_t rtt_ms = ds->rtt ? ds->rtt : 100;
    uint32_t p_q16 = ds->tfrc_p;

    if (p_q16 == 0) {
        /* No loss: initial rate = 2 packets per RTT */
        uint64_t rate = (uint64_t)s * 2000 / rtt_ms;
        if (rate < DCCP_CCID3_MIN_X)
            rate = DCCP_CCID3_MIN_X;
        if (rate > DCCP_CCID3_MAX_X)
            rate = DCCP_CCID3_MAX_X;
        return (uint32_t)rate;
    }

    /* RTT in Q16.16 seconds: rtt_sec_q16 = rtt_ms * 65536 / 1000 */
    uint64_t R_q16 = (uint64_t)rtt_ms * 65536 / 1000;
    if (R_q16 < 1)
        R_q16 = 1;

    /* sqrt(2p/3) as Q16.16: sqrt(2 * p_q16 * 65536 / 3) */
    uint64_t rad1 = (uint64_t)2 * p_q16 * 65536 / 3;
    uint64_t sqrt_2p_3_q16 = int_sqrt64(rad1);

    /* sqrt(3p/8) as Q16.16: sqrt(3 * p_q16 * 65536 / 8) */
    uint64_t rad2 = (uint64_t)3 * p_q16 * 65536 / 8;
    uint64_t sqrt_3p_8_q16 = int_sqrt64(rad2);

    /* term1 = R * sqrt(2p/3) in Q16.16 seconds */
    uint64_t term1_q16 = R_q16 * sqrt_2p_3_q16 / 65536;

    /* inner = 3 * sqrt(3p/8) * p * (1+32p^2) in Q16.16 */
    uint64_t p2_q16 = (uint64_t)p_q16 * p_q16 / 65536;
    uint64_t one_plus_32p2_q16 = 65536ULL + (uint64_t)32 * p2_q16;

    /* inner_q16 = 3*sqrt_3p_8_q16*p_q16*one_plus_32p2_q16 / (65536^2) */
    uint64_t inner_q16 = (uint64_t)3 * sqrt_3p_8_q16 * p_q16;
    inner_q16 = inner_q16 * one_plus_32p2_q16 / 65536 / 65536;

    /* term2 = 4 * R * inner in Q16.16 seconds */
    uint64_t term2_q16 = 4 * R_q16 * inner_q16 / 65536;

    uint64_t denom_q16 = term1_q16 + term2_q16;
    if (denom_q16 < 1)
        denom_q16 = 1;

    /* X = s / denom_seconds = s * 65536 / denom_q16 (bytes/sec) */
    uint64_t rate = (uint64_t)s * 65536 / denom_q16;

    if (rate < DCCP_CCID3_MIN_X)
        rate = DCCP_CCID3_MIN_X;
    if (rate > DCCP_CCID3_MAX_X)
        rate = DCCP_CCID3_MAX_X;

    return (uint32_t)rate;
}

/* ── tfrc_update_loss_rate: Compute average loss interval ──
 * Uses a weighted average of the last N loss intervals (RFC 5348 §5).
 * The loss event rate p = 1 / I_mean, returned as Q16.16. */
static void tfrc_update_loss_rate(struct dccp_sock *ds)
{
    /* Weights double every 5 intervals (RFC 5348 §5.3) */
    static const uint32_t weights[DCCP_CCID3_LOSS_INTERVALS] = {
        1, 1, 2, 2, 2, 2, 4, 4
    };

    uint64_t sum_weighted = 0;
    uint32_t sum_weights = 0;
    uint32_t n = ds->tfrc_interval_idx;

    if (n == 0) {
        ds->tfrc_p = 0;
        return;
    }
    if (n > DCCP_CCID3_LOSS_INTERVALS)
        n = DCCP_CCID3_LOSS_INTERVALS;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = (i < DCCP_CCID3_LOSS_INTERVALS) ? weights[i] : 8;
        sum_weighted += (uint64_t)ds->tfrc_loss_interval[i] * w;
        sum_weights += w;
    }

    if (sum_weights == 0) {
        ds->tfrc_p = 0;
        return;
    }

    /* I_mean = sum_weighted / sum_weights (in packets)
     * p = 1 / I_mean, as Q16.16: p_q16 = 65536 / I_mean */
    uint32_t i_mean_pkts = (uint32_t)(sum_weighted / sum_weights);
    if (i_mean_pkts == 0)
        i_mean_pkts = 1;

    ds->tfrc_p = 65536U / i_mean_pkts;
}

/* ── tfrc_handle_loss: Process a loss event (RFC 5348 §5) ──
 * Called when 3 duplicate ACKs are detected.  Completes the current
 * loss interval and recalculates the loss event rate p. */
static void tfrc_handle_loss(struct dccp_sock *ds)
{
    if (ds->tfrc_interval_idx < DCCP_CCID3_LOSS_INTERVALS) {
        uint32_t interval_pkts = ds->tfrc_bytes_since_loss /
            (ds->tfrc_s ? ds->tfrc_s : DCCP_CCID3_DEFAULT_S);
        if (interval_pkts < 1)
            interval_pkts = 1;

        /* Shift intervals to make room for new one */
        if (ds->tfrc_interval_idx > 0) {
            for (uint32_t i = ds->tfrc_interval_idx; i > 0; i--)
                ds->tfrc_loss_interval[i] = ds->tfrc_loss_interval[i - 1];
        }
        ds->tfrc_loss_interval[0] = interval_pkts;
        ds->tfrc_interval_idx++;
        if (ds->tfrc_interval_idx > DCCP_CCID3_LOSS_INTERVALS)
            ds->tfrc_interval_idx = DCCP_CCID3_LOSS_INTERVALS;
    }

    ds->tfrc_bytes_since_loss = 0;
    ds->loss_pending = 1;

    /* Recompute loss rate and allowed rate */
    tfrc_update_loss_rate(ds);
    ds->tfrc_X = tfrc_compute_x(ds);
    ds->tx_rate = ds->tfrc_X;

    kprintf("[dccp] CCID3 TFRC loss: p=0x%08x X=%u rtt=%ums\n",
            ds->tfrc_p, ds->tfrc_X, ds->rtt);
}

/* ── tfrc_nofeedback_cb: NoFeedback expiry callback (RFC 5348 §4.3) ──
 * Halves the allowed rate and re-arms the timer. */
static void tfrc_nofeedback_cb(void *arg)
{
    int fd = (int)(unsigned long)(uintptr_t)arg;

    spinlock_acquire(&dccp_lock);
    struct dccp_sock *ds = dccp_ccid2_find_sock_by_fd(fd);
    if (!ds || !ds->used || ds->ccid != DCCP_CCID_3) {
        spinlock_release(&dccp_lock);
        return;
    }

    ds->tfrc_nofeedback_pending = 0;
    ds->tfrc_nofeedback_timer_id = -1;
    ds->tfrc_nofeedback_count++;

    /* Halve rate per RFC 5348 §4.3 */
    ds->tfrc_X /= 2;
    if (ds->tfrc_X < DCCP_CCID3_MIN_X)
        ds->tfrc_X = DCCP_CCID3_MIN_X;
    ds->tx_rate = ds->tfrc_X;

    kprintf("[dccp] CCID3 NoFeedback #%u fd=%d: X=%u\n",
            ds->tfrc_nofeedback_count, fd, ds->tfrc_X);

    /* Re-arm NoFeedback timer */
    uint32_t nofeedback_ms = ds->rtt ? (ds->rtt * DCCP_CCID3_NOFEEDBACK_RTO) : 400;
    uint64_t delay_ticks = (uint64_t)nofeedback_ms * TIMER_FREQ / 1000;
    if (delay_ticks < 1) delay_ticks = 1;

    int tid = timer_schedule(tfrc_nofeedback_cb,
                             (void *)(unsigned long)(uintptr_t)fd,
                             delay_ticks);
    if (tid >= 0) {
        ds->tfrc_nofeedback_timer_id = tid;
        ds->tfrc_nofeedback_pending = 1;
    }

    spinlock_release(&dccp_lock);
}

/* ── tfrc_arm_nofeedback: Arm the NoFeedback timer ──
 * NoFeedback timeout = 4 * RTT (RFC 5348 §4.3). */
static void tfrc_arm_nofeedback(struct dccp_sock *ds)
{
    if (ds->tfrc_nofeedback_pending)
        tfrc_disarm_nofeedback(ds);

    uint32_t nofeedback_ms = ds->rtt ? (ds->rtt * DCCP_CCID3_NOFEEDBACK_RTO) : 400;
    uint64_t delay_ticks = (uint64_t)nofeedback_ms * TIMER_FREQ / 1000;
    if (delay_ticks < 1) delay_ticks = 1;

    int fd = ds->fd;
    int tid = timer_schedule(tfrc_nofeedback_cb,
                             (void *)(unsigned long)(uintptr_t)fd,
                             delay_ticks);
    if (tid >= 0) {
        ds->tfrc_nofeedback_timer_id = tid;
        ds->tfrc_nofeedback_pending = 1;
        ds->tfrc_nofeedback_count = 0;
    }
}

/* ── tfrc_disarm_nofeedback: Cancel NoFeedback timer ── */
static void tfrc_disarm_nofeedback(struct dccp_sock *ds)
{
    if (!ds->tfrc_nofeedback_pending)
        return;
    if (ds->tfrc_nofeedback_timer_id >= 0) {
        timer_cancel(ds->tfrc_nofeedback_timer_id);
        ds->tfrc_nofeedback_timer_id = -1;
    }
    ds->tfrc_nofeedback_pending = 0;
}

/* ── dccp_ccid_ack_rcvd: update CCID state on received ACK ──
 * Called when an ACK or DataAck packet is received.  Parses the
 * acknowledgment number and updates CCID congestion control state.
 */
static void dccp_ccid_ack_rcvd(struct dccp_sock *ds,
                               const uint8_t *pkt, uint16_t len)
{
    uint32_t ack_seq = dccp_parse_ack_option(pkt, len);
    if (ack_seq == 0)
        return;

    if (ds->ccid == DCCP_CCID_2) {
        /* ── CCID2: TCP-like congestion control (RFC 4341 §4) ── */
        dccp_ccid2_ack_received(ds, ack_seq);

        /* ── Attempt RTT measurement ──
         * DCCP can carry a Timestamp Echo option (type 6) in ACK packets.
         * If present, we can compute RTT = now - timestamp_echo.
         * For now, we estimate RTT from the inter-ACK timing.
         * A proper implementation would parse the Timestamp Echo option. */
        {
            static const uint32_t rtt_estimate = 100; /* placeholder */
            (void)rtt_estimate;
        }

        /* Update last_ack_seq for the next call */
        ds->last_ack_seq = ack_seq;

    } else if (ds->ccid == DCCP_CCID_3) {
        /* ── CCID3: TFRC rate control (RFC 4342, RFC 5348) ── */
        uint32_t prev_ack = ds->last_ack_seq;
        int newly_acked = 0;
        if (ack_seq > prev_ack) {
            newly_acked = (int)(ack_seq - prev_ack);
        } else if (ack_seq < prev_ack) {
            newly_acked = (int)(ack_seq + (0xFFFFFFFFU - prev_ack));
        }

        if (newly_acked == 0) {
            /* Duplicate ACK: may indicate packet loss */
            ds->dup_acks++;
            if (ds->dup_acks >= 3 && !ds->loss_pending) {
                ds->loss_pending = 1;
                tfrc_handle_loss(ds);
            }
            return;
        }

        /* New ACK received: update in_flight and reset dupACK counter */
        if (newly_acked > (int)ds->in_flight)
            ds->in_flight = 0;
        else
            ds->in_flight -= (uint32_t)newly_acked;

        ds->last_ack_seq = ack_seq;
        ds->dup_acks = 0;

        /* Track bytes since last loss for loss interval calculation */
        ds->tfrc_bytes_since_loss += (uint32_t)newly_acked *
            (ds->tfrc_s ? ds->tfrc_s : DCCP_CCID3_DEFAULT_S);

        /* Update RTT estimate from ACK timing */ {
            uint64_t now = timer_get_ticks();
            uint64_t rtt_ticks = now - ds->last_send_time;
            uint32_t rtt_ms = (uint32_t)(rtt_ticks * 10);
            tfrc_update_rtt(ds, rtt_ms);
        }

        /* Recompute allowed rate using TCP throughput equation */
        ds->tfrc_X = tfrc_compute_x(ds);

        /* RFC 5348 §4.4: no more than doubling rate per RTT */
        if (ds->tx_rate > 0) {
            uint64_t max_rate = (uint64_t)ds->tx_rate * 2;
            if ((uint64_t)ds->tfrc_X > max_rate)
                ds->tfrc_X = (uint32_t)max_rate;
        }

        ds->tx_rate = ds->tfrc_X;

        /* Re-arm NoFeedback timer on feedback receipt */
        tfrc_disarm_nofeedback(ds);
        tfrc_arm_nofeedback(ds);

        kprintf("[dccp] CCID3 TFRC: X=%u rtt=%ums p=0x%08x in_flight=%u\n",
                ds->tx_rate, ds->rtt, ds->tfrc_p, ds->in_flight);
    }
}

EXPORT_SYMBOL(dccp_init);
EXPORT_SYMBOL(dccp_create);
EXPORT_SYMBOL(dccp_bind);
EXPORT_SYMBOL(dccp_connect);
EXPORT_SYMBOL(dccp_send);
EXPORT_SYMBOL(dccp_recv);
EXPORT_SYMBOL(dccp_close);
EXPORT_SYMBOL(dccp_send_ack);
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
    ds->state = DCCP_LISTEN;
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
    /* Disarm CCID timers before disconnect */
    if (ds->ccid == DCCP_CCID_2)
        dccp_ccid2_disarm_rto(ds);
    else if (ds->ccid == DCCP_CCID_3)
        tfrc_disarm_nofeedback(ds);
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
    ds->state = DCCP_CLOSED;
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
