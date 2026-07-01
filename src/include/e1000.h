#ifndef E1000_H
#define E1000_H

#include "types.h"

/* Forward declaration for VLAN offload callbacks that take net_device */
struct net_device;

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

/* Promiscuous mode and multicast filter control */
int e1000_set_promisc(int enable);
int e1000_set_allmulti(int enable);
int e1000_set_multicast(void *dev, void *addr, int count);

/* VLAN offload control */
int e1000_vlan_rx_add_vid(struct net_device *dev, uint16_t vid);
int e1000_vlan_rx_kill_vid(struct net_device *dev, uint16_t vid);

/* ── RSS control API ─────────────────────────────────────────────── */

/* Set the RSS hash key (4 dwords, 128 bits total).  Written to HW immediately. */
void e1000_rss_set_key(const uint32_t key[4]);

/* Read back the current RSS hash key. */
void e1000_rss_get_key(uint32_t key[4]);

/* Set which RSS hash types are enabled (bitmask of E1000_RSS_HASH_*).
 * Returns 0 on success, negative errno on invalid types. */
int e1000_rss_set_hash_types(uint32_t types);

/* Get the current RSS hash type bitmask. */
uint32_t e1000_rss_get_hash_types(void);

/* Re-program the RSS Redirection Table (128 entries).
 * If @table is NULL, a round-robin mapping across active queues is used.
 * Returns 0 on success, negative errno on error. */
int e1000_rss_set_reta(const uint8_t table[128]);

/* Compute the RSS hash and queue index for a received Ethernet frame.
 * @buf:    received frame (including Ethernet header).
 * @len:    frame length in bytes.
 * @hash_out: if non-NULL, receives the 32-bit RSS hash.
 * Returns the target queue index (0..num_queues-1). */
int e1000_rss_get_queue_hash(const uint8_t *buf, uint16_t len,
                             uint32_t *hash_out);

#endif
