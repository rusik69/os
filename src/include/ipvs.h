#ifndef IPVS_H
#define IPVS_H

#include "types.h"

/* IP Virtual Server */
#define IPVS_MAX_VIRTUALS 8
#define IPVS_MAX_REALS    8

/* Connection tracking constants */
#define IPVS_CONN_HASH_SIZE 64
#define IPVS_CONN_TIMEOUT_MS 300000  /* 5 minutes default */

/* Virtual service */
struct ipvs_virtual {
    uint32_t vip;
    uint16_t port;
    uint8_t  protocol;
    int      active;
    struct ipvs_real *reals[IPVS_MAX_REALS];
    int      num_reals;
    int      rr_next;  /* round-robin next index */
};

/* Real server */
struct ipvs_real {
    uint32_t rip;
    uint16_t port;
    int      weight;
    int      active_conns;
};

/* ── Connection tracking ──────────────────────────────────────────── */

/* Connection states */
#define IP_VS_CONN_NONE     0
#define IP_VS_CONN_ESTAB    1
#define IP_VS_CONN_FIN_WAIT 2
#define IP_VS_CONN_CLOSE    3

/* NAT modes */
#define IPVS_NAT_NONE    0
#define IPVS_NAT_MASQ    1  /* MASQUERADE (SNAT) */
#define IPVS_NAT_DR      2  /* Direct Routing */
#define IPVS_NAT_TUNNEL  3

/* IPVS connection entry */
struct ip_vs_conn {
    uint32_t c_ip;          /* client IP */
    uint16_t c_port;        /* client port */
    uint32_t v_ip;          /* virtual service IP */
    uint16_t v_port;        /* virtual service port */
    uint32_t d_ip;          /* destination (real server) IP */
    uint16_t d_port;        /* destination (real server) port */
    uint8_t  protocol;      /* IP protocol */
    uint8_t  state;         /* connection state */
    uint8_t  nat_mode;      /* NAT mode (NONE/MASQ/DR/TUNNEL) */

    /* Saved original addresses for NAT reverse translation */
    uint32_t orig_src_ip;
    uint16_t orig_src_port;
    uint32_t orig_dst_ip;
    uint16_t orig_dst_port;

    uint64_t expiry;        /* expiration timestamp (ms) */
    struct ip_vs_conn *next; /* hash chain */
};

/* ── Connection tracking API ──────────────────────────────────────── */

/* Create a new connection entry */
struct ip_vs_conn *ip_vs_conn_new(uint32_t c_ip, uint16_t c_port,
                                   uint32_t v_ip, uint16_t v_port,
                                   uint32_t d_ip, uint16_t d_port,
                                   uint8_t protocol);

/* Look up an existing connection by client<->virtual tuple */
struct ip_vs_conn *ip_vs_conn_lookup(uint32_t c_ip, uint16_t c_port,
                                      uint32_t v_ip, uint16_t v_port,
                                      uint8_t protocol);

/* Mark a connection for expiry (removes from hash, schedules cleanup) */
void ip_vs_conn_expire(struct ip_vs_conn *conn);

/* Clean up all expired connections (call periodically) */
void ip_vs_conn_cleanup(void);

/* Get the number of active connections */
int ip_vs_conn_count(void);

/* ── NAT functions ────────────────────────────────────────────────── */

/* NAT inbound: translate destination address from virtual to real server.
 * Called on packets arriving at the virtual service destined for a real server.
 * Returns 0 on success, -1 if conn is NULL. */
int ipvs_nat_in(struct ip_vs_conn *conn, uint32_t *ip, uint16_t *port);

/* NAT outbound: translate source address from real server back to virtual.
 * Called on returning packets from the real server to the client.
 * Returns 0 on success, -1 if conn is NULL. */
int ipvs_nat_out(struct ip_vs_conn *conn, uint32_t *ip, uint16_t *port);

/* ── Virtual / Real server API (existing) ────────────────────────── */

int  ipvs_init(void);
int  ipvs_add_virtual(uint32_t vip, uint16_t port, uint8_t protocol);
int  ipvs_del_virtual(uint32_t vip, uint16_t port);
int  ipvs_add_real(uint32_t vip, uint16_t port, uint32_t rip, uint16_t rport, int weight);
int  ipvs_del_real(uint32_t vip, uint16_t port, uint32_t rip, uint16_t rport);
int  ipvs_get_dest(uint32_t vip, uint16_t port, uint32_t *rip_out, uint16_t *rport_out);

#endif /* IPVS_H */
