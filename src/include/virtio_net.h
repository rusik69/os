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

#endif
