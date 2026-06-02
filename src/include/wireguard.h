#ifndef WIREGUARD_H
#define WIREGUARD_H

#include "types.h"

/* WireGuard peer */
#define WG_MAX_PEERS 8
#define WG_KEEPALIVE_DEFAULT_INTERVAL 25  /* default 25 seconds */
#define WG_KEEPALIVE_MIN_INTERVAL      5   /* minimum 5 seconds */

/* WireGuard transport message types */
#define WG_MSG_DATA      4
#define WG_MSG_KEEPALIVE 4  /* same type, zero-length payload */

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

#endif /* WIREGUARD_H */
