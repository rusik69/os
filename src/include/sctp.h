#ifndef SCTP_H
#define SCTP_H

#include "types.h"

/* SCTP (Stream Control Transmission Protocol) — RFC 4960
 * AF_SCTP = 30 (Linux-compatible)
 */

#define AF_SCTP             30

/* SCTP socket types */
#define SCTP_SEQPACKET      5   /* Sequenced-packet socket */
#define SCTP_STREAM         6   /* Stream socket */

/* SCTP protocol numbers for IPPROTO */
#define IPPROTO_SCTP        132

/* SCTP association state */
enum sctp_state {
    SCTP_STATE_CLOSED       = 0,
    SCTP_STATE_COOKIE_WAIT  = 1,
    SCTP_STATE_COOKIE_ECHOED = 2,
    SCTP_STATE_ESTABLISHED  = 3,
    SCTP_STATE_SHUTDOWN_PENDING = 4,
    SCTP_STATE_SHUTDOWN_SENT = 5,
    SCTP_STATE_SHUTDOWN_RECEIVED = 6,
    SCTP_STATE_SHUTDOWN_ACK_SENT = 7,
};

/* SCTP common header */
struct sctp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t vtag;
    uint32_t checksum;
} __attribute__((packed));

/* SCTP chunk header */
struct sctp_chunk {
    uint8_t  type;
    uint8_t  flags;
    uint16_t length;
    /* data follows */
} __attribute__((packed));

/* Chunk types */
#define SCTP_DATA            0
#define SCTP_INIT            1
#define SCTP_INIT_ACK        2
#define SCTP_SACK            3
#define SCTP_HEARTBEAT       4
#define SCTP_HEARTBEAT_ACK   5
#define SCTP_ABORT           6
#define SCTP_SHUTDOWN        7
#define SCTP_SHUTDOWN_ACK    8
#define SCTP_ERROR           9
#define SCTP_COOKIE_ECHO     10
#define SCTP_COOKIE_ACK      11
#define SCTP_ECNE            12
#define SCTP_CWR             13
#define SCTP_SHUTDOWN_COMPLETE 14

/* SCTP association */
#define SCTP_MAX_STREAMS     16
#define SCTP_MAX_ASSOCS      8

struct sctp_stream {
    uint16_t id;
    uint16_t in_seq;
    uint16_t out_seq;
};

struct sctp_assoc {
    int         used;
    uint32_t    local_tag;
    uint32_t    peer_tag;
    uint16_t    local_port;
    uint16_t    peer_port;
    uint32_t    peer_ip;
    enum sctp_state state;
    struct sctp_stream in_streams[SCTP_MAX_STREAMS];
    struct sctp_stream out_streams[SCTP_MAX_STREAMS];
    uint8_t     num_in_streams;
    uint8_t     num_out_streams;
    /* Receive buffer */
    uint8_t     rcvbuf[65536];
    uint16_t    rcvlen;
    /* Transmit buffer */
    uint8_t     sndbuf[65536];
    uint16_t    sndlen;
    /* Association statistics */
    uint64_t    rx_packets;
    uint64_t    tx_packets;
    uint32_t    rtt;
};

/* API */
void sctp_init(void);
int  sctp_create(int fd, int type);
int  sctp_bind(int fd, uint16_t port);
int  sctp_connect(int fd, uint32_t ip, uint16_t port);
int  sctp_send(int fd, const void *data, uint16_t len, uint16_t stream_id);
int  sctp_recv(int fd, void *buf, uint16_t maxlen, uint16_t *stream_id);
void sctp_close(int fd);
int  sctp_is_valid_fd(int fd);

/* Called from IP layer when protocol=132 */
void handle_sctp(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *payload, uint16_t len);

#endif /* SCTP_H */
