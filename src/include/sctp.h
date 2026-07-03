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
#define SCTP_COOKIE_SECRET_SIZE 32
#define SCTP_MAX_COOKIE_SIZE 128

/* State cookie (RFC 4960 §5.1.3) — contains parameters needed to
 * recreate TCB when COOKIE-ECHO arrives back */
struct sctp_cookie {
    uint32_t    local_tag;      /* Responder's tag */
    uint32_t    peer_tag;       /* Initiator's tag */
    uint32_t    local_tsn;      /* Responder's initial TSN */
    uint32_t    peer_tsn;       /* Initiator's initial TSN */
    uint16_t    local_port;     /* Responder's port */
    uint16_t    peer_port;      /* Initiator's port */
    uint32_t    peer_ip;        /* Initiator's IP */
    uint16_t    num_in_streams;
    uint16_t    num_out_streams;
    uint32_t    timestamp;      /* Creation time (ticks) */
    uint32_t    crc;            /* Integrity check */
} __attribute__((packed));

struct sctp_stream {
    uint16_t id;
    uint16_t in_seq;
    uint16_t out_seq;
};

/* DATA chunk payload header (RFC 4960 §3.3.1) */
struct sctp_data_hdr {
    struct sctp_chunk hdr;
    uint32_t          tsn;
    uint16_t          stream_id;
    uint16_t          stream_seq;
    uint32_t          ppid;
    /* data follows */
} __attribute__((packed));

/* SACK chunk (RFC 4960 §3.3.4) */
struct sctp_sack_hdr {
    struct sctp_chunk hdr;
    uint32_t          cum_tsn_ack;
    uint32_t          a_rwnd;
    uint16_t          num_gap_blocks;
    uint16_t          num_dup_tsns;
    /* gap_ack_blocks[] and dup_tsn[] follow */
} __attribute__((packed));

/* Single gap ack block (start/end offsets from cum_tsn_ack) */
struct sctp_gap_block {
    uint16_t start;
    uint16_t end;
} __attribute__((packed));

/* DATA chunk flags */
#define SCTP_DATA_UNORDERED  0x01  /* U bit */
#define SCTP_DATA_BEG        0x02  /* B bit — beginning fragment */
#define SCTP_DATA_END        0x04  /* E bit — ending fragment */
#define SCTP_DATA_LAST_FRAG  (SCTP_DATA_BEG | SCTP_DATA_END) /* complete unfragmented */

#define SCTP_MAX_GAP_BLOCKS  16
#define SCTP_MAX_DUP_TSNS   16

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
    /* TSN tracking */
    uint32_t    initial_tsn;
    uint32_t    next_tsn;
    uint32_t    cum_tsn_ack;
    uint32_t    last_rcvd_tsn;
    uint32_t    rwnd;
    uint32_t    peer_rwnd;
    /* SACK gap tracking */
    uint32_t    gap_ack_start[SCTP_MAX_GAP_BLOCKS];
    uint32_t    gap_ack_end[SCTP_MAX_GAP_BLOCKS];
    uint8_t     num_gap_blocks;
    uint32_t    dup_tsns[SCTP_MAX_DUP_TSNS];
    uint8_t     num_dup_tsns;
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

/* State machine: called from handle_sctp for individual chunk processing */
int  sctp_sm_handle_init(struct sctp_assoc *a, uint32_t src_ip,
                         const struct sctp_header *sh,
                         const struct sctp_chunk *chunk, uint16_t chunk_len);
int  sctp_sm_handle_init_ack(struct sctp_assoc *a, uint32_t src_ip,
                             const struct sctp_header *sh,
                             const struct sctp_chunk *chunk, uint16_t chunk_len);
int  sctp_sm_handle_cookie_echo(struct sctp_assoc *a, uint32_t src_ip,
                                const struct sctp_header *sh,
                                const struct sctp_chunk *chunk, uint16_t chunk_len);
int  sctp_sm_handle_cookie_ack(struct sctp_assoc *a,
                               const struct sctp_header *sh);
int  sctp_sm_send_init_ack(struct sctp_assoc *a, uint32_t peer_ip,
                           uint16_t peer_port, uint32_t peer_tag,
                           uint32_t peer_tsn, uint8_t num_in,
                           uint8_t num_out);

/* Cookie management */
int  sctp_cookie_generate(const struct sctp_cookie *cookie_in,
                          uint8_t *out_cookie, uint16_t *out_len);
int  sctp_cookie_validate(const uint8_t *cookie_data, uint16_t cookie_len,
                          struct sctp_cookie *cookie_out);

/* TSN management */
uint32_t sctp_tsn_alloc(struct sctp_assoc *a);
int      sctp_tsn_rcv_data(struct sctp_assoc *a, uint32_t src_ip,
                           uint32_t peer_tag,
                           const struct sctp_data_hdr *dh,
                           uint16_t chunk_len);
int      sctp_tsn_build_sack(struct sctp_assoc *a, uint32_t peer_ip,
                             uint16_t peer_port, uint32_t peer_tag,
                             uint16_t local_port);
int      sctp_tsn_process_sack(struct sctp_assoc *a,
                               const struct sctp_sack_hdr *sh,
                               uint16_t chunk_len);

#endif /* SCTP_H */
