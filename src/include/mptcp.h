#ifndef MPTCP_H
#define MPTCP_H

#include "types.h"
#include "spinlock.h"

/* Multipath TCP (MPTCP) — RFC 8684
 * AF_MPTCP pseudo-family for subflow management.
 * Actual subflows use AF_INET, MPTCP is a meta-protocol.
 */

#define AF_MPTCP            48  /* Linux-compatible */

/* MPTCP option kind */
#define TCPOPT_MPTCP        30

/* MPTCP option subtypes */
#define MPTCP_CAPABLE       0   /* MP_CAPABLE handshake */
#define MPTCP_JOIN          1   /* MP_JOIN subflow addition */
#define MPTCP_DSS           2   /* Data Sequence Signal */
#define MPTCP_ADD_ADDR      3   /* ADD_ADDR advertisement */
#define MPTCP_REMOVE_ADDR   4   /* REMOVE_ADDR */
#define MPTCP_PRIO          5   /* MP_PRIO backup/active */
#define MPTCP_FAIL          6   /* MP_FAIL fallback */
#define MPTCP_FASTCLOSE     7   /* MP_FASTCLOSE */

/* MP_FASTCLOSE option length (RFC 8684 §3.6)
 * Format: kind(1) + len(1) + subtype+flags(1) + reserved(1) + rcv_key(8) = 12
 * The V flag (0x01) indicates the key is included for verification. */
#define MPTCP_FASTCLOSE_LEN      12  /* total option bytes */
#define MPTCP_FASTCLOSE_FLAG_V   0x01  /* V flag: key verification present */

/* MPTCP flags */
#define MPTCP_CAPABLE_HMAC  0x01  /* MP_CAPABLE includes HMAC */

/* MP_CAPABLE option lengths (RFC 8684 §3.1) */
#define MPTCP_CAPABLE_SYN_LEN     12  /* kind(1) + len(1) + sub/flags(1) + resv(1) + key(8) */
#define MPTCP_CAPABLE_ACK_LEN    24  /* kind(1) + len(1) + sub/flags(1) + resv(1) + snd_key(8) + rcv_key(8) + hmac(8) */

/* MP_JOIN option lengths (RFC 8684 §3.2) — per RFC errata, ACK length is 12 */
#define MPTCP_JOIN_SYN_LEN      12  /* kind(1)+len(1)+sub/flags(1)+addr_id(1)+token(4)+nonce(4) */
#define MPTCP_JOIN_SYNACK_LEN   16  /* kind(1)+len(1)+sub/flags(1)+addr_id(1)+nonce(4)+hmac(8) */
#define MPTCP_JOIN_ACK_LEN      12  /* kind(1)+len(1)+sub/flags(1)+resv(1)+hmac(8) */

/* MP_JOIN flags (lower nibble of byte 2) */
#define MPTCP_JOIN_FLAG_BACKUP  0x01  /* B bit: backup subflow */

/* ADD_ADDR / REMOVE_ADDR option lengths (RFC 8684 §3.4) */
#define MPTCP_ADD_ADDR4_LEN       10  /* IPv4 ADD_ADDR without port */
#define MPTCP_ADD_ADDR4_LEN_PORT  12  /* IPv4 ADD_ADDR with port */
#define MPTCP_REMOVE_ADDR_MIN_LEN  4  /* Minimum REMOVE_ADDR (1 addr_id) */

/* DSS (Data Sequence Signal) flag bits — byte 3 of DSS option (RFC 8684 §3.3) */
#define MPTCP_DSS_FLAG_A     0x80  /* Data ACK present */
#define MPTCP_DSS_FLAG_A8    0x40  /* Data ACK is 8 bytes (vs 4 bytes when A=1 and a=0) */
#define MPTCP_DSS_FLAG_M     0x20  /* Data Sequence Number (DSN) present */
#define MPTCP_DSS_FLAG_M4    0x10  /* DSN is 4 bytes (vs 8 when M=1 and m=0) */
#define MPTCP_DSS_FLAG_F     0x08  /* Subflow Sequence Number (SSN) present */
#define MPTCP_DSS_FLAG_F2    0x04  /* SSN is 2 bytes (vs 4 when F=1 and f=0) */
#define MPTCP_DSS_FLAG_C     0x01  /* Checksum present (2 bytes at end of option) */

