#ifndef MPTCP_H
#define MPTCP_H

#include "types.h"

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

/* MPTCP flags */
#define MPTCP_CAPABLE_HMAC  0x01  /* MP_CAPABLE includes HMAC */

/* MP_CAPABLE option lengths (RFC 8684 §3.1) */
#define MPTCP_CAPABLE_SYN_LEN     12  /* kind(1) + len(1) + sub/flags(1) + resv(1) + key(8) */
#define MPTCP_CAPABLE_ACK_LEN    24  /* kind(1) + len(1) + sub/flags(1) + resv(1) + snd_key(8) + rcv_key(8) + hmac(8) */

/* ADD_ADDR / REMOVE_ADDR option lengths (RFC 8684 §3.4) */
#define MPTCP_ADD_ADDR4_LEN       10  /* IPv4 ADD_ADDR without port */
#define MPTCP_ADD_ADDR4_LEN_PORT  12  /* IPv4 ADD_ADDR with port */
#define MPTCP_REMOVE_ADDR_MIN_LEN  4  /* Minimum REMOVE_ADDR (1 addr_id) */

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
    uint64_t    rcv_data_seq;        /* Data-level receiving sequence number */
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
};

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
int  mptcp_handle_dss(int conn_id, const uint8_t *opt, uint16_t optlen,
                       uint32_t seq, uint32_t ack);

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

#endif /* MPTCP_H */
