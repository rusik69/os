#ifndef WIREGUARD_H
#define WIREGUARD_H

#include "types.h"

/* WireGuard peer */
#define WG_MAX_PEERS 8
#define WG_KEEPALIVE_DEFAULT_INTERVAL 25  /* default 25 seconds */
#define WG_KEEPALIVE_MIN_INTERVAL      5   /* minimum 5 seconds */

/* WireGuard message types */
#define WG_MSG_HANDSHAKE_INIT    1
#define WG_MSG_HANDSHAKE_RESPONSE 2
#define WG_MSG_COOKIE_REPLY      3
#define WG_MSG_TRANSPORT_DATA    4

/* WireGuard message type alias (backward compat) */
#define WG_MSG_DATA      WG_MSG_TRANSPORT_DATA
#define WG_MSG_KEEPALIVE WG_MSG_TRANSPORT_DATA  /* zero-length payload */

/* WireGuard message lengths */
#define WG_HANDSHAKE_INIT_LEN       148
#define WG_HANDSHAKE_RESPONSE_LEN    92
#define WG_COOKIE_REPLY_LEN          80
#define WG_COOKIE_LEN                16

/* Cookie nonce length (24 bytes for XChaCha20 compatibility;
 * we use first 12 bytes for ChaCha20 and zero-pad the rest). */
#define WG_COOKIE_NONCE_LEN         24

/* Cookie secret rotation interval (~120 seconds at 100 Hz) */
#define WG_COOKIE_SECRET_ROTATION   12000

/* Rate limit: max handshake initiations per second per IP */
#define WG_RATELIMIT_MAX_BURST      20
#define WG_RATELIMIT_WINDOW_TICKS   100  /* 1 second at 100 Hz */

/* Maximum number of tracked rate-limit entries */
#define WG_RATELIMIT_ENTRIES        64

/* Maximum allowed-IP entries per peer */
#define WG_MAX_ALLOWED_IPS 16

/* Maximum concurrent handshake states tracked */
#define WG_MAX_HANDSHAKES 4

/* WireGuard allowed-IP routing entry */
struct wg_allowed_ip {
    uint32_t addr;       /* Network address (network byte order) */
    uint8_t  cidr;       /* CIDR prefix length (0-32) */
    int      active;     /* 1 if this entry is used */
};

struct wg_peer {
    uint32_t endpoint_ip;       /* configured peer address */
    uint16_t endpoint_port;     /* configured peer port */
    uint8_t  public_key[32];
    int      active;

    /* ── Transport session state (derived from Noise handshake) ── */
    uint8_t  transport_key[32];     /* ChaCha20Poly1305 session key */
    uint64_t tx_counter;            /* monotonic counter for tx nonce */
    uint64_t rx_counter;            /* expected counter for rx nonce */
    int      session_established;   /* 1 = handshake done, transport ready */

    /* ── Keepalive and roaming state ──────────────────────────── */
    uint64_t last_tx_time;      /* timer_get_ticks() of last data send */
    uint64_t last_rx_time;      /* timer_get_ticks() of last data receive */
    uint32_t persistent_keepalive_interval; /* seconds, 0 = disabled */
    uint32_t rx_ip;             /* last observed source IP (for roaming) */
    uint16_t rx_port;           /* last observed source port (for roaming) */

    /* ── Anti-DoS cookie state ──────────────────────────────────── */
    uint8_t  cookie_key[32];            /* expanded cookie key for KDF (32B) */
    int      has_cookie;                /* 1 if cookie is valid */

    /* ── Allowed-IP routing table (implicit routing) ──────────── */
    struct wg_allowed_ip allowed_ips[WG_MAX_ALLOWED_IPS];
    int                  num_allowed_ips;
};

/* WireGuard device state */
struct wg_device {
    uint8_t  private_key[32];
    uint8_t  public_key[32];
    uint16_t listen_port;
    struct wg_peer peers[WG_MAX_PEERS];
    int      num_peers;

    /* ── Device-level keepalive timer ──────────────────────────── */
    uint64_t last_poll_time;    /* last time wg_poll() ran */
};

/* ── API ────────────────────────────────────────────────────────── */

