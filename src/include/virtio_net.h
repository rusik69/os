#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "types.h"

/* ── Virtio-net header GSO type constants ────────────────────────── */
#define VIRTIO_NET_HDR_GSO_NONE     0  /* Not a GSO packet */
#define VIRTIO_NET_HDR_GSO_TCPV4    1  /* TCPv4 segments merged */
#define VIRTIO_NET_HDR_GSO_TCPV6    2  /* TCPv6 segments merged */
#define VIRTIO_NET_HDR_GSO_UDP      3  /* UDP fragments merged */
#define VIRTIO_NET_HDR_GSO_TCP_ECN  4  /* TCP with ECN segments merged */

/* ── LRO statistics structure ────────────────────────────────────── */
struct virtio_net_lro_stats {
    uint64_t  merged_packets;       /* Total number of segments that were merged */
    uint64_t  lro_packets;          /* Number of LRO packets received (merged groups) */
    uint64_t  total_bytes;          /* Total bytes in all LRO packets */
    uint64_t  seg_failures;         /* Segmentation failures (e.g. unsupported proto) */
    uint64_t  non_lro_packets;      /* Regular (non-LRO) packets received */
    uint64_t  dropped_oversize;     /* Packets dropped because exceed internal limit */
};

/* Initialize virtio-net (PCI 1AF4:1000).  Returns 0 if found, -1 if absent. */
int  virtio_net_init(void);
int  virtio_net_send(const uint8_t *data, uint32_t len);
int  virtio_net_receive(void *buf, uint16_t max_len);
void virtio_net_get_mac(uint8_t *mac);
int virtio_net_present(void);
void virtio_net_irq_rearm(void);

/* LRO (Large Receive Offload) control and statistics */
int  virtio_net_lro_enabled(void);
void virtio_net_get_lro_stats(struct virtio_net_lro_stats *stats);

/* GRO (Generic Receive Offload) — merge received packets per-flow */
int virtio_net_gro_receive(const uint8_t *pkt, uint32_t len,
                            uint8_t *merged_buf, uint16_t *merged_len,
                            uint64_t current_ticks);

/* ── RSS statistics structure ────────────────────────────────────── */
struct virtio_net_rss_stats {
    uint64_t packets_hashed;       /* Total packets where RSS hash was computed */
    uint64_t packets_unhashed;     /* Packets where no hash type matched */
    uint64_t hash_ipv4;            /* Hash computed for IPv4 (no L4) */
    uint64_t hash_tcpv4;           /* Hash computed for TCPv4 */
    uint64_t hash_udpv4;           /* Hash computed for UDPv4 */
    uint64_t hash_ipv6;            /* Hash computed for IPv6 (no L4) */
    uint64_t hash_tcpv6;           /* Hash computed for TCPv6 */
    uint64_t hash_udpv6;           /* Hash computed for UDPv6 */
    uint64_t hash_ipv6_ex;         /* Hash computed for IPv6 with extension hdrs */
    uint64_t hash_tcpv6_ex;        /* Hash computed for TCPv6 with extension hdrs */
    uint64_t hash_udpv6_ex;        /* Hash computed for UDPv6 with extension hdrs */
};

/* ── RSS hash report structure ───────────────────────────────────── */
struct virtio_net_rss_hash_report {
    uint32_t hash_value;           /* RSS Toeplitz hash value */
    uint8_t  hash_type;            /* VIRTIO_NET_HASH_REPORT_* type */
    uint16_t queue;                /* Selected queue index (if steered) */
};

/* ── RSS functions ────────────────────────────────────────────────── *
 * RSS (Receive Side Steering) infrastructure: compute Toeplitz hash
 * on received packets, configure hash types and indirection table,
 * and query RSS statistics.
 *
 * RSS is implemented in software for the legacy transport; the modern
 * transport path may additionally negotiate VIRTIO_NET_F_RSS with the
 * device for hardware-accelerated steering. */

/* Initialize RSS subsystem: set default hash key and indirection table. */
int  virtio_net_rss_init(void);

/* Set RSS configuration: hash key (40 bytes), indirection table,
 * hash type enable mask, and fallback queue index for unclassified pkts.
 * Returns 0 on success, -1 on invalid arguments.
 * If a control virtqueue is available, also sends the config to the device. */
int  virtio_net_rss_set_config(const uint8_t *key, size_t key_len,
                                const uint16_t *indir_table, uint16_t indir_len,
                                uint32_t hash_types, uint16_t unclassified_q);

/* Get current RSS configuration. Pointers may be NULL to skip. */
void virtio_net_rss_get_config(uint32_t *hash_types, uint16_t *unclassified_q);

/* Copy current RSS statistics into the provided structure. */
void virtio_net_rss_get_stats(struct virtio_net_rss_stats *stats);

/* Compute the RSS Toeplitz hash for a received Ethernet packet.
 * Returns the 32-bit hash value and sets *hash_type_out to the
 * corresponding VIRTIO_NET_HASH_REPORT_* constant (NONE if the
 * packet type is not configured for RSS hashing).
 * The caller must provide the full Ethernet frame in 'pkt'. */
uint32_t virtio_net_rss_hash_packet(const uint8_t *pkt, uint32_t len,
                                     uint8_t *hash_type_out);

/* ── Control VQ (MAC filtering) ──────────────────────────────────────
 * Requires VIRTIO_NET_F_CTRL_VQ and VIRTIO_NET_F_CTRL_RX negotiated.
 * These functions control promiscuous/allmulti mode and MAC address
 * filtering via the control virtqueue. */

/* Set promiscuous mode: 1 = accept all packets, 0 = filter per MAC table.
 * Returns 0 on success, -1 if control vq not available or device rejected. */
int virtio_net_ctrl_set_promisc(int on);

/* Set all-multicast mode: 1 = accept all multicast, 0 = filter per MAC table.
 * Returns 0 on success, -1 if control vq not available or device rejected. */
int virtio_net_ctrl_set_allmulti(int on);

/* Set MAC address filtering table.
 * uc_macs: array of 6-byte unicast MAC addresses to accept (may be NULL if num_uc==0).
 * num_uc: number of unicast MACs in uc_macs.
 * mc_macs: array of 6-byte multicast MAC addresses to accept (may be NULL if num_mc==0).
 * num_mc: number of multicast MACs in mc_macs.
 * Pass num_uc=0 and uc_macs=NULL to accept no unicast (when promisc off).
 * Pass num_mc=0 and mc_macs=NULL to accept no multicast (when allmulti off).
 * Returns 0 on success, -1 if control vq not available or device rejected. */
int virtio_net_ctrl_set_mac_table(const uint8_t *uc_macs, uint16_t num_uc,
                                   const uint8_t *mc_macs, uint16_t num_mc);

#endif
