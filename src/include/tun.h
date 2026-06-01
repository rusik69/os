#ifndef TUN_H
#define TUN_H

#include "types.h"

/* TUN/TAP flags */
#define IFF_TUN      0x0001  /* Layer 3 (IP) tunnel */
#define IFF_TAP      0x0002  /* Layer 2 (Ethernet) tap */
#define IFF_NO_PI    0x1000  /* No packet information */

/* TUN/TAP ring buffer size */
#define TUN_RING_SIZE 256
#define TUN_PKT_MAX   2048

/* TUN device state */
struct tun_device {
    int      flags;
    uint8_t  ring_buf[TUN_RING_SIZE][TUN_PKT_MAX];
    uint16_t ring_len[TUN_RING_SIZE];
    int      ring_head;
    int      ring_tail;
    int      ring_count;
    int      opened;
};

/* ── API ────────────────────────────────────────────────────────── */

int  tun_init(void);
int  tun_open(int flags);
int  tun_write(int fd, const void *data, uint16_t len);
int  tun_read(int fd, void *buf, uint16_t max_len);
void tun_destroy(void);

#endif /* TUN_H */