/* DSS option length variants (RFC 8684 §3.3)
 * The option header is always 4 bytes: kind(1), len(1), subtype+flags(1), data_flags(1) */
#define MPTCP_DSS_MIN_LEN           4   /* Minimal: just header, no data */
#define MPTCP_DSS_ACK4_LEN          8   /* header(4) + data_ack(4) */
#define MPTCP_DSS_ACK8_LEN         12   /* header(4) + data_ack(8) */
#define MPTCP_DSS_DATA8_SSN4_LEN   18   /* header(4) + DSN(8) + SSN(4) + data_len(2) */
#define MPTCP_DSS_DATA4_SSN2_LEN   11   /* header(4) + DSN(4) + SSN(2) + data_len(1) */
#define MPTCP_DSS_CKSUM_LEN         2   /* Checksum always 2 bytes */

/* Address advertisement table size */
#define MPTCP_MAX_ADDRS           8

/* Address entry flags */
#define MPTCP_ADDR_FLAG_ECHO      0x01  /* E flag — echo of an ADD_ADDR received */
#define MPTCP_ADDR_FLAG_IPV4      0x02  /* Address is IPv4 */

/* Maximum subflows per MPTCP connection */
#define MPTCP_MAX_SUBFLOWS  4

/* MPTCP subflow state */
struct mptcp_subflow {
    int         used;
    int         conn_id;        /* TCP connection ID */
    uint32_t    token;          /* Connection token */
    uint32_t    snd_isn;        /* Initial sequence number for this subflow */
    uint32_t    rcv_isn;        /* Receiver's initial sequence number */
    uint8_t     key[8];         /* 64-bit key */
    uint8_t     backup;         /* 1 = backup subflow */
    /* MP_JOIN handshake state */
    uint32_t    join_nonce;     /* Peer's nonce during MP_JOIN handshake */
    uint32_t    join_local_nonce; /* Our nonce during MP_JOIN handshake */
    uint8_t     join_id;        /* Peer's address ID during MP_JOIN */
    uint8_t     join_local_id;  /* Our address ID during MP_JOIN */
    uint8_t     join_hmac[8];   /* Saved HMAC for MP_JOIN validation */
    /* DSS mapping state (RFC 8684 §3.3) — tracks current mapping between
     * subflow sequence numbers and the MPTCP data-level sequence number. */
    uint64_t    dss_data_seq;    /* Data-level sequence number of mapping start */
    uint32_t    dss_subflow_seq; /* Subflow-level seq number of mapping start */
    uint16_t    dss_mapped_len;  /* Bytes covered by current DSS mapping */
};

/* Address advertisement entry — tracks ADD_ADDR state per connection */
struct mptcp_addr_entry {
    int     used;
    uint8_t addr_id;       /* Address ID (unique within connection) */
    uint32_t addr;          /* IPv4 address (network byte order) */
    uint16_t port;          /* Port (0 = use existing subflow port) */
    uint8_t  flags;         /* MPTCP_ADDR_FLAG_* */
};

/* MPTCP connection — aggregates multiple subflows */
struct mptcp_conn {
    int         used;
    uint32_t    token;               /* Locally-generated token */
    uint32_t    peer_token;          /* Peer's token */
    uint8_t     snd_key[8];          /* Local 64-bit key */
    uint8_t     rcv_key[8];          /* Remote 64-bit key */
    uint64_t    snd_data_seq;        /* Data-level sending sequence number */
    uint64_t    snd_data_ack;        /* Highest data-level ACK received from peer */
    uint64_t    rcv_data_seq;        /* Data-level receiving sequence number */
    uint64_t    rcv_data_ack;        /* Highest data-level ACK we have sent */
    uint8_t     num_subflows;
    struct mptcp_subflow subflows[MPTCP_MAX_SUBFLOWS];
    /* Receive reassembly buffer */
    uint8_t     rcvbuf[65536];
    uint16_t    rcvlen;
    /* State */
    int         established;
    /* Address advertisement table (ADD_ADDR/REMOVE_ADDR) */
    uint8_t     num_addrs;
    struct mptcp_addr_entry addrs[MPTCP_MAX_ADDRS];
    /* Path scheduler state — D204 task 6 */
    uint8_t     sched_algo;         /* Current scheduler algorithm (MPTCP_SCHED_*) */
    uint8_t     last_selected;      /* Index of last subflow selected for RR fallback */
    uint32_t    sched_epoch;        /* Scheduler epoch (increments on subflow add/remove) */
};

