#ifndef E1000_H
#define E1000_H

#include "types.h"

#define E1000_VENDOR    0x8086
#define E1000_DEVICE    0x100E  /* 82540EM - QEMU default */
#define E1000_DEV_82574 0x10D3  /* 82574L - supports multi-queue RSS (2 queues) */
#define E1000_DEV_82576 0x10C9  /* 82576 - supports up to 4 queues */

/* Maximum number of RX/TX queues supported (82574: up to 2) */
#define E1000_MAX_QUEUES 4

/* RSS hash types */
#define E1000_RSS_HASH_TCP_IPV4  (1U << 1)
#define E1000_RSS_HASH_IPV4      (1U << 2)
#define E1000_RSS_HASH_TCP_IPV6  (1U << 3)
#define E1000_RSS_HASH_IPV6      (1U << 4)
#define E1000_RSS_HASH_IPV6_EX   (1U << 5)
#define E1000_RSS_HASH_TCP_IPV6_EX (1U << 6)

int e1000_init(void);
int e1000_send(const void *data, uint16_t len);
int e1000_receive(void *buf, uint16_t max_len);
void e1000_get_mac(uint8_t *mac);
int e1000_is_present(void);
void e1000_irq_rearm(void);
void e1000_exit(void);

/* Return the number of RX queues active */
int e1000_rx_queue_count(void);

#endif
