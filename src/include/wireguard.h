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

/* WireGuard MTU constants */
#define WG_MTU                      1420   /* Default WireGuard tunnel MTU (inner packet) */
#define WG_OVERHEAD                 32     /* Transport header (16) + Poly1305 auth tag (16) */
#define WG_MAX_WIRE_SIZE            (WG_MTU + WG_OVERHEAD)  /* Maximum outer UDP payload */

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

/* WireGuard TX packet queue */
#define WG_TX_QUEUE_MAX_DEPTH 64

struct wg_tx_packet {
    uint8_t             *data;      /* heap-allocated, complete WireGuard message */
    int                  len;       /* total length of data */
    uint32_t             dst_ip;    /* destination IP (network byte order) */
    uint16_t             dst_port;  /* destination UDP port */
    uint16_t             src_port;  /* source UDP port */
    struct wg_tx_packet *next;      /* next in queue */
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

    /* ── TX packet queue (encrypted, waiting to be sent via UDP) ──── */
    struct wg_tx_packet *tx_head;
    struct wg_tx_packet *tx_tail;
    int                  tx_count;
};

/* WireGuard device state */
struct wg_device {
    uint8_t  private_key[32];
    uint8_t  public_key[32];
    uint16_t listen_port;
    int      mtu;               /* Tunnel MTU (default WG_MTU, maximum inner packet size) */
    struct wg_peer peers[WG_MAX_PEERS];
    int      num_peers;

    /* ── Device-level keepalive timer ──────────────────────────── */
    uint64_t last_poll_time;    /* last time wg_poll() ran */

    /* ── Virtual interface (net_device) index ──────────────────── */
    int      ifindex;           /* net_device ifindex, -1 if not registered */
};

/* ── API ────────────────────────────────────────────────────────── */

int  wg_init(void);
int  wg_get_mtu(void);
int  wg_set_mtu(int mtu);
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

/* WireGuard TX queue flush — sends all queued encrypted packets via UDP.
 * Should be called periodically (e.g., from wg_poll() or a timer). */
void wg_tx_flush(void);

/* ── Interface lifecycle (virtual net_device) ───────────────────── */

/* Create and register the WireGuard virtual network interface.
 * Returns the ifindex (>= 0) on success, or negative errno. */
int  wg_iface_create(void);

/* Destroy and unregister the WireGuard virtual network interface.
 * Flushes all TX queues and tears down peer sessions.
 * Returns 0 on success, or negative errno. */
int  wg_iface_destroy(void);

/* Bring the WireGuard interface up (set IFF_UP | IFF_RUNNING).
 * Returns 0 on success, or negative errno. */
int  wg_iface_up(void);

/* Bring the WireGuard interface down (clear IFF_UP | IFF_RUNNING).
 * Flushes pending TX queues and stops the keepalive timer.
 * Returns 0 on success, or negative errno. */
int  wg_iface_down(void);

/* ── Generic Netlink family (userspace configuration) ──────────────── */

/* WireGuard generic netlink family name */
#define WG_GENL_FAMILY_NAME  "wireguard"
#define WG_GENL_VERSION      1

/* WireGuard generic netlink commands */
enum wg_cmd {
    WG_CMD_UNSPEC       = 0,
    WG_CMD_SET_DEVICE,      /* Configure the WireGuard device */
    WG_CMD_GET_DEVICE,      /* Query WireGuard device and peer state */
    WG_CMD_SET_PEER,        /* Add or update a peer */
    WG_CMD_REMOVE_PEER,     /* Remove a peer */
    __WG_CMD_MAX,
};

#define WG_CMD_MAX (__WG_CMD_MAX - 1)

/* WireGuard generic netlink device attributes */
enum wg_device_attr {
    WG_DEVICE_A_UNSPEC         = 0,
    WG_DEVICE_A_IFINDEX,           /* uint32_t — interface index */
    WG_DEVICE_A_PRIVATE_KEY,       /* binary, 32 bytes */
    WG_DEVICE_A_PUBLIC_KEY,        /* binary, 32 bytes (read-only) */
    WG_DEVICE_A_LISTEN_PORT,       /* uint16_t */
    WG_DEVICE_A_FWMARK,            /* uint32_t */
    WG_DEVICE_A_PEERS,             /* nested — array of WG_PEER_A_* */
    __WG_DEVICE_A_MAX,
};

#define WG_DEVICE_A_MAX (__WG_DEVICE_A_MAX - 1)

/* WireGuard generic netlink peer attributes */
enum wg_peer_attr {
    WG_PEER_A_UNSPEC               = 0,
    WG_PEER_A_PUBLIC_KEY,              /* binary, 32 bytes */
    WG_PEER_A_PERSISTENT_KEEPALIVE_INTERVAL, /* uint32_t, seconds */
    WG_PEER_A_ENDPOINT_IP,             /* uint32_t, network byte order */
    WG_PEER_A_ENDPOINT_PORT,           /* uint16_t */
    WG_PEER_A_ALLOWED_IPS,             /* nested — array of WG_ALLOWED_IP_A_* */
    WG_PEER_A_REMOVE_ME,               /* flag — mark peer for removal */
    __WG_PEER_A_MAX,
};

#define WG_PEER_A_MAX (__WG_PEER_A_MAX - 1)

/* WireGuard allowed-IP attributes (nested inside WG_PEER_A_ALLOWED_IPS) */
enum wg_allowed_ip_attr {
    WG_ALLOWED_IP_A_UNSPEC  = 0,
    WG_ALLOWED_IP_A_ADDR,        /* uint32_t, network byte order */
    WG_ALLOWED_IP_A_CIDR,        /* uint8_t, 0-32 */
    __WG_ALLOWED_IP_A_MAX,
};

#define WG_ALLOWED_IP_A_MAX (__WG_ALLOWED_IP_A_MAX - 1)

/* ── Accessor functions for wg_netlink.c ───────────────────────────── */

/* Set the device private key and derive the public key.
 * Returns 0 on success. */
int  wg_set_private_key(const uint8_t key[32]);

/* Get the device public key (read-only).
 * @out: 32-byte buffer to receive the public key. */
void wg_get_device_pubkey(uint8_t out[32]);

/* Get/set the listen port. */
uint16_t wg_get_listen_port(void);
void     wg_set_listen_port(uint16_t port);

/* Find a peer by its Curve25519 public key.
 * Returns peer index (>= 0) on success, or -ENOENT if not found. */
int  wg_find_peer_by_pubkey(const uint8_t pubkey[32]);

/* Create a peer with a specific public key (no auto-generation).
 * Returns peer index on success, or negative errno. */
int  wg_create_peer_with_key(const uint8_t pubkey[32]);

/* Get the current number of active peers. */
int  wg_get_num_peers(void);

/* Copy peer state into the provided structure.
 * @idx: peer index (0-based, must be active)
 * @out: output buffer for peer state
 * Returns 0 on success, negative errno on error. */
int  wg_get_peer_info(int idx, struct wg_peer *out);

/* Initialise the WireGuard generic netlink family.
 * Called from wg_init(). */
int  wg_netlink_init(void);

#endif /* WIREGUARD_H */