/* ── MPTCP sysctl knobs ──────────────────────────────────────────── */
/* Global MPTCP enable/disable flag.  When 0, all MPTCP operations
 * (create, add_subflow, send, etc.) return -EOPNOTSUPP.
 * Read/write via sysctl "mptcp_enabled" under /proc/sys/kernel/. */
extern int mptcp_enabled;

/* API */
void mptcp_init(void);
int  mptcp_create(void);
int  mptcp_add_subflow(uint32_t token, int conn_id, uint32_t addr, uint16_t port);
int  mptcp_remove_subflow(uint32_t token, uint32_t addr, uint16_t port);
int  mptcp_send(uint32_t token, const void *data, uint32_t len);
int  mptcp_recv(uint32_t token, void *buf, uint32_t maxlen);
void mptcp_close(uint32_t token);
int  mptcp_get_token(void);

/* Associate a TCP connection with an MPTCP token (copies key from mptcp_conn) */
int  mptcp_associate(int conn_id, uint32_t token);

/* Build MP_CAPABLE option for SYN (client side) */
int  mptcp_build_capable_syn(uint8_t *buf, uint16_t *len, const uint8_t snd_key[8]);
/* Build MP_CAPABLE option for SYN+ACK (server side) */
int  mptcp_build_capable_synack(uint8_t *buf, uint16_t *len, const uint8_t snd_key[8]);
/* Build MP_CAPABLE option for 3rd ACK (both keys + HMAC) */
int  mptcp_build_capable_ack(uint8_t *buf, uint16_t *len,
                              const uint8_t snd_key[8], const uint8_t rcv_key[8]);
/* Parse MP_CAPABLE option, extract peer's key.  Returns 0 on success. */
int  mptcp_parse_capable(const uint8_t *opt, uint16_t optlen, uint8_t peer_key[8]);

/* TCP option handlers — called from TCP stack */
int  mptcp_handle_capable(int conn_id, const uint8_t *opt, uint16_t optlen);
int  mptcp_handle_join(int conn_id, const uint8_t *opt, uint16_t optlen);
int mptcp_handle_dss(int conn_id, const uint8_t *opt, uint16_t optlen,
                       uint32_t seq, uint32_t ack,
                       const void *tcp_data, uint16_t tcp_data_len);

/* Address advertisement (ADD_ADDR / REMOVE_ADDR) */
int  mptcp_has_addr(uint32_t token, uint32_t addr);
int  mptcp_find_free_addr(uint32_t token, uint8_t *addr_id_out);

/* Build ADD_ADDR option (IPv4) for TCP options */
int  mptcp_build_add_addr_v4(uint8_t *buf, uint16_t *len,
                              uint8_t addr_id, uint32_t addr,
                              uint16_t port, uint8_t flags);

/* Build REMOVE_ADDR option for TCP options */
int  mptcp_build_remove_addr(uint8_t *buf, uint16_t *len,
                              uint8_t addr_id);

/* Parse received ADD_ADDR option */
int  mptcp_parse_add_addr(const uint8_t *opt, uint16_t optlen,
                           uint8_t *addr_id_out, uint32_t *addr_out,
                           uint16_t *port_out, uint8_t *flags_out);

/* Parse received REMOVE_ADDR option */
int  mptcp_parse_remove_addr(const uint8_t *opt, uint16_t optlen,
                              uint8_t *addr_id_out);

/* Handle received ADD_ADDR on a connection */
int  mptcp_handle_add_addr(int conn_id, const uint8_t *opt, uint16_t optlen);

/* Handle received REMOVE_ADDR on a connection */
int  mptcp_handle_remove_addr(int conn_id, const uint8_t *opt, uint16_t optlen);

/* ── MP_JOIN subflow setup (RFC 8684 §3.2) ─────────────────────── */

/* ── DSS (Data Sequence Signal) — RFC 8684 §3.3 ───────────────── */

