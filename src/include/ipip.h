#ifndef IPIP_H
#define IPIP_H

#include "types.h"

/* IP-in-IP tunnel (protocol 4) */
#define IPIP_PROTOCOL 4

/* Tunnel endpoint */
struct ipip_tunnel {
    uint32_t remote_ip;
    uint32_t local_ip;
    int      active;
};

/* ── API ────────────────────────────────────────────────────────── */

int  ipip_init(void);
int  ipip_create_tunnel(uint32_t remote, uint32_t local);
int  ipip_destroy_tunnel(void);
int  ipip_encapsulate(const uint8_t *inner_pkt, int inner_len,
                      uint8_t *outer_buf, int outer_max);
int  ipip_decapsulate(const uint8_t *outer_pkt, int outer_len,
                      uint8_t *inner_buf, int inner_max);

#endif /* IPIP_H */
