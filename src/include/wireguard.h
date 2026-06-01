#ifndef WIREGUARD_H
#define WIREGUARD_H

#include "types.h"

/* WireGuard peer */
#define WG_MAX_PEERS 8

struct wg_peer {
    uint32_t endpoint_ip;
    uint16_t endpoint_port;
    uint8_t  public_key[32];
    int      active;
};

/* WireGuard device state */
struct wg_device {
    uint8_t  private_key[32];
    uint8_t  public_key[32];
    uint16_t listen_port;
    struct wg_peer peers[WG_MAX_PEERS];
    int      num_peers;
};

/* ── API ────────────────────────────────────────────────────────── */

int  wg_init(void);
int  wg_create_peer(uint32_t endpoint_ip, uint16_t port);
int  wg_remove_peer(int index);
int  wg_send(const uint8_t *data, int len);
int  wg_receive(const uint8_t *data, int len);

#endif /* WIREGUARD_H */