/* ── MPTCP Data Acknowledgement (RFC 8684 §3.3) ──────────────── */

/* Get the current receive-side Data ACK value for the connection.
 * This is the highest data-level byte that has been received in order.
 * Returns 0 on success, negative errno on failure. */
int  mptcp_get_data_ack(uint32_t token, uint64_t *ack_out);

/* Advance the receive-side Data ACK after consuming data.
 * ack is the new data-level ACK position (should be >= the current value).
 * Returns 0 on success, negative errno on failure. */
int  mptcp_update_data_ack(uint32_t token, uint64_t ack);

/* Send a pure MPTCP Data ACK on the specified subflow.
 * Builds a TCP pure ACK segment with the DSS option carrying only the
 * Data ACK field (no Data Sequence Number, no Subflow Sequence Number).
 * conn_id identifies the TCP subflow connection to send on.
 * Returns 0 on success, negative errno on failure. */
int  mptcp_send_data_ack(uint32_t token, int conn_id);

/* Build a DSS option for a data segment.
 * On entry, *len is the buffer capacity; on exit, *len is bytes written.
 * Set data_ack_valid=0 to omit the Data ACK field.
 * Set data_seq_valid=0 to omit the Data Sequence Number field.
 * Set subflow_seq_valid=0 to omit the Subflow Sequence Number field.
 * All three can be combined as needed (common: data_ack only for pure ACKs,
 * or data_seq+subflow_seq+data_len for data packets).
 * Returns 0 on success, negative errno on failure. */
int  mptcp_build_dss(uint8_t *buf, uint16_t *len,
                      uint64_t data_ack, int data_ack_valid,
                      uint64_t data_seq, int data_seq_valid,
                      uint32_t subflow_seq, int subflow_seq_valid,
                      uint16_t data_len, int include_checksum);

/* Parse a received DSS option.  Extracts all fields that are present.
 * On entry, *data_ack_valid, *data_seq_valid, *subflow_seq_valid are
 * assumed 0; on exit they are set to 1 if the corresponding field was
 * present.  Fields are only written when the corresponding valid flag
 * is set (caller can pre-initialise to zeros and check valid flags). */
int  mptcp_parse_dss(const uint8_t *opt, uint16_t optlen,
                      uint64_t *data_ack_out, int *data_ack_valid,
                      uint64_t *data_seq_out, int *data_seq_valid,
                      uint32_t *subflow_seq_out, int *subflow_seq_valid,
                      uint16_t *data_len_out, int *include_checksum);

/* ── MPTCP Data Checksum (RFC 8684 §3.3) ─────────────────────────── */

/* Compute the MPTCP data checksum over pseudo-header + payload.
 * The pseudo-header uses protocol=0 per RFC 8684 §3.3.
 * Returns 16-bit Internet checksum in network byte order. */
uint16_t mptcp_compute_data_checksum(uint32_t src_ip, uint32_t dst_ip,
                                      const void *data, uint16_t data_len);

/* Patch the checksum into a built DSS option buffer (replacing the
 * zero placeholder written by mptcp_build_dss).  The C flag must be
 * set in the DSS flags byte (buf[3] & MPTCP_DSS_FLAG_C). */
int  mptcp_update_dss_checksum(uint8_t *buf,
                                uint32_t src_ip, uint32_t dst_ip,
                                const void *data, uint16_t data_len);

/* Verify the checksum in a received DSS option.  Returns 0 if valid,
 * -EBADMSG if mismatch, -ENODATA if no C flag, -EINVAL on bad args. */
int  mptcp_verify_dss_checksum(const uint8_t *buf, uint16_t optlen,
                                uint32_t src_ip, uint32_t dst_ip,
                                const void *data, uint16_t data_len);


/* Token derivation from key (truncated SHA-256 first 4 bytes) */
uint32_t mptcp_token_from_key(const uint8_t key[8]);

/* Build MP_JOIN option for SYN (client-side subflow addition) */
int  mptcp_build_join_syn(uint8_t *buf, uint16_t *len,
                           uint8_t addr_id, uint32_t token, uint32_t nonce);