int  wg_init(void);
int  wg_create_peer(uint32_t endpoint_ip, uint16_t port);
int  wg_remove_peer(int index);
int  wg_send(const uint8_t *data, int len);
int  wg_receive(const uint8_t *data, int len, uint32_t src_ip, uint16_t src_port);
int  wg_set_persistent_keepalive(int index, uint32_t interval);
void wg_poll(void);

/* ── Handshake API ──────────────────────────────────────────── */

/* WireGuard Noise_IKpsk2 handshake initiation.
 * Builds the handshake initiation message (148 bytes) for the given
 * endpoint and returns WG_HANDSHAKE_INIT_LEN on success, or negative errno. */
int  wireguard_send_handshake_init(uint32_t endpoint_ip, uint16_t endpoint_port);

/* Process an incoming handshake initiation message from a peer.
 * Returns 0 on success with session established, or negative errno. */
int  wireguard_recv_handshake_init(const uint8_t *pkt, uint16_t len,
                                   uint32_t src_ip, uint16_t src_port);

/* WireGuard handshake response: send to a peer after receiving an init.
 * Returns WG_HANDSHAKE_RESPONSE_LEN on success, or negative errno. */
int  wireguard_send_handshake_response(uint32_t endpoint_ip, uint16_t endpoint_port);

/* Process an incoming handshake response message.
 * Returns 0 on success, or negative errno. */
int  wireguard_recv_handshake_response(const uint8_t *pkt, uint16_t len,
                                       uint32_t src_ip, uint16_t src_port);

/* WireGuard cookie reply (anti-DoS mechanism).
 * Returns WG_COOKIE_REPLY_LEN on success, or negative errno. */
int  wireguard_send_cookie(uint32_t endpoint_ip, uint16_t endpoint_port,
                           const uint8_t *cookie, uint16_t cookie_len);

/* Process an incoming cookie reply message. */
int  wireguard_recv_cookie(const uint8_t *pkt, uint16_t len,
                           uint8_t *cookie_out, uint16_t *cookie_len);

/* WireGuard rate limiting for handshake messages.
 * Returns 0 to allow, negative to drop. */
int  wireguard_ratelimit(uint32_t src_ip);

/* WireGuard peer expiry — marks peer inactive and tears down session. */
int  wireguard_expire(int peer_idx);

/* ── Allowed-IP routing API ─────────────────────────────────────── */

/* Add an allowed-IP CIDR entry to a peer.
 * @peer_idx: index of the peer (0..num_peers-1)
 * @addr: network address in network byte order
 * @cidr: prefix length (0-32)
 * Returns 0 on success, or negative errno. */
int  wg_peer_add_allowed_ip(int peer_idx, uint32_t addr, uint8_t cidr);

/* Remove an allowed-IP CIDR entry from a peer.
 * Returns 0 on success, or negative errno. */
int  wg_peer_remove_allowed_ip(int peer_idx, uint32_t addr, uint8_t cidr);

/* Find the peer with the most specific matching allowed-IP for a
 * destination address.  Returns peer index (>= 0) on success, or
 * -EHOSTUNREACH if no peer covers dest_ip. */
int  wg_peer_lookup_by_dest(uint32_t dest_ip);

/* Send data to a peer, routing by destination IP via allowed-IP lookup.
 * Falls back to the first active peer if no specific route matches.
 * Returns total bytes sent on success, or negative errno. */
int  wg_send_to(uint32_t dest_ip, const uint8_t *data, int len);

/* Check whether a source IP is allowed for a peer (matches allowed-IPs).
 * Returns 1 if allowed, 0 if denied (or peer has no allowed-IPs). */
int  wg_peer_check_source(int peer_idx, uint32_t src_ip);

/* WireGuard encrypt/decrypt utility functions */
int  wireguard_encrypt(const uint8_t *plaintext, uint64_t plaintext_len,
                       uint8_t *ciphertext, const uint8_t *key, const uint8_t *nonce);
int  wireguard_decrypt(const uint8_t *ciphertext, uint64_t ciphertext_len,
                       uint8_t *plaintext, const uint8_t *key, const uint8_t *nonce);

#endif /* WIREGUARD_H */
