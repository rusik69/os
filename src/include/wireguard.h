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
#define WG_HANDSHAKE_INIT_LEN     148
#define WG_HANDSHAKE_RESPONSE_LEN  92

/* Maximum concurrent handshake states tracked */
#define WG_MAX_HANDSHAKES 4

struct wg_peer {
    uint32_t endpoint_ip;       /* configured peer address */
    uint16_t endpoint_port;     /* configured peer port */
    uint8_t  public_key[32];
    int      active;

    /* ── Keepalive and roaming state ──────────────────────────── */
    uint64_t last_tx_time;      /* timer_get_ticks() of last data send */
    uint64_t last_rx_time;      /* timer_get_ticks() of last data receive */
    uint32_t persistent_keepalive_interval; /* seconds, 0 = disabled */
    uint32_t rx_ip;             /* last observed source IP (for roaming) */
    uint16_t rx_port;           /* last observed source port (for roaming) */
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

/* WireGuard encrypt/decrypt utility functions */
int  wireguard_encrypt(const uint8_t *plaintext, uint64_t plaintext_len,
                       uint8_t *ciphertext, const uint8_t *key, const uint8_t *nonce);
int  wireguard_decrypt(const uint8_t *ciphertext, uint64_t ciphertext_len,
                       uint8_t *plaintext, const uint8_t *key, const uint8_t *nonce);

#endif /* WIREGUARD_H */
