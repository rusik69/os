#ifndef IPVS_H
#define IPVS_H

#include "types.h"

/* IP Virtual Server */
#define IPVS_MAX_VIRTUALS 8
#define IPVS_MAX_REALS    8

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

/* ── API ────────────────────────────────────────────────────────── */

int  ipvs_init(void);
int  ipvs_add_virtual(uint32_t vip, uint16_t port, uint8_t protocol);
int  ipvs_del_virtual(uint32_t vip, uint16_t port);
int  ipvs_add_real(uint32_t vip, uint16_t port, uint32_t rip, uint16_t rport, int weight);
int  ipvs_del_real(uint32_t vip, uint16_t port, uint32_t rip, uint16_t rport);
int  ipvs_get_dest(uint32_t vip, uint16_t port, uint32_t *rip_out, uint16_t *rport_out);

#endif /* IPVS_H */
