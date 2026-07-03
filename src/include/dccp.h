#ifndef DCCP_H
#define DCCP_H

#include "types.h"

/* DCCP (Datagram Congestion Control Protocol) — RFC 4340
 * AF_DCCP = 33 (Linux-compatible)
 */

#define AF_DCCP             33
#define IPPROTO_DCCP        33

/* DCCP socket types */
#define DCCP_SOCK_TYPE      6   /* Connection-oriented datagram */

/* DCCP packet types */
#define DCCP_PKT_REQUEST    0
#define DCCP_PKT_RESPONSE   1
#define DCCP_PKT_DATA       2
#define DCCP_PKT_ACK        3
#define DCCP_PKT_DATAACK    4
#define DCCP_PKT_CLOSEREQ   5
#define DCCP_PKT_CLOSE      6
#define DCCP_PKT_RESET      7
#define DCCP_PKT_SYNC       8
#define DCCP_PKT_SYNCACK    9

/* DCCP header (generic) */
struct dccp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  data_offset;   /* upper 4 bits = data offset in 32-bit words */
    uint8_t  ccval_cscov;   /* CCVal (4) | CsCov (4) */
    uint16_t checksum;
    uint8_t  type_reset;    /* type (4) | X (1) | reserved (3) */
    uint8_t  seq_high;      /* high 8 bits of sequence number (if X=1) */
    uint32_t seq_low;       /* low 32 bits of sequence number */
} __attribute__((packed));

/* DCCP congestion control IDs (CCID) */
#define DCCP_CCID_DISABLED  0
#define DCCP_CCID_2         2   /* TCP-like congestion control (RFC 4341) */
#define DCCP_CCID_3         3   /* TFRC (RFC 4342) */

/* DCCP states (RFC 4340 §8) */
#define DCCP_CLOSED          0
#define DCCP_LISTEN          1
#define DCCP_REQUEST         2
#define DCCP_RESPOND         3
#define DCCP_ESTABLISHED     4
#define DCCP_CLOSING         5

/* Feature types for feature negotiation (RFC 4340 §6.4) */
#define DCCP_FEAT_CCID              1
#define DCCP_FEAT_SERVICE_CODE      2
#define DCCP_FEAT_ACK_RATIO         3
#define DCCP_FEAT_SEND_ACK_VEC      4
#define DCCP_FEAT_SEND_NDP          5
#define DCCP_FEAT_MIN_CSUM_COV      6
#define DCCP_FEAT_DATA_CSUM         7

/* Feature negotiation option types (RFC 4340 §6.1) */
#define DCCP_OPT_CHANGE_L   32
#define DCCP_OPT_CONFIRM_L  34
#define DCCP_OPT_CHANGE_R   33
#define DCCP_OPT_CONFIRM_R  35

/* Service code option (RFC 4340 §15.4) */
#define DCCP_OPT_SERVICE_CODE 15

/* Acknowledgement number option (RFC 4340 §11.3) */
#define DCCP_OPT_ACK_NUM    14

/* Option padding */
#define DCCP_OPT_PADDING    0

/* ── CCID2 constants (RFC 4341) ──────────────────────────────── */
#define DCCP_CCID2_INIT_CWND     2       /* Initial congestion window (packets) */
#define DCCP_CCID2_INIT_SSTHRESH 16      /* Initial slow start threshold */
#define DCCP_CCID2_INIT_RTO      3000    /* Initial RTO: 3 seconds (ms) */
#define DCCP_CCID2_MIN_RTO       200     /* Minimum RTO: 200 ms */
#define DCCP_CCID2_MAX_RTO       60000   /* Maximum RTO: 60 seconds */
#define DCCP_CCID2_INIT_ACK_RATIO 2     /* Ack Ratio initial value */
/* Scaling factors for SRTT/RTTVAR (RFC 6298) */
#define DCCP_CCID2_RTO_ALPHA     1       /* 1/8 for SRTT update */
#define DCCP_CCID2_RTO_BETA      1       /* 1/4 for RTTVAR update */

/* ── CCID3: TFRC constants (RFC 5348, RFC 4342) ────────────────── */
#define DCCP_CCID3_DEFAULT_S        1500    /* Default packet size (bytes) */
#define DCCP_CCID3_INIT_X           20000   /* Initial allowed rate (bytes/sec) */
#define DCCP_CCID3_MIN_X            240     /* Min rate ~1 pkt per 64 sec */
#define DCCP_CCID3_MAX_X            100000000 /* Cap at 100 MB/s */
#define DCCP_CCID3_LOSS_INTERVALS   8       /* Loss interval history length */
#define DCCP_CCID3_NOFEEDBACK_RTO   4       /* NoFeedback timeout = 4 * RTT */

