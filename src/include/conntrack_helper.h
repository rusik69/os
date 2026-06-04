#ifndef CONNTRACK_HELPER_H
#define CONNTRACK_HELPER_H

#include "types.h"
#include "netfilter.h"

/* Forward declarations for network header structures used in
 * the helper hook function type. */
struct ip_header;
struct tcp_header;

/*
 * conntrack_helper.h — Conntrack ALG (Application Layer Gateway) helpers
 *
 * Provides infrastructure for protocol helpers that inspect application-layer
 * payload (e.g., FTP PORT commands) and create "expected" connections for
 * related data channels (e.g., FTP data connections).
 *
 * An expected connection tuple says: "a connection from src_ip:src_port to
 * dst_ip:dst_port using 'protocol' is expected soon."  When the actual
 * connection arrives, conntrack matches it against the expected table and
 * marks it NF_CONN_RELATED.
 *
 * Helpers are per-protocol (FTP, SIP, TFTP, etc.) and register via
 * nf_helper_register().
 */

/* ── Expected connection table ───────────────────────────────────── */
#define NF_CT_EXPECT_MAX  32

struct nf_ct_expect {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  used;
    uint64_t expiry;          /* timer ticks after which this entry expires */
};

/* ── Helper hook type ─────────────────────────────────────────────── */

/*
 * A helper hook is called for each packet of a known helper protocol
 * (e.g., TCP port 21 for FTP).  The hook receives the raw packet buffer
 * (starting at Ethernet header), the IP and TCP header pointers, and the
 * TCP payload pointer + length.
 *
 * Return NF_ACCEPT to continue, NF_DROP to drop.
 */
typedef int (*nf_helper_hook_t)(void *skb,
                                struct ip_header *ip,
                                struct tcp_header *tcp,
                                const uint8_t *payload,
                                uint16_t payload_len,
                                int from_originator);

/* ── Helper entry ─────────────────────────────────────────────────── */

struct nf_helper {
    uint8_t  protocol;         /* IPPROTO_TCP, IPPROTO_UDP */
    uint16_t dst_port;         /* port to match (e.g., 21 for FTP) */
    nf_helper_hook_t hook_fn;
    char     name[24];
    uint8_t  used;
};

/* ── Public API ───────────────────────────────────────────────────── */

/* Register a helper. Returns 0 on success, -1 on failure. */
int nf_helper_register(uint8_t protocol, uint16_t dst_port,
                       nf_helper_hook_t fn, const char *name);

/* Unregister a previously registered helper. */
void nf_helper_unregister(nf_helper_hook_t fn);

/* Register an expected connection for a RELATED data channel.
 * The 'master' connection is the control connection (e.g., FTP control).
 * The expected tuple describes the anticipated data channel. */
int nf_ct_expect_related(struct nf_conn *master,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         uint8_t protocol);

/* Check if the given tuple matches an expected connection.
 * Returns a pointer to the expected entry, or NULL. */
struct nf_ct_expect *nf_ct_expect_lookup(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port, uint16_t dst_port,
                                          uint8_t protocol);

/* Remove expected entries belonging to a given master connection. */
void nf_ct_expect_clear(struct nf_conn *master);

/* Periodic expiry of stale expected entries. */
void nf_ct_expect_purge(void);

/* Initialise the helper subsystem. */
void nf_helper_init(void);

/* Check if a new connection (src,dst,ports,proto) matches an expected
 * entry from a protocol ALG.  Returns 1 if RELATED (expected), 0 otherwise.
 * If found, the expected entry is consumed (one-shot). */
int nf_ct_check_expected(uint32_t src_ip, uint32_t dst_ip,
                          uint16_t src_port, uint16_t dst_port,
                          uint8_t protocol);

#endif /* CONNTRACK_HELPER_H */
