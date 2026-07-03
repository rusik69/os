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