/* Build MP_JOIN option for SYN+ACK (server-side) */
int  mptcp_build_join_synack(uint8_t *buf, uint16_t *len,
                              uint8_t addr_id, uint32_t nonce,
                              const uint8_t hmac[8]);

/* Build MP_JOIN option for ACK (completing subflow handshake) */
int  mptcp_build_join_ack(uint8_t *buf, uint16_t *len,
                           const uint8_t hmac[8]);

/* Parse received MP_JOIN SYN option */
int  mptcp_parse_join_syn(const uint8_t *opt, uint16_t optlen,
                           uint8_t *addr_id_out, uint32_t *token_out,
                           uint32_t *nonce_out);

/* Parse received MP_JOIN SYN+ACK option */
int  mptcp_parse_join_synack(const uint8_t *opt, uint16_t optlen,
                              uint8_t *addr_id_out, uint32_t *nonce_out,
                              uint8_t hmac_out[8]);

/* Parse received MP_JOIN ACK option */
int  mptcp_parse_join_ack(const uint8_t *opt, uint16_t optlen,
                           uint8_t hmac_out[8]);

/* ── MPTCP Path Scheduler (D204 task 6) ─────────────────────────── */

/* Spinlock protecting MPTCP connection state — shared between
 * mptcp.c and mptcp_sched.c.  Must be held when calling
 * mptcp_find_by_token(). */
extern spinlock_t mptcp_lock;

/* Look up an MPTCP connection by token.  Caller must hold mptcp_lock.
 * Returns pointer to mptcp_conn, or NULL if not found. */
struct mptcp_conn *mptcp_find_by_token(uint32_t token);

/* Path scheduler algorithms */
#define MPTCP_SCHED_MIN_RTT   0   /* Min-RTT: pick subflow with lowest srtt */
#define MPTCP_SCHED_REDUNDANT 1   /* Redundant: send on all active subflows */
#define MPTCP_SCHED_DEFAULT   MPTCP_SCHED_MIN_RTT

/* Select the best subflow index for sending data on the given MPTCP
 * connection.  Returns the subflow index (0..num_subflows-1) on success,
 * or negative errno (e.g. -ENETDOWN if no active subflows available).
 * The selection is based on the configured path scheduler algorithm. */
int  mptcp_sched_select(struct mptcp_conn *mc);

/* Get the smoothed RTT (srtt from tcp_conns[].srtt) for the subflow
 * at the given index in an MPTCP connection.  Returns the srtt value
 * (scaled by 8, per Jacobson's algorithm), or 0 if the subflow has
 * no RTT measurement yet. */
int32_t mptcp_sched_get_rtt(const struct mptcp_conn *mc, int idx);

/* Set the path scheduler algorithm for an MPTCP connection.
 * alg: one of MPTCP_SCHED_* constants.  Returns 0 on success. */
int  mptcp_sched_set_algo(uint32_t token, int alg);

/* Get the current path scheduler algorithm for an MPTCP connection. */
int  mptcp_sched_get_algo(uint32_t token);

/* ── MPTCP Fast Close (RFC 8684 §3.6) ────────────────────────── */

/* Build an MP_FASTCLOSE TCP option.
 * On entry, *len is the buffer capacity; on exit, *len is bytes written.
 * rcv_key is the receiver's 64-bit key (the key we received from the peer
 * during the MP_CAPABLE handshake, stored in mc->rcv_key).
 * Returns 0 on success, negative errno on failure. */
int  mptcp_build_fastclose(uint8_t *buf, uint16_t *len,
                            const uint8_t rcv_key[8]);

/* Initiate fast close on an MPTCP connection.
 * Sends TCP RST + MP_FASTCLOSE on ALL active subflows, then destroys
 * the MPTCP connection state.  Returns 0 on success, negative errno
 * if the token is not found or the connection is not established. */
int  mptcp_fastclose(uint32_t token);

/* Handle a received MP_FASTCLOSE option.
 * Called from the TCP input path when an MP_FASTCLOSE option is
 * detected on an established TCP connection (conn_id).
 * Validates the key, sends RST on all subflows, and destroys the
 * MPTCP connection.  Returns 0 on success, negative errno on failure. */
int  mptcp_handle_fastclose(int conn_id, const uint8_t *opt, uint16_t optlen);

#endif /* MPTCP_H */