/* DCCP socket state */
struct dccp_sock {
    int         used;
    int         fd;
    uint16_t    local_port;
    uint16_t    peer_port;
    uint32_t    peer_ip;
    int         connected;
    int         ccid;           /* Congestion control ID */
    uint32_t    seq;            /* Current sequence number */
    uint32_t    ack_seq;        /* Last ACKed sequence number */
    /* CCID state */
    uint32_t    in_flight;      /* Packets sent but not yet ACKed */
    uint32_t    last_ack_seq;   /* Last sequence number ACKed by peer */
    uint64_t    last_send_time; /* timer_get_ticks() of last data send */
    uint32_t    dup_acks;       /* Duplicate ACK count (CCID2 fast retransmit) */
    int         loss_pending;   /* Loss event flag */
    uint32_t    rtt_samples;    /* RTT sample count (CCID3) */
    uint32_t    min_rtt;        /* Minimum observed RTT in ms (CCID3) */
    /* TFRC state (CCID 3) */
    uint32_t    tx_rate;        /* Transmit rate (bytes/sec) */
    uint32_t    rtt;            /* Round-trip time estimate (ms) */
    /* Congestion window for CCID 2 */
    uint32_t    cwnd;
    uint32_t    ssthresh;
    /* Receive buffer */
    uint8_t     rcvbuf[4096];
    uint16_t    rcvlen;
    /* Socket options / state */
    uint32_t    service_code;   /* DCCP service code */
    int         state;          /* DCCP_CLOSED, DCCP_LISTEN, DCCP_REQUEST, ... */
    int         backlog;        /* Listen backlog */
    /* Connection setup (RFC 4340 §5.1) */
    uint32_t    iss;            /* Initial send sequence number */
    /* ── CCID2: TCP-like congestion control (RFC 4341) ─────────────── */
    /* RTT estimation (RFC 6298) */
    uint32_t    srtt;           /* Smoothed RTT (in ms, scaled by 8) */
    uint32_t    rttvar;         /* RTT variance (in ms, scaled by 4) */
    uint32_t    rto;            /* Retransmission timeout (in ms) */
    /* Retransmission timer */
    int         rto_timer_id;   /* timer_schedule() ID, -1 if not pending */
    uint64_t    rto_expire_tick;/* tick at which RTO fires */
    uint8_t     rto_pending;    /* 1 = RTO timer is armed */
    /* Fast recovery state */
    uint8_t     recover;        /* 1 = in fast recovery (RFC 6582) */
    uint32_t    recover_seq;    /* Sequence # when recovery started */
    uint32_t    pipe;           /* Estimated packets in pipe (RFC 6675) */
    /* Ack Ratio (RFC 4341 §9) — DCCP-specific: ACKs per data window */
    uint16_t    ack_ratio;      /* Send one ACK per ack_ratio data packets */
    uint16_t    data_pkts_since_ack; /* Count of data packets since last ACK */
    /* Data sequence tracking for retransmission */
    uint32_t    snd_una;        /* Oldest unacknowledged sequence number */
    uint32_t    snd_nxt;        /* Next sequence number to send */
    uint32_t    highest_sent;   /* Highest sequence number sent */
    /* Loss statistics */
    uint32_t    retransmits;    /* Total retransmissions */
    /* ── CCID3: TFRC rate control (RFC 5348) ─────────────────────── */
    uint32_t    tfrc_p;                     /* Loss event rate (Q16.16) */
    uint32_t    tfrc_s;                     /* Packet size (bytes) */
    uint32_t    tfrc_loss_interval[DCCP_CCID3_LOSS_INTERVALS]; /* Loss intervals */
    uint32_t    tfrc_bytes_since_loss;      /* Bytes sent since last loss */
    uint32_t    tfrc_interval_idx;          /* Current loss interval index */
    uint8_t     tfrc_nofeedback_pending;    /* NoFeedback timer armed */
    int         tfrc_nofeedback_timer_id;   /* NoFeedback timer ID */
    uint64_t    tfrc_last_feedback;         /* Last feedback time (ticks) */
    uint32_t    tfrc_nofeedback_count;      /* Consecutive NoFeedback timeouts */
    uint32_t    tfrc_X;                     /* Computed rate from equation (bytes/sec) */
};

/* DCCP options */
#define DCCP_OPT_MANDATORY       0
#define DCCP_OPT_DATA_OFFSET     1   /* deprecated */
#define DCCP_OPT_NDP_COUNT       2
#define DCCP_OPT_ACK_VECTOR0     3
#define DCCP_OPT_ACK_VECTOR1     4
#define DCCP_OPT_TIMESTAMP       5
#define DCCP_OPT_TIMESTAMP_ECHO  6
#define DCCP_OPT_ELAPSED_TIME    7
#define DCCP_OPT_DATA_CHECKSUM   8
#define DCCP_OPT_CCID_SPECIFIC   32

/* API */
void dccp_init(void);
int  dccp_create(int fd, int type);
int  dccp_bind(int fd, uint16_t port);
int  dccp_connect(int fd, uint32_t ip, uint16_t port);
int  dccp_send(int fd, const void *data, uint16_t len);
int  dccp_recv(int fd, void *buf, uint16_t maxlen);
void dccp_close(int fd);
int  dccp_is_valid_fd(int fd);
int  dccp_send_ack(int fd);

/* Called from IP layer */
void handle_dccp(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *payload, uint16_t len);

#endif /* DCCP_H */
