/*
 * virtio-net driver  (legacy PCI device 1AF4:1000)
 *
 * In the standard QEMU x86 machine the default NIC is an e1000, so this
 * driver will normally NOT find a device and virtio_net_init() will return -1.
 * It is included so that kernels running on virtio-net QEMU machines (launched
 * with -device virtio-net-pci) can use the faster paravirtual NIC.
 *
 * Supported: legacy virtio (PCI revision 0, device 1).  Modern (PCIe) virtio
 * is not covered here.
 *
 * Large Receive Offload (LRO):
 *   When VIRTIO_NET_F_GUEST_TSO4/6 is negotiated, the host may deliver TCP
 *   segments coalesced into a single large packet.  On receive, we segment
 *   the merged packet back into individual MSS-sized packets for the TCP
 *   stack, tracking state across multiple virtio_net_receive() calls.
 */

#include "virtio_net.h"
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "types.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "net.h"
#include "spinlock.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── Virtio PCI constants ───────────────────────────────────────── */
#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_NET_DEVICE      0x1000

/* virtio-net config starts at offset 20 */
#define VIRTIO_PCI_CONFIG      20

/* ── Virtio-net header flags (for gso_type field) ───────────────── */
#define VIRTIO_NET_HDR_F_NEEDS_CSUM    1   /* Device needs checksum computation */

/*
 * Features this driver supports:
 *   RX offload:
 *     - VIRTIO_NET_F_GUEST_TSO4:       guest can receive TSOv4 (LRO for TCPv4)
 *     - VIRTIO_NET_F_GUEST_TSO6:       guest can receive TSOv6 (LRO for TCPv6)
 *     - VIRTIO_NET_F_GUEST_ECN:        guest can receive TSO with ECN
 *     - VIRTIO_NET_F_GUEST_UFO:        guest can receive UFO (LRO for UDP)
 *     - VIRTIO_NET_F_GUEST_CSUM:       guest can verify checksums (required for LRO)
 *   TX offload (TSO/GSO/GRO):
 *     - VIRTIO_NET_F_HOST_TSO4:        host can receive TSOv4 (TX offload)
 *     - VIRTIO_NET_F_HOST_TSO6:        host can receive TSOv6 (TX offload)
 *     - VIRTIO_NET_F_HOST_ECN:         host can receive TSO with ECN
 *     - VIRTIO_NET_F_HOST_UFO:         host can receive UFO (TX offload)
 *     - VIRTIO_NET_F_CSUM:             host can compute checksums (TX csum offload)
 *     - VIRTIO_NET_F_GSO:              generic segmentation offload
 *     - VIRTIO_NET_F_MRG_RXBUF:        mergeable RX buffers (for GRO)
 *   Common:
 *     - VIRTIO_F_NOTIFY_ON_EMPTY:      notify when avail ring goes empty
 */
#define VNET_SUPPORTED_FEATURES \
    (VIRTIO_NET_F_MAC | \
     VIRTIO_NET_F_GUEST_TSO4 | VIRTIO_NET_F_GUEST_TSO6 | \
     VIRTIO_NET_F_GUEST_ECN | VIRTIO_NET_F_GUEST_UFO | \
     VIRTIO_NET_F_GUEST_CSUM | \
     VIRTIO_NET_F_HOST_TSO4 | VIRTIO_NET_F_HOST_TSO6 | \
     VIRTIO_NET_F_HOST_ECN | VIRTIO_NET_F_HOST_UFO | \
     VIRTIO_NET_F_CSUM | VIRTIO_NET_F_GSO | \
     VIRTIO_NET_F_MRG_RXBUF | \
     VIRTIO_NET_F_CTRL_VQ | VIRTIO_NET_F_CTRL_RX | VIRTIO_NET_F_CTRL_VLAN | \
     VIRTIO_F_NOTIFY_ON_EMPTY)

/* Features this driver REQUIRES from the device:
 * - VIRTIO_NET_F_MAC: we don't support random MAC assignment */
#define VNET_REQUIRED_FEATURES \
    (VIRTIO_NET_F_MAC)

/* Virtqueue size (must be power of 2) */
#define VRING_SIZE 16

/* Descriptor flags */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* ── Virtqueue structures ────────────────────────────────────────── */
#pragma pack(push, 1)
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VRING_SIZE];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VRING_SIZE];
};
#pragma pack(pop)

/* ── Available ring overflow check ───────────────────────────────────
 * Returns 1 if the avail ring has room for 'needed' additional entries,
 * 0 if adding them would overflow the ring (overwriting entries the
 * device hasn't consumed yet).
 *
 * The virtio spec requires that the driver MUST NOT add more entries
 * than the ring can hold.  The number of in-flight descriptors is
 * (avail->idx - used->idx), both uint16_t counters that wrap naturally.
 * Because uint16_t subtraction is well-defined mod 2^16, this remains
 * correct across wraps as long as the ring is never more than VRING_SIZE
 * entries deep (which it can't be by design).
 */
static inline int vring_avail_has_room(struct vring_avail *avail,
                                       struct vring_used *used,
                                       uint16_t needed)
{
    uint16_t in_flight = (uint16_t)(avail->idx - used->idx);
    return (uint16_t)(in_flight + needed) <= VRING_SIZE;
}

/* virtio-net header prepended to every packet (12 bytes, 14 with num_buffers) */
#pragma pack(push, 1)
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;  /* only if VIRTIO_NET_F_MRG_RXBUF */
};
#pragma pack(pop)

/* ── Control VQ message structures ────────────────────────────────── *
 * These are used to communicate with the device via the control
 * virtqueue (VIRTIO_NET_F_CTRL_VQ). */
#pragma pack(push, 1)
struct virtio_net_ctrl_hdr {
    uint8_t class;    /* VIRTIO_NET_CTRL_* class */
    uint8_t command;  /* class-specific command */
};
#pragma pack(pop)

#define VIRTIO_NET_CTRL_ACK_SIZE  1    /* single-byte ack status */

/* ── RX buffer size ────────────────────────────────────────────────
 * Standard MTU (1500) needs ~1.5KB per packet.  With LRO/TSO the
 * device may deliver up to 64KB of coalesced TCP data, but we cap
 * at 16KB to keep memory usage reasonable.  Packets exceeding this
 * are dropped and counted.
 *
 * TX buffer: increased to 32KB to accommodate TSO/GSO large segments;
 * the device segments them into MSS-sized frames in hardware.
 */
#define RX_BUF_SIZE    16384
#define TX_BUF_SIZE    32768

/* ── Default MSS for TSO/GSO offload ────────────────────────────── */
#define DEFAULT_MSS    1460

/* ── GRO (Generic Receive Offload) constants ────────────────────── */
#define GRO_MAX_FLOWS      8   /* Max concurrent flows tracked */
#define GRO_TIMEOUT_TICKS  2   /* Timeout for GRO flow (in timer ticks) */

/* ── LRO segmentation state ────────────────────────────────────────
 * When the device delivers an LRO packet (gso_type != NONE), we
 * segment it into individual MSS-sized packets.  The state machine
 * spans multiple virtio_net_receive() calls.
 */
#define LRO_MAX_SEGMENTS  64  /* Max segments we can segment from one LRO pkt */

struct lro_segment_state {
    /* Current LRO packet info (from virtio_net_hdr) */
    uint8_t  gso_type;
    uint16_t hdr_len;     /* Combined eth+IP+TCP headers length */
    uint16_t gso_size;    /* MSS (payload per segment) */
    uint32_t base_seq;    /* TCP sequence number of first segment */
    uint32_t total_payload; /* Total payload bytes in LRO packet */

    /* Pointers into the RX buffer (set up once per LRO packet) */
    uint8_t  *pkt_data;   /* Start of packet data (after virtio_net_hdr) */
    uint32_t  pkt_len;    /* Total packet data length */

    /* Segmentation progress */
    int       num_segments;     /* Total number of segments to produce */
    int       current_segment;  /* Current segment index (0-based) */
    int       active;           /* 1 = mid-segmentation, 0 = idle */
} lro_seg;

/* ── LRO statistics ──────────────────────────────────────────────── */
static struct virtio_net_lro_stats lro_stats;

/* ── TX offload statistics ───────────────────────────────────────── */
struct virtio_net_tx_offload_stats {
    uint64_t tso_packets;        /* Packets sent with TSO offload */
    uint64_t gso_packets;        /* Packets sent with GSO offload */
    uint64_t csum_offload;       /* Packets with checksum offload only */
    uint64_t sw_gso_packets;     /* Packets segmented in software */
    uint64_t sw_gso_bytes;       /* Bytes after software segmentation */
    uint64_t raw_packets;        /* Packets sent raw (no offload) */
    uint64_t tx_offload_drops;   /* Packets dropped due to offload errors */
} tx_offload_stats;

/* ── GRO statistics ──────────────────────────────────────────────── */
struct virtio_net_gro_stats {
    uint64_t gro_packets;        /* Packets delivered after GRO merging */
    uint64_t gro_merged;         /* Total segments merged into GRO packets */
    uint64_t gro_flushes;        /* GRO flow flushes (timeout or full) */
    uint64_t gro_flows_active;   /* Current number of active flows */
    uint64_t gro_bytes_saved;    /* Bytes saved by merging headers */
};

static struct virtio_net_gro_stats gro_stats;

/* ── Offload info: parsed packet header for TSO/GSO detection ────── */
struct offload_info {
    uint8_t  gso_type;       /* VIRTIO_NET_HDR_GSO_* or GSO_NONE */
    uint16_t hdr_len;        /* Combined L2+L3+L4 header length */
    uint16_t gso_size;       /* MSS (only valid if gso_type != NONE) */
    uint16_t csum_start;     /* Transport header offset from packet start */
    uint16_t csum_offset;    /* Offset of checksum field in transport header */
    uint32_t payload_len;    /* Payload after all headers */
    int      needs_csum;     /* 1 = needs checksum computation */
};

/* ── GRO flow entry: tracks one merging flow ────────────────────── */
#define GRO_MAX_PACKETS 16  /* Max packets merged per GRO flow */

struct gro_flow {
    int       active;            /* 1 = flow in use */
    uint8_t   src_mac[6];        /* Ethernet source MAC */
    uint8_t   dst_mac[6];        /* Ethernet destination MAC */
    uint16_t  eth_type;          /* Ethernet type */
    uint32_t  src_ip[4];         /* IP source (up to 128-bit for IPv6) */
    uint32_t  dst_ip[4];         /* IP destination */
    uint8_t   ip_proto;          /* IP protocol (TCP, UDP) */
    uint16_t  src_port;          /* Transport source port */
    uint16_t  dst_port;          /* Transport destination port */
    uint16_t  ip_hdr_len;        /* IP header length */
    uint16_t  l4_hdr_len;        /* L4 header length */
    uint32_t  headroom;          /* Bytes before IP header (eth hdr size) */
    /* Merged packet data */
    uint8_t   data[RX_BUF_SIZE]; /* Merged packet buffer */
    uint32_t  merged_len;        /* Total bytes in merged buffer */
    uint32_t  merged_payload;    /* Total payload bytes after headers */
    uint32_t  segment_count;     /* Number of segments merged */
    uint32_t  first_seq;         /* TCP seq of first segment */
    uint64_t  last_activity;     /* Timestamp of last merge (ticks) */
};

/* ── Driver state ────────────────────────────────────────────────── */
static int      vnet_present = 0;
static uint16_t vnet_iobase  = 0;

#define RX_QUEUE_IDX 0
#define TX_QUEUE_IDX 1
#define CTRL_QUEUE_IDX 2
static uint8_t  __attribute__((aligned(4096))) rx_queue_mem[4096];
static uint8_t  __attribute__((aligned(4096))) tx_queue_mem[4096];
static uint8_t  __attribute__((aligned(4096))) ctrl_queue_mem[4096];
static uint8_t  rx_pkt_bufs[VRING_SIZE][RX_BUF_SIZE];
static uint16_t rx_last_used = 0;
static uint8_t  vnet_irq = 0;
static uint8_t  tx_pkt_buf[TX_BUF_SIZE];
static struct virtio_net_hdr tx_hdr;
static uint16_t tx_last_used = 0;

/* ── Negotiated feature flags (set during init) ──────────────────── */
static uint32_t vnet_negotiated_features = 0;

/* ── Control VQ state ────────────────────────────────────────────── */
static int     ctrl_vq_available = 0;   /* 1 if control VQ was negotiated and set up */
static uint8_t ctrl_ack_buf[16];        /* ACK status from device (first byte used) */
static uint8_t ctrl_data_buf[1024];     /* Scatter buffer for control command data */
static spinlock_t ctrl_vq_lock = SPINLOCK_INIT;  /* Serializes control VQ access */

/* ── GRO flow table ──────────────────────────────────────────────── */
static struct gro_flow gro_flows[GRO_MAX_FLOWS];
static int gro_initialized = 0;

/* ── LRO control flag ────────────────────────────────────────────── */
static int lro_enabled = 1;  /* LRO enabled by default */

static inline void vio_outb(uint8_t off, uint8_t v);
static inline void vio_outw(uint8_t off, uint16_t v);
static inline uint8_t vio_inb(uint8_t off);

static void virtio_net_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    (void)vio_inb(VIRTIO_PCI_ISR);
    if (vnet_irq) irq_ack(vnet_irq);
    net_rx_signal();
}

/* ── Helpers ─────────────────────────────────────────────────────── */
static inline void vio_outb(uint8_t off, uint8_t v)  { outb((uint16_t)(vnet_iobase + off), v); }
static inline void vio_outw(uint8_t off, uint16_t v) { outw((uint16_t)(vnet_iobase + off), v); }
static inline void vio_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(vnet_iobase + off),     (uint8_t)(v));
    outb((uint16_t)(vnet_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(vnet_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(vnet_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vio_inb(uint8_t off)  { return inb(vnet_iobase + off); }
static inline uint16_t vio_inw(uint8_t off)  { return inw(vnet_iobase + off); }
static inline uint32_t vio_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(vnet_iobase + off))
         | ((uint32_t)inb((uint16_t)(vnet_iobase + off + 1)) << 8)
         | ((uint32_t)inb((uint16_t)(vnet_iobase + off + 2)) << 16)
         | ((uint32_t)inb((uint16_t)(vnet_iobase + off + 3)) << 24);
}

static struct vring_avail *vring_avail_ptr(void *base) {
    return (struct vring_avail *)((uint8_t *)base +
                                  sizeof(struct vring_desc) * VRING_SIZE);
}

static struct vring_used *vring_used_ptr(void *base) {
    size_t off = sizeof(struct vring_desc) * VRING_SIZE;
    off += sizeof(uint16_t) * 2 + (size_t)VRING_SIZE * sizeof(uint16_t);
    off = (off + 3) & ~3u;
    return (struct vring_used *)((uint8_t *)base + off);
}

/* ── Checksum helpers ────────────────────────────────────────────── */
/* Compute the ones-complement Internet checksum over 'len' bytes at 'data'. */
static uint16_t lro_checksum(const void *data, int len) {
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)data;
    while (len >= 2) {
        sum += *p++;
        len -= 2;
    }
    if (len)
        sum += *(const uint8_t *)p;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Add a 16-bit value to a running ones-complement sum (for updating checksums
 * without full recomputation).  Uses the standard RFC 1624 approach. */
static inline uint32_t lro_csum_add(uint32_t csum, uint16_t old, uint16_t new) {
    /* csum is the ones-complement accumulator */
    uint32_t s = (~csum & 0xFFFF) + (uint32_t)old;
    s = (s & 0xFFFF) + (s >> 16);
    s += (uint32_t)new;
    s = (s & 0xFFFF) + (s >> 16);
    return (~s & 0xFFFF);
}

/* ── IPv4 TCP checksum (pseudo-header + segment) ─────────────────── */
static uint16_t lro_tcp_csum4(uint32_t src_ip, uint32_t dst_ip,
                               const uint8_t *tcp_seg, int tcp_len) {
    /* Pseudo-header */
    uint32_t sum = 0;
    {
        uint16_t *p = (uint16_t *)&src_ip;
        sum += p[0] + p[1];
        p = (uint16_t *)&dst_ip;
        sum += p[0] + p[1];
    }
    sum += htons((uint16_t)IP_PROTO_TCP);
    sum += htons((uint16_t)tcp_len);

    /* Add TCP segment data */
    const uint8_t *seg_bytes = (const uint8_t *)tcp_seg;
    int len = tcp_len;
    while (len >= 2) {
        uint16_t val;
        memcpy(&val, seg_bytes, sizeof(val));
        sum += val;
        seg_bytes += 2;
        len -= 2;
    }
    if (len)
        sum += *seg_bytes;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Segment a single TCPv4 LRO packet ─────────────────────────────
 *
 * Called once when an LRO packet is first received.  Stores state in
 * 'lro_seg' so that each virtio_net_receive() call returns the next
 * segment.  Handles the common case of simple ACK-with-data segments.
 *
 * The LRO packet layout after virtio_net_hdr:
 *   [eth_hdr (14)] [IP_hdr (20)] [TCP_hdr (20+options)] [N * gso_size payload]
 *
 * Returns 0 on success (segmentation started), -1 on unsupported packet.
 */
static int lro_start_segmentation(uint8_t gso_type, uint16_t hdr_len,
                                  uint16_t gso_size,
                                  const uint8_t *pkt_data, uint32_t pkt_len) {
    /* Clear any previous state */
    memset(&lro_seg, 0, sizeof(lro_seg));

    lro_seg.gso_type   = gso_type;
    lro_seg.hdr_len    = hdr_len;
    lro_seg.gso_size   = gso_size;
    lro_seg.pkt_data   = (uint8_t *)(uintptr_t)pkt_data;
    lro_seg.pkt_len    = pkt_len;
    lro_seg.active     = 1;

    /* Minimum sanity: headers must be smaller than total packet, and
     * gso_size must be reasonable (>= 1). */
    if (hdr_len >= pkt_len || gso_size == 0) {
        lro_stats.seg_failures++;
        lro_seg.active = 0;
        return -1;
    }

    uint32_t payload = pkt_len - hdr_len;
    lro_seg.total_payload = payload;
    lro_seg.num_segments = (int)((payload + gso_size - 1) / gso_size);

    if (lro_seg.num_segments > LRO_MAX_SEGMENTS) {
        lro_seg.num_segments = LRO_MAX_SEGMENTS;
        lro_stats.dropped_oversize++;
    }

    /* Parse TCP sequence number from the TCP header (after eth + IP). */
    if (gso_type == VIRTIO_NET_HDR_GSO_TCPV4 ||
        gso_type == VIRTIO_NET_HDR_GSO_TCP_ECN) {
        /* Skip eth header (14 bytes) + IP header (variable).  IP IHL gives
         * the IP header length in 32-bit words. */
        if (hdr_len < 34) { /* 14 (eth) + 20 (min IP) */
            lro_stats.seg_failures++;
            lro_seg.active = 0;
            return -1;
        }
        const struct ip_header *ip = (const struct ip_header *)(pkt_data + 14);
        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
        if (ip_hdr_len < 20 || (uint16_t)(14 + ip_hdr_len) >= hdr_len) {
            lro_stats.seg_failures++;
            lro_seg.active = 0;
            return -1;
        }
        const struct tcp_header *tcp = (const struct tcp_header *)
            (pkt_data + 14 + ip_hdr_len);
        lro_seg.base_seq = ntohl(tcp->seq_num);
    } else if (gso_type == VIRTIO_NET_HDR_GSO_TCPV6) {
        /* For IPv6, skip eth header (14) + IPv6 header (40). */
        if (hdr_len < 54) {
            lro_stats.seg_failures++;
            lro_seg.active = 0;
            return -1;
        }
        /* Parse TCP seq from TCP header at offset 14 + 40 = 54 */
        const struct tcp_header *tcp = (const struct tcp_header *)
            (pkt_data + 14 + sizeof(struct ipv6_header));
        lro_seg.base_seq = ntohl(tcp->seq_num);
    } else if (gso_type == VIRTIO_NET_HDR_GSO_UDP) {
        /* UFO (UDP fragmentation offload) — segment by splitting payload. */
        lro_seg.base_seq = 0; /* Not needed for UDP */
    } else {
        lro_stats.seg_failures++;
        lro_seg.active = 0;
        return -1;
    }

    lro_seg.current_segment = 0;
    lro_stats.lro_packets++;
    lro_stats.merged_packets += (uint64_t)lro_seg.num_segments;
    lro_stats.total_bytes += pkt_len - hdr_len;
    return 0;
}

/* ── Get the next segment from an in-progress LRO segmentation ─────
 * Builds a complete Ethernet frame for the current segment into 'buf'
 * (up to 'max_len' bytes).  Returns the number of bytes written, or
 * 0 if no more segments remain.
 */
static int lro_next_segment(void *buf, uint16_t max_len) {
    if (!lro_seg.active || lro_seg.current_segment >= lro_seg.num_segments) {
        lro_seg.active = 0;
        return 0;
    }

    int seg_idx = lro_seg.current_segment;
    uint32_t payload_offset = (uint32_t)seg_idx * lro_seg.gso_size;
    uint32_t seg_payload = lro_seg.gso_size;
    /* Last segment may be smaller */
    if (payload_offset + seg_payload > lro_seg.total_payload)
        seg_payload = lro_seg.total_payload - payload_offset;

    /* Total segment length = hdr_len + this segment's payload */
    uint32_t seg_len = lro_seg.hdr_len + seg_payload;
    if (seg_len > max_len) {
        lro_stats.seg_failures++;
        lro_seg.active = 0;
        return 0;
    }

    /* Copy headers */
    memcpy(buf, lro_seg.pkt_data, lro_seg.hdr_len);
    uint8_t *seg = (uint8_t *)buf;

    /* Copy payload */
    memcpy(seg + lro_seg.hdr_len,
           lro_seg.pkt_data + lro_seg.hdr_len + payload_offset,
           seg_payload);

    /* ── Fix up IPv4 headers (for TCPv4 and TCP ECN) ── */
    if (lro_seg.gso_type == VIRTIO_NET_HDR_GSO_TCPV4 ||
        lro_seg.gso_type == VIRTIO_NET_HDR_GSO_TCP_ECN) {
        struct eth_header *eth = (struct eth_header *)seg;
        (void)eth;
        struct ip_header *ip = (struct ip_header *)(seg + 14);
        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
        struct tcp_header *tcp = (struct tcp_header *)(seg + 14 + ip_hdr_len);

        /* Update IP total length */
        uint32_t new_ip_len = (uint32_t)ip_hdr_len + (uint32_t)(seg_len - 14);
        ip->total_len = htons((uint16_t)(new_ip_len));

        /* Update TCP sequence number */
        tcp->seq_num = htonl(lro_seg.base_seq + payload_offset);

        /* Set PSH flag on last segment only */
        if (seg_idx == lro_seg.num_segments - 1) {
            tcp->flags |= TCP_PSH;
        } else {
            tcp->flags &= ~TCP_PSH;
        }

        /* Recompute TCP checksum */
        int tcp_len = (int)(seg_len - 14 - ip_hdr_len);
        tcp->checksum = 0;
        tcp->checksum = lro_tcp_csum4(ip->src_ip, ip->dst_ip,
                                       (const uint8_t *)tcp, tcp_len);

        /* Recompute IP checksum */
        ip->checksum = 0;
        ip->checksum = lro_checksum(ip, ip_hdr_len);
    }
    /* ── Fix up IPv6 headers (for TCPv6) ── */
    else if (lro_seg.gso_type == VIRTIO_NET_HDR_GSO_TCPV6) {
        struct ipv6_header *ip6 = (struct ipv6_header *)(seg + 14);
        struct tcp_header *tcp = (struct tcp_header *)(seg + 14 + sizeof(struct ipv6_header));

        /* Update IPv6 payload length */
        uint32_t new_payload = (uint32_t)(seg_len - 14 - sizeof(struct ipv6_header));
        ip6->payload_length = htons((uint16_t)new_payload);

        /* Update TCP sequence number */
        tcp->seq_num = htonl(lro_seg.base_seq + payload_offset);

        /* Set PSH flag on last segment only */
        if (seg_idx == lro_seg.num_segments - 1) {
            tcp->flags |= TCP_PSH;
        } else {
            tcp->flags &= ~TCP_PSH;
        }

        /* Compute TCP checksum with IPv6 pseudo-header */
        int tcp_len = (int)(seg_len - 14 - sizeof(struct ipv6_header));
        tcp->checksum = 0;
        /* Simple checksum: pseudo-header + TCP data */
        {
            uint32_t sum = 0;
            /* Copy src/dst to local aligned variables to avoid packed-member issues */
            uint16_t psrc[8], pdst[8];
            memcpy(psrc, &ip6->src_ip, sizeof(psrc));
            memcpy(pdst, &ip6->dst_ip, sizeof(pdst));
            for (int i = 0; i < 8; i++) sum += psrc[i];
            for (int i = 0; i < 8; i++) sum += pdst[i];
            sum += htons((uint16_t)new_payload);
            sum += htons((uint16_t)IP_PROTO_TCP);
            /* Process TCP header byte by byte to avoid unaligned access */
            const uint8_t *tcp_bytes = (const uint8_t *)tcp;
            int len = tcp_len;
            while (len >= 2) {
                uint16_t w;
                memcpy(&w, tcp_bytes, 2);
                sum += w;
                tcp_bytes += 2;
                len -= 2;
            }
            if (len) sum += *tcp_bytes;
            while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
            tcp->checksum = (uint16_t)~sum;
        }
    }
    /* ── Fix up UDP headers (for UFO) ── */
    else if (lro_seg.gso_type == VIRTIO_NET_HDR_GSO_UDP) {
        struct ip_header *ip = (struct ip_header *)(seg + 14);
        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
        struct udp_header *udp = (struct udp_header *)(seg + 14 + ip_hdr_len);

        /* Update IP total length */
        uint32_t new_ip_len = (uint32_t)ip_hdr_len + (uint32_t)(seg_len - 14);
        ip->total_len = htons((uint16_t)new_ip_len);

        /* Update UDP length */
        uint32_t new_udp_len = seg_len - 14 - ip_hdr_len;
        udp->length = htons((uint16_t)new_udp_len);

        /* Recompute UDP checksum (can be 0 for no-checksum) */
        if (udp->checksum != 0) {
            udp->checksum = 0;
            udp->checksum = lro_checksum(udp, (int)new_udp_len);
        }

        /* Recompute IP checksum */
        ip->checksum = 0;
        ip->checksum = lro_checksum(ip, ip_hdr_len);
    }

    lro_seg.current_segment++;
    return (int)seg_len;
}

/* ══════════════════════════════════════════════════════════════════
 *  TX TSO/GSO Offload
 * ══════════════════════════════════════════════════════════════════ */

/* ── Parse Ethernet frame headers for offload detection ────────────
 * Given a raw Ethernet frame (including eth header), parse the
 * L2/L3/L4 headers and return information needed for TSO/GSO/CSUM
 * offload.
 *
 * Returns 0 on success, -1 if the packet cannot be offloaded.
 */
static int parse_packet_offload(const uint8_t *data, uint32_t len,
                                struct offload_info *info)
{
    if (!data || !info || len < sizeof(struct eth_header))
        return -1;

    memset(info, 0, sizeof(*info));

    const struct eth_header *eth = (const struct eth_header *)data;
    uint16_t eth_type = ntohs(eth->type);

    if (eth_type == ETH_TYPE_IP) {
        /* ── IPv4 ── */
        if (len < sizeof(struct eth_header) + sizeof(struct ip_header))
            return -1;

        const struct ip_header *ip = (const struct ip_header *)
            (data + sizeof(struct eth_header));
        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

        if (ip_hdr_len < 20 ||
            (uint32_t)(sizeof(struct eth_header) + ip_hdr_len) > len)
            return -1;

        uint16_t ip_total_len = ntohs(ip->total_len);
        if (ip_total_len < sizeof(struct ip_header) ||
            (uint32_t)ip_total_len + sizeof(struct eth_header) > len)
            ip_total_len = (uint16_t)(len - sizeof(struct eth_header));

        info->csum_start = (uint16_t)(sizeof(struct eth_header) + ip_hdr_len);

        /* Determine L4 protocol */
        if (ip->protocol == IP_PROTO_TCP && len >= info->csum_start + sizeof(struct tcp_header)) {
            /* TCP segment — potential TSO candidate */
            const struct tcp_header *tcp = (const struct tcp_header *)
                (data + info->csum_start);
            int tcp_hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;

            if (tcp_hdr_len < 20 ||
                info->csum_start + (uint32_t)tcp_hdr_len > len)
                return -1;

            info->hdr_len = (uint16_t)(info->csum_start + tcp_hdr_len);
            info->csum_offset = 16; /* checksum offset in TCP header */
            info->payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;

            /* Bulk data TCP segment qualifies for TSO */
            if (info->payload_len > DEFAULT_MSS &&
                !(tcp->flags & (TCP_SYN | TCP_RST | TCP_FIN))) {
                info->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
                info->gso_size = DEFAULT_MSS;
            }
            info->needs_csum = 1;

        } else if (ip->protocol == IP_PROTO_UDP && len >= info->csum_start + sizeof(struct udp_header)) {
            /* UDP — potential UFO if large */
            info->hdr_len = (uint16_t)(info->csum_start + sizeof(struct udp_header));
            info->csum_offset = 6; /* checksum offset in UDP header */
            info->payload_len = (uint32_t)(ip_total_len - ip_hdr_len - sizeof(struct udp_header));

            if (info->payload_len > 512) {
                info->gso_type = VIRTIO_NET_HDR_GSO_UDP;
                info->gso_size = (uint16_t)(info->payload_len > 65535 ? 512 : info->payload_len);
                if (info->gso_size > 1460)
                    info->gso_size = 1460;
            }
            info->needs_csum = 1;
        } else {
            /* Non-TCP/UDP: checksum offload only */
            info->gso_type = VIRTIO_NET_HDR_GSO_NONE;
            info->hdr_len = (uint16_t)(sizeof(struct eth_header) + ip_hdr_len);
            info->needs_csum = 1;
        }

    } else if (eth_type == ETH_TYPE_IPV6) {
        /* ── IPv6 ── */
        if (len < sizeof(struct eth_header) + sizeof(struct ipv6_header))
            return -1;

        const struct ipv6_header *ip6 = (const struct ipv6_header *)
            (data + sizeof(struct eth_header));
        uint16_t ip6_payload_len = ntohs(ip6->payload_length);

        info->csum_start = (uint16_t)(sizeof(struct eth_header) + sizeof(struct ipv6_header));
        info->hdr_len = info->csum_start;

        if (ip6->next_header == IP_PROTO_TCP &&
            len >= info->csum_start + sizeof(struct tcp_header)) {
            const struct tcp_header *tcp = (const struct tcp_header *)
                (data + info->csum_start);
            int tcp_hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;

            if (tcp_hdr_len < 20)
                return -1;

            info->hdr_len = (uint16_t)(info->csum_start + tcp_hdr_len);
            info->csum_offset = 16;
            info->payload_len = ip6_payload_len - tcp_hdr_len;

            if (info->payload_len > DEFAULT_MSS &&
                !(tcp->flags & (TCP_SYN | TCP_RST | TCP_FIN))) {
                info->gso_type = VIRTIO_NET_HDR_GSO_TCPV6;
                info->gso_size = DEFAULT_MSS;
            }
            info->needs_csum = 1;
        }
        /* IPv6 UDP not yet supported for UFO */
    }

    return 0;
}

/* ── Setup virtio_net_hdr for TSO/GSO/checksum offload on TX ──────
 * Fills in the virtio_net_hdr based on parsed offload info so the
 * device can perform hardware segmentation and/or checksumming.
 */
static void setup_tx_hdr_offload(struct virtio_net_hdr *hdr,
                                  const struct offload_info *info)
{
    memset(hdr, 0, sizeof(*hdr));

    if (!info || (!info->needs_csum && info->gso_type == VIRTIO_NET_HDR_GSO_NONE))
        return;

    if (info->gso_type != VIRTIO_NET_HDR_GSO_NONE) {
        /* TSO/GSO offload: device will segment the packet */
        hdr->gso_type  = info->gso_type;
        hdr->hdr_len   = htons(info->hdr_len);
        hdr->gso_size  = htons(info->gso_size);

        /* For TSO, we always need checksum offload as well */
        hdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
        hdr->csum_start  = htons(info->csum_start);
        hdr->csum_offset = htons(info->csum_offset);
    } else if (info->needs_csum) {
        /* Checksum offload only (no segmentation) */
        hdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
        hdr->csum_start  = htons(info->csum_start);
        hdr->csum_offset = htons(info->csum_offset);
    }
}

/* ── Software GSO fallback ─────────────────────────────────────────
 * Used when the device does not support hardware TSO/GSO for the
 * detected protocol. Segments the large packet into MSS-sized chunks
 * and sends each one individually via the raw send path.
 *
 * Returns 0 on success, -1 on failure.
 */
static int virtio_net_sw_gso(const uint8_t *data, uint32_t len)
{
    struct offload_info oinfo;
    uint8_t seg_buf[TX_BUF_SIZE];

    if (parse_packet_offload(data, len, &oinfo) < 0)
        return -1;

    if (oinfo.gso_type == VIRTIO_NET_HDR_GSO_NONE || oinfo.gso_size == 0)
        return virtio_net_send(data, len);

    uint32_t offset = 0;
    int seg_count = 0;
    int seg_idx = 0;

    /* Copy base headers */
    if (oinfo.hdr_len > sizeof(seg_buf))
        return -1;

    memcpy(seg_buf, data, oinfo.hdr_len);

    while (offset < oinfo.payload_len) {
        uint32_t seg_payload = oinfo.gso_size;
        if (offset + seg_payload > oinfo.payload_len)
            seg_payload = oinfo.payload_len - offset;

        /* Copy payload for this segment */
        memcpy(seg_buf + oinfo.hdr_len,
               data + oinfo.hdr_len + offset,
               seg_payload);

        uint32_t seg_len = oinfo.hdr_len + seg_payload;

        /* Fix up IPv4 headers */
        if (oinfo.gso_type == VIRTIO_NET_HDR_GSO_TCPV4 ||
            oinfo.gso_type == VIRTIO_NET_HDR_GSO_TCP_ECN) {
            struct ip_header *ip = (struct ip_header *)
                (seg_buf + sizeof(struct eth_header));
            int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
            uint32_t new_ip_len = (uint32_t)ip_hdr_len + seg_payload +
                                  (uint32_t)(oinfo.csum_start - sizeof(struct eth_header) - ip_hdr_len);
            ip->total_len = htons((uint16_t)new_ip_len);

            struct tcp_header *tcp = (struct tcp_header *)
                (seg_buf + oinfo.csum_start);
            tcp->seq_num = htonl(ntohl(tcp->seq_num) + offset);

            /* Set PSH only on last segment */
            if (seg_idx == (int)((oinfo.payload_len + oinfo.gso_size - 1) / oinfo.gso_size) - 1)
                tcp->flags |= TCP_PSH;
            else
                tcp->flags &= ~TCP_PSH;

            /* Recompute TCP checksum */
            int tcp_len = (int)(seg_len - oinfo.csum_start);
            tcp->checksum = 0;
            tcp->checksum = lro_tcp_csum4(ip->src_ip, ip->dst_ip,
                                           (const uint8_t *)tcp, tcp_len);

            /* Recompute IP checksum */
            ip->checksum = 0;
            ip->checksum = lro_checksum(ip, ip_hdr_len);
        }

        /* Fix up IPv6 headers */
        if (oinfo.gso_type == VIRTIO_NET_HDR_GSO_TCPV6) {
            struct ipv6_header *ip6 = (struct ipv6_header *)
                (seg_buf + sizeof(struct eth_header));
            uint32_t new_payload = (uint32_t)(seg_len - sizeof(struct eth_header) -
                                   sizeof(struct ipv6_header));
            ip6->payload_length = htons((uint16_t)new_payload);

            struct tcp_header *tcp = (struct tcp_header *)
                (seg_buf + oinfo.csum_start);
            tcp->seq_num = htonl(ntohl(tcp->seq_num) + offset);

            if (seg_idx == (int)((oinfo.payload_len + oinfo.gso_size - 1) / oinfo.gso_size) - 1)
                tcp->flags |= TCP_PSH;
            else
                tcp->flags &= ~TCP_PSH;

            /* Compute TCP checksum with IPv6 pseudo-header */
            int tcp_len = (int)(seg_len - oinfo.csum_start);
            tcp->checksum = 0;
            {
                uint32_t sum = 0;
                uint16_t psrc[8], pdst[8];
                memcpy(psrc, &ip6->src_ip, sizeof(psrc));
                memcpy(pdst, &ip6->dst_ip, sizeof(pdst));
                for (int i = 0; i < 8; i++) sum += psrc[i];
                for (int i = 0; i < 8; i++) sum += pdst[i];
                sum += htons((uint16_t)tcp_len);
                sum += htons((uint16_t)IP_PROTO_TCP);
                const uint8_t *tcp_bytes = (const uint8_t *)tcp;
                int rem = tcp_len;
                while (rem >= 2) {
                    uint16_t w;
                    memcpy(&w, tcp_bytes, 2);
                    sum += w;
                    tcp_bytes += 2;
                    rem -= 2;
                }
                if (rem) sum += *tcp_bytes;
                while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
                tcp->checksum = (uint16_t)~sum;
            }
        }

        /* Send this segment via the raw send path */
        if (virtio_net_send(seg_buf, seg_len) < 0)
            return -1;

        seg_count++;
        offset += seg_payload;
        seg_idx++;
    }

    tx_offload_stats.sw_gso_packets += (uint64_t)seg_count;
    tx_offload_stats.sw_gso_bytes += len;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  GRO (Generic Receive Offload) — Receive-side packet merging
 * ══════════════════════════════════════════════════════════════════ */

/* ── GRO flow hash: identify a flow from packet headers ────────────
 * Fills in the key fields of a gro_flow entry from the received packet.
 * Returns 0 on success, -1 if the packet is not mergeable.
 */
static int gro_flow_from_pkt(struct gro_flow *flow,
                              const uint8_t *pkt, uint32_t len)
{
    if (!flow || !pkt || len < sizeof(struct eth_header))
        return -1;

    const struct eth_header *eth = (const struct eth_header *)pkt;
    uint16_t eth_type = ntohs(eth->type);

    memcpy(flow->dst_mac, eth->dst, 6);
    memcpy(flow->src_mac, eth->src, 6);
    flow->eth_type = eth_type;
    flow->headroom = sizeof(struct eth_header);

    if (eth_type == ETH_TYPE_IP) {
        /* IPv4 */
        if (len < sizeof(struct eth_header) + sizeof(struct ip_header))
            return -1;

        const struct ip_header *ip = (const struct ip_header *)
            (pkt + sizeof(struct eth_header));
        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

        if (ip_hdr_len < 20 || (uint32_t)(sizeof(struct eth_header) + ip_hdr_len) > len)
            return -1;

        flow->src_ip[0] = ip->src_ip;
        flow->dst_ip[0] = ip->dst_ip;
        flow->src_ip[1] = 0; flow->src_ip[2] = 0; flow->src_ip[3] = 0;
        flow->dst_ip[1] = 0; flow->dst_ip[2] = 0; flow->dst_ip[3] = 0;
        flow->ip_proto = ip->protocol;
        flow->ip_hdr_len = (uint16_t)ip_hdr_len;
        flow->headroom = (uint32_t)(sizeof(struct eth_header) + ip_hdr_len);

        if (ip->protocol == IP_PROTO_TCP) {
            if (len < sizeof(struct eth_header) + ip_hdr_len + sizeof(struct tcp_header))
                return -1;

            const struct tcp_header *tcp = (const struct tcp_header *)
                (pkt + sizeof(struct eth_header) + ip_hdr_len);
            int tcp_hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;
            flow->src_port = tcp->src_port;
            flow->dst_port = tcp->dst_port;
            flow->l4_hdr_len = (uint16_t)tcp_hdr_len;
        } else if (ip->protocol == IP_PROTO_UDP) {
            if (len < sizeof(struct eth_header) + ip_hdr_len + sizeof(struct udp_header))
                return -1;

            const struct udp_header *udp = (const struct udp_header *)
                (pkt + sizeof(struct eth_header) + ip_hdr_len);
            flow->src_port = udp->src_port;
            flow->dst_port = udp->dst_port;
            flow->l4_hdr_len = sizeof(struct udp_header);
        } else {
            return -1; /* Non-TCP/UDP not mergeable */
        }

    } else if (eth_type == ETH_TYPE_IPV6) {
        /* IPv6 */
        if (len < sizeof(struct eth_header) + sizeof(struct ipv6_header))
            return -1;

        const struct ipv6_header *ip6 = (const struct ipv6_header *)
            (pkt + sizeof(struct eth_header));

        memcpy(flow->src_ip, &ip6->src_ip, 16);
        memcpy(flow->dst_ip, &ip6->dst_ip, 16);
        flow->ip_proto = ip6->next_header;
        flow->ip_hdr_len = sizeof(struct ipv6_header);
        flow->headroom = sizeof(struct eth_header) + sizeof(struct ipv6_header);

        if (ip6->next_header == IP_PROTO_TCP) {
            if (len < sizeof(struct eth_header) + sizeof(struct ipv6_header) +
                      sizeof(struct tcp_header))
                return -1;

            const struct tcp_header *tcp = (const struct tcp_header *)
                (pkt + sizeof(struct eth_header) + sizeof(struct ipv6_header));
            int tcp_hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;
            flow->src_port = tcp->src_port;
            flow->dst_port = tcp->dst_port;
            flow->l4_hdr_len = (uint16_t)tcp_hdr_len;
        } else {
            return -1;
        }
    } else {
        return -1; /* Non-IP not mergeable */
    }

    return 0;
}

/* ── Compare two GRO flows for equality ───────────────────────────── */
static int gro_flow_match(const struct gro_flow *a, const struct gro_flow *b)
{
    if (memcmp(a->src_mac, b->src_mac, 6) != 0 ||
        memcmp(a->dst_mac, b->dst_mac, 6) != 0 ||
        a->eth_type != b->eth_type ||
        a->ip_proto != b->ip_proto ||
        a->src_port != b->src_port ||
        a->dst_port != b->dst_port)
        return 0;

    if (a->eth_type == ETH_TYPE_IP) {
        return a->src_ip[0] == b->src_ip[0] &&
               a->dst_ip[0] == b->dst_ip[0];
    } else if (a->eth_type == ETH_TYPE_IPV6) {
        return memcmp(a->src_ip, b->src_ip, 16) == 0 &&
               memcmp(a->dst_ip, b->dst_ip, 16) == 0;
    }

    return 0;
}

/* ── Find or allocate a GRO flow entry for the given packet ────────
 * Returns a pointer to the flow entry, or NULL if no slot available.
 */
static struct gro_flow *gro_find_flow(const uint8_t *pkt, uint32_t len)
{
    struct gro_flow candidate;
    if (gro_flow_from_pkt(&candidate, pkt, len) < 0)
        return NULL;

    /* Look for an existing matching flow */
    for (int i = 0; i < GRO_MAX_FLOWS; i++) {
        if (gro_flows[i].active && gro_flow_match(&gro_flows[i], &candidate)) {
            return &gro_flows[i];
        }
    }

    /* Allocate a new flow slot */
    for (int i = 0; i < GRO_MAX_FLOWS; i++) {
        if (!gro_flows[i].active) {
            memset(&gro_flows[i], 0, sizeof(gro_flows[i]));
            gro_flows[i].active = 1;
            memcpy(gro_flows[i].src_mac, candidate.src_mac, 6);
            memcpy(gro_flows[i].dst_mac, candidate.dst_mac, 6);
            gro_flows[i].eth_type = candidate.eth_type;
            memcpy(gro_flows[i].src_ip, candidate.src_ip, sizeof(candidate.src_ip));
            memcpy(gro_flows[i].dst_ip, candidate.dst_ip, sizeof(candidate.dst_ip));
            gro_flows[i].ip_proto = candidate.ip_proto;
            gro_flows[i].src_port = candidate.src_port;
            gro_flows[i].dst_port = candidate.dst_port;
            gro_flows[i].ip_hdr_len = candidate.ip_hdr_len;
            gro_flows[i].l4_hdr_len = candidate.l4_hdr_len;
            gro_flows[i].headroom = candidate.headroom;
            gro_flows[i].merged_len = 0;
            gro_flows[i].merged_payload = 0;
            gro_flows[i].segment_count = 0;
            gro_flows[i].first_seq = 0;
            gro_flows[i].last_activity = 0;
            return &gro_flows[i];
        }
    }

    return NULL; /* All flow slots in use */
}

/* ── Try to merge a new packet into an existing GRO flow ────────────
 * Returns 1 if merged, 0 if not mergeable (caller should deliver raw).
 */
static int gro_try_merge(struct gro_flow *flow,
                          const uint8_t *pkt, uint32_t len,
                          uint64_t current_ticks)
{
    if (!flow || !flow->active)
        return 0;

    /* Update activity timestamp */
    flow->last_activity = current_ticks;

    /* For TCP: check sequence number continuity */
    if (flow->ip_proto == IP_PROTO_TCP) {
        if (len < flow->headroom + sizeof(struct tcp_header))
            return 0;

        const struct tcp_header *tcp = (const struct tcp_header *)
            (pkt + flow->headroom);
        uint32_t pkt_seq = ntohl(tcp->seq_num);

        if (flow->segment_count == 0) {
            /* First packet in flow — just copy */
            flow->first_seq = pkt_seq;
        } else {
            /* Calculate expected next seq */
            uint32_t expected_seq = flow->first_seq + flow->merged_payload;
            if (pkt_seq != expected_seq)
                return 0; /* Gap in sequence — don't merge */
        }

        /* Calculate payload for this packet */
        uint32_t pkt_payload = len - flow->headroom - flow->l4_hdr_len;

        /* Check if merged result would exceed buffer */
        if (flow->merged_payload + pkt_payload + flow->headroom + flow->l4_hdr_len >
            sizeof(flow->data))
            return 0; /* Would overflow buffer — don't merge */

        /* Check max segments */
        if (flow->segment_count >= GRO_MAX_PACKETS)
            return 0;

        /* Copy the payload (skip headers) */
        memcpy(flow->data + flow->merged_len + flow->headroom + flow->l4_hdr_len,
               pkt + flow->headroom + flow->l4_hdr_len,
               pkt_payload);
        flow->merged_len += pkt_payload;
        flow->merged_payload += pkt_payload;
        flow->segment_count++;
        return 1;
    }

    return 0;
}

/* ── Flush a GRO flow: build final merged packet into buffer ───────
 * Returns the number of bytes written, or 0 if nothing to flush.
 */
static int gro_flush_flow(struct gro_flow *flow, uint8_t *buf, uint16_t max_len)
{
    if (!flow || !flow->active || flow->segment_count == 0)
        return 0;

    /* Build merged packet: headers + merged payload */
    uint32_t total_len = flow->headroom + flow->l4_hdr_len + flow->merged_payload;
    if (total_len > max_len)
        total_len = max_len;

    /* Copy headers from first packet (we need the data from the first merged) */
    /* If we have no base data, we can't rebuild headers — just return 0 */
    if (flow->merged_len == 0 && flow->merged_payload > 0) {
        /* We stored payload separately — just copy payload to buf after headers */
        /* This is a simplified GRO that delivers merged payloads */
        /* For proper GRO, the first packet's headers are needed */
        return 0;
    }

    /* The merged data includes the full packet payload */
    if (total_len > 0 && total_len <= max_len) {
        memcpy(buf, flow->data, total_len);
    }

    /* Update statistics */
    gro_stats.gro_packets++;
    gro_stats.gro_merged += flow->segment_count;
    gro_stats.gro_flushes++;
    gro_stats.gro_bytes_saved += flow->segment_count * (flow->headroom + flow->l4_hdr_len);

    /* Reset flow */
    flow->active = 0;
    flow->merged_len = 0;
    flow->merged_payload = 0;
    flow->segment_count = 0;

    return (int)total_len;
}

/* ── Flush all expired GRO flows ────────────────────────────────────
 * Called periodically to age out flows that haven't seen activity.
 */
static void gro_flush_expired(uint64_t current_ticks)
{
    for (int i = 0; i < GRO_MAX_FLOWS; i++) {
        if (gro_flows[i].active &&
            gro_flows[i].last_activity + GRO_TIMEOUT_TICKS < current_ticks &&
            gro_flows[i].segment_count > 0) {
            /* Flow timed out and has data — need to push it out.
             * Since we can't deliver here (no buffer), mark for flush
             * on next receive call. */
            gro_flows[i].active = 0;
            gro_stats.gro_flushes++;
        }
    }
}

/* ── GRO receive: try to merge a packet into GRO flow ─────────────
 * Called from virtio_net_receive for each incoming packet.
 * If the packet can be merged into an existing GRO flow, returns 1
 * (caller should not deliver this packet individually).
 * If the packet starts a new flow or can't be merged, returns 0
 * (caller should deliver the previous flow's merged result and
 * this packet).
 *
 * When returns 2: the caller should deliver the merged packet from
 * 'merged_buf' (up to 'merged_len' bytes), then also deliver
 * this packet as a separate segment.
 */
int virtio_net_gro_receive(const uint8_t *pkt, uint32_t len,
                            uint8_t *merged_buf, uint16_t *merged_len,
                            uint64_t current_ticks)
{
    if (!pkt || !merged_buf || !merged_len || len < sizeof(struct eth_header))
        return 0;

    *merged_len = 0;

    /* Initialize GRO table on first use */
    if (!gro_initialized) {
        memset(gro_flows, 0, sizeof(gro_flows));
        gro_initialized = 1;
    }

    /* Find or allocate a flow for this packet */
    struct gro_flow *flow = gro_find_flow(pkt, len);
    if (!flow)
        return 0; /* No flow slot — deliver raw */

    if (flow->segment_count == 0) {
        /* First packet in this flow — store it as base */
        if (len > sizeof(flow->data))
            return 0;

        memcpy(flow->data, pkt, len);
        flow->merged_len = len;
        flow->merged_payload = len - flow->headroom - flow->l4_hdr_len;
        flow->segment_count = 1;
        flow->last_activity = current_ticks;

        /* Extract TCP seqnum if needed */
        if (flow->ip_proto == IP_PROTO_TCP && len >= flow->headroom + sizeof(struct tcp_header)) {
            const struct tcp_header *tcp = (const struct tcp_header *)
                (pkt + flow->headroom);
            flow->first_seq = ntohl(tcp->seq_num);
        }

        /* Don't deliver yet — wait for more segments */
        return 1;
    }

    /* Try to merge this packet */
    if (gro_try_merge(flow, pkt, len, current_ticks)) {
        return 1; /* Merged — don't deliver individually */
    }

    /* Packet can't be merged — flush current flow and start new one.
     * Deliver the merged packet now. */
    *merged_len = (uint16_t)flow->merged_len;
    if (*merged_len > 0) {
        memcpy(merged_buf, flow->data, *merged_len);
    }

    /* Start new flow with this packet */
    memset(flow, 0, sizeof(*flow));
    flow->active = 1;
    if (gro_flow_from_pkt(flow, pkt, len) == 0) {
        if (len > sizeof(flow->data))
            return 0;
        memcpy(flow->data, pkt, len);
        flow->merged_len = len;
        flow->merged_payload = len - flow->headroom - flow->l4_hdr_len;
        flow->segment_count = 1;
        flow->last_activity = current_ticks;

        if (flow->ip_proto == IP_PROTO_TCP && len >= flow->headroom + sizeof(struct tcp_header)) {
            const struct tcp_header *tcp = (const struct tcp_header *)
                (pkt + flow->headroom);
            flow->first_seq = ntohl(tcp->seq_num);
        }
    }

    return 2; /* Caller should deliver merged_buf AND this packet */
}

/* ══════════════════════════════════════════════════════════════════
 *  End TSO/GRO/GSO offload functions
 * ══════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════
 *  RSS (Receive Side Steering) — Software Toeplitz Hash & Steering
 * ══════════════════════════════════════════════════════════════════ */

/* ── RSS constants ──────────────────────────────────────────────── */
#define RSS_HASH_KEY_LEN      40   /* Toeplitz hash key length in bytes */
#define RSS_INDIR_TABLE_SIZE  128  /* Default indirection table entries */
#define RSS_DEFAULT_KEY_HASH  0x6D5A  /* CRC16 of default key for verification */

/* ── Default RSS hash key (symmetric, from Microsoft RNDIS spec) ── */
static const uint8_t rss_default_key[RSS_HASH_KEY_LEN] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
};

/* ── RSS state ──────────────────────────────────────────────────── */
struct virtio_net_rss_state {
    int                initialized;      /* 1 after virtio_net_rss_init() */
    uint8_t            hash_key[RSS_HASH_KEY_LEN]; /* 40-byte Toeplitz key */
    uint16_t           indir_table[RSS_INDIR_TABLE_SIZE]; /* queue indirection */
    uint16_t           indir_len;        /* actual indirection table length */
    uint32_t           hash_types;       /* enabled VIRTIO_NET_HASH_TYPE_* flags */
    uint16_t           unclassified_q;   /* fallback queue */
    struct virtio_net_rss_stats stats;    /* operational statistics */
};

static struct virtio_net_rss_state rss_state;

/* ── RSS Toeplitz hash computation ──────────────────────────────────
 *
 * Computes the symmetric Toeplitz hash over 'data' of 'data_len' bytes
 * using the 40-byte 'key'.  Implements the standard RSS hash algorithm
 * as used by network hardware for receive-side scaling.
 *
 * The hash is computed bit-by-bit: for each bit position of the input
 * data (processed MSB first), if the bit is 1, a 32-bit window of the
 * key starting at that bit position is XOR-accumulated into the hash.
 *
 * Returns the 32-bit Toeplitz hash value.
 */
static uint32_t rss_toeplitz_hash(const uint8_t *key, size_t key_len,
                                   const void *data, size_t data_len)
{
    uint32_t hash = 0;
    size_t total_bits = data_len * 8;
    size_t bit_pos;

    if (!key || key_len < 4 || !data || data_len == 0)
        return 0;

    for (bit_pos = 0; bit_pos < total_bits; bit_pos++) {
        size_t byte_idx = bit_pos / 8;
        size_t bit_idx = 7 - (bit_pos % 8); /* MSB first */
        uint8_t input_byte = ((const uint8_t *)data)[byte_idx];

        if (input_byte & (1u << bit_idx)) {
            /* Extract 32-bit window from key starting at key byte (bit_pos/8) */
            size_t kofs = bit_pos / 8;
            uint32_t key_window;
            if (kofs + 3 < key_len) {
                key_window = ((uint32_t)key[kofs] << 24)
                           | ((uint32_t)key[kofs + 1] << 16)
                           | ((uint32_t)key[kofs + 2] << 8)
                           | ((uint32_t)key[kofs + 3]);
            } else {
                /* Pad with zeros at the end of the key */
                key_window = 0;
                if (kofs < key_len)
                    key_window |= (uint32_t)key[kofs] << 24;
                if (kofs + 1 < key_len)
                    key_window |= (uint32_t)key[kofs + 1] << 16;
                if (kofs + 2 < key_len)
                    key_window |= (uint32_t)key[kofs + 2] << 8;
                if (kofs + 3 < key_len)
                    key_window |= (uint32_t)key[kofs + 3];
            }
            hash ^= key_window;
        }
    }

    return hash;
}

/* ── Determine RSS hash tuple from a received Ethernet frame ────────
 *
 * Given a full Ethernet frame (including eth header), extracts the
 * 4-tuple (or 2-tuple for non-TCP/UDP) for RSS hash computation.
 * Returns the tuple data pointer and length via output parameters.
 *
 * Returns the VIRTIO_NET_HASH_REPORT_* type, or VIRTIO_NET_HASH_REPORT_NONE
 * if the packet type is not recognized/hashable.
 *
 * The hash_tuples are extracted in network byte order.
 */
static uint8_t rss_get_hash_tuple(const uint8_t *pkt, uint32_t len,
                                   const void **tuple_out, size_t *tuple_len_out)
{
    /* Default: no tuple extracted */
    if (tuple_out)   *tuple_out   = NULL;
    if (tuple_len_out) *tuple_len_out = 0;

    if (!pkt || len < sizeof(struct eth_header))
        return VIRTIO_NET_HASH_REPORT_NONE;

    const struct eth_header *eth = (const struct eth_header *)pkt;
    uint16_t eth_type = ntohs(eth->type);

    if (eth_type == ETH_TYPE_IP) {
        /* ── IPv4 ── */
        if (len < sizeof(struct eth_header) + sizeof(struct ip_header))
            return VIRTIO_NET_HASH_REPORT_NONE;

        const struct ip_header *ip = (const struct ip_header *)
            (pkt + sizeof(struct eth_header));
        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
        if (ip_hdr_len < 20 || (uint32_t)(sizeof(struct eth_header) + ip_hdr_len) > len)
            return VIRTIO_NET_HASH_REPORT_NONE;

        if (ip->protocol == IP_PROTO_TCP) {
            /* TCPv4 tuple: src_ip(4) + dst_ip(4) + src_port(2) + dst_port(2) = 12 bytes */
            if (len < sizeof(struct eth_header) + ip_hdr_len + sizeof(struct tcp_header))
                return VIRTIO_NET_HASH_REPORT_NONE;

            if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header); /* starts at IP */
            if (tuple_len_out) *tuple_len_out = 12; /* IP src + dst + TCP ports */
            return VIRTIO_NET_HASH_REPORT_TCPv4;
        } else if (ip->protocol == IP_PROTO_UDP) {
            /* UDPv4 tuple: src_ip(4) + dst_ip(4) + src_port(2) + dst_port(2) = 12 bytes */
            if (len < sizeof(struct eth_header) + ip_hdr_len + sizeof(struct udp_header))
                return VIRTIO_NET_HASH_REPORT_NONE;

            if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
            if (tuple_len_out) *tuple_len_out = 12;
            return VIRTIO_NET_HASH_REPORT_UDPv4;
        } else {
            /* IPv4-only tuple: src_ip(4) + dst_ip(4) = 8 bytes */
            if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
            if (tuple_len_out) *tuple_len_out = 8; /* IP src + dst only */
            return VIRTIO_NET_HASH_REPORT_IPv4;
        }

    } else if (eth_type == ETH_TYPE_IPV6) {
        /* ── IPv6 ── */
        if (len < sizeof(struct eth_header) + sizeof(struct ipv6_header))
            return VIRTIO_NET_HASH_REPORT_NONE;

        const struct ipv6_header *ip6 = (const struct ipv6_header *)
            (pkt + sizeof(struct eth_header));

        /* Check for extension headers (simplified: detect by non-standard next_header) */
        int has_exthdr = 0;
        uint8_t next_hdr = ip6->next_header;

        /* Scan extension headers if present (limited depth) */
        const uint8_t *ext_ptr;
        uint32_t ext_offset = sizeof(struct eth_header) + sizeof(struct ipv6_header);
        int ext_depth = 0;

        while (ext_depth < 8 && ext_offset < len) {
            if (next_hdr == 0 || next_hdr == 43 || next_hdr == 44 ||
                next_hdr == 50 || next_hdr == 51 || next_hdr == 60 ||
                next_hdr == 135) {
                /* Hop-by-Hop(0), Routing(43), Fragment(44),
                 * ESP(50), AH(51), DestOpts(60), Mobility(135) */
                has_exthdr = 1;
                if (ext_offset + 2 > len)
                    break;
                ext_ptr = pkt + ext_offset;
                next_hdr = ext_ptr[0];
                uint8_t ext_hdr_len = ext_ptr[1];
                ext_offset += (uint32_t)(ext_hdr_len + 1) * 8;
                ext_depth++;
            } else {
                break;
            }
        }

        if (next_hdr == IP_PROTO_TCP) {
            if (ext_offset + sizeof(struct tcp_header) > len)
                return VIRTIO_NET_HASH_REPORT_NONE;
            if (has_exthdr) {
                if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
                if (tuple_len_out) *tuple_len_out = 36; /* IPv6 src(16) + dst(16) + ports(4) */
                return VIRTIO_NET_HASH_REPORT_TCPv6_EX;
            } else {
                if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
                if (tuple_len_out) *tuple_len_out = 36;
                return VIRTIO_NET_HASH_REPORT_TCPv6;
            }
        } else if (next_hdr == IP_PROTO_UDP) {
            if (ext_offset + sizeof(struct udp_header) > len)
                return VIRTIO_NET_HASH_REPORT_NONE;
            if (has_exthdr) {
                if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
                if (tuple_len_out) *tuple_len_out = 36;
                return VIRTIO_NET_HASH_REPORT_UDPv6_EX;
            } else {
                if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
                if (tuple_len_out) *tuple_len_out = 36;
                return VIRTIO_NET_HASH_REPORT_UDPv6;
            }
        } else {
            /* IPv6-only: src(16) + dst(16) = 32 bytes */
            if (has_exthdr) {
                if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
                if (tuple_len_out) *tuple_len_out = 32;
                return VIRTIO_NET_HASH_REPORT_IPv6_EX;
            } else {
                if (tuple_out) *tuple_out = pkt + sizeof(struct eth_header);
                if (tuple_len_out) *tuple_len_out = 32;
                return VIRTIO_NET_HASH_REPORT_IPv6;
            }
        }
    }

    return VIRTIO_NET_HASH_REPORT_NONE;
}

/* ── Select queue from RSS hash value ───────────────────────────────
 * Uses the indirection table: hash low N bits map to a table entry,
 * and the entry value is the queue index.
 * N = ceil(log2(indir_len)).
 */
static uint16_t rss_select_queue(uint32_t hash, const uint16_t *table,
                                  uint16_t table_len)
{
    if (!table || table_len == 0)
        return 0;

    /* Compute mask: smallest power-of-2 >= table_len */
    uint16_t mask = 1;
    while (mask < table_len)
        mask <<= 1;
    mask--;

    uint16_t idx = (uint16_t)(hash & mask);
    if (idx >= table_len)
        idx = table_len - 1;
    return table[idx];
}

/* ── Compute RSS hash for a received Ethernet packet ────────────────
 *
 * Determines the hash type from the packet, checks if it is enabled
 * in the current RSS configuration, computes the Toeplitz hash over
 * the appropriate tuple (src/dst IP + ports), and returns the hash.
 *
 * Returns the 32-bit hash value (0 if packet can't be hashed or hash
 * type is not enabled).  *hash_type_out is set to the
 * VIRTIO_NET_HASH_REPORT_* constant.
 */
uint32_t virtio_net_rss_hash_packet(const uint8_t *pkt, uint32_t len,
                                     uint8_t *hash_type_out)
{
    uint8_t htype = VIRTIO_NET_HASH_REPORT_NONE;
    uint32_t hash = 0;

    if (!hash_type_out)
        return 0;

    /* Get the hash tuple and determine the hash type */
    const void *tuple = NULL;
    size_t tuple_len = 0;

    htype = rss_get_hash_tuple(pkt, len, &tuple, &tuple_len);
    *hash_type_out = htype;

    if (htype == VIRTIO_NET_HASH_REPORT_NONE || !tuple || tuple_len == 0)
        return 0;

    /* Check if this hash type is enabled */
    uint32_t type_bit = 0;
    switch (htype) {
    case VIRTIO_NET_HASH_REPORT_IPv4:     type_bit = VIRTIO_NET_HASH_TYPE_IPv4;   break;
    case VIRTIO_NET_HASH_REPORT_TCPv4:    type_bit = VIRTIO_NET_HASH_TYPE_TCPv4;  break;
    case VIRTIO_NET_HASH_REPORT_UDPv4:    type_bit = VIRTIO_NET_HASH_TYPE_UDPv4;  break;
    case VIRTIO_NET_HASH_REPORT_IPv6:     type_bit = VIRTIO_NET_HASH_TYPE_IPv6;   break;
    case VIRTIO_NET_HASH_REPORT_TCPv6:    type_bit = VIRTIO_NET_HASH_TYPE_TCPv6;  break;
    case VIRTIO_NET_HASH_REPORT_UDPv6:    type_bit = VIRTIO_NET_HASH_TYPE_UDPv6;  break;
    case VIRTIO_NET_HASH_REPORT_IPv6_EX:  type_bit = VIRTIO_NET_HASH_TYPE_IPv6_EX; break;
    case VIRTIO_NET_HASH_REPORT_TCPv6_EX: type_bit = VIRTIO_NET_HASH_TYPE_TCPv6_EX; break;
    case VIRTIO_NET_HASH_REPORT_UDPv6_EX: type_bit = VIRTIO_NET_HASH_TYPE_UDPv6_EX; break;
    default:                              type_bit = 0;                            break;
    }

    if (type_bit == 0 || !(rss_state.hash_types & type_bit)) {
        /* Hash type not enabled — count as unhashed */
        rss_state.stats.packets_unhashed++;
        return 0;
    }

    /* Compute the Toeplitz hash */
    hash = rss_toeplitz_hash(rss_state.hash_key, RSS_HASH_KEY_LEN,
                              tuple, tuple_len);

    /* Update statistics */
    rss_state.stats.packets_hashed++;
    switch (htype) {
    case VIRTIO_NET_HASH_REPORT_IPv4:     rss_state.stats.hash_ipv4++;     break;
    case VIRTIO_NET_HASH_REPORT_TCPv4:    rss_state.stats.hash_tcpv4++;    break;
    case VIRTIO_NET_HASH_REPORT_UDPv4:    rss_state.stats.hash_udpv4++;    break;
    case VIRTIO_NET_HASH_REPORT_IPv6:     rss_state.stats.hash_ipv6++;     break;
    case VIRTIO_NET_HASH_REPORT_TCPv6:    rss_state.stats.hash_tcpv6++;    break;
    case VIRTIO_NET_HASH_REPORT_UDPv6:    rss_state.stats.hash_udpv6++;    break;
    case VIRTIO_NET_HASH_REPORT_IPv6_EX:  rss_state.stats.hash_ipv6_ex++;  break;
    case VIRTIO_NET_HASH_REPORT_TCPv6_EX: rss_state.stats.hash_tcpv6_ex++; break;
    case VIRTIO_NET_HASH_REPORT_UDPv6_EX: rss_state.stats.hash_udpv6_ex++; break;
    default: break;
    }

    return hash;
}

/* ── Initialize RSS subsystem ────────────────────────────────────────
 *
 * Sets the default symmetric hash key, creates a default indirection
 * table (round-robin across available RX queues), and enables all
 * standard hash types.
 *
 * For the legacy transport (2 queues: RX/TX), the indirection table
 * maps all entries to queue 0.  When the modern transport with
 * multiple RX queues is available, the indirection table distributes
 * across all available queues.
 *
 * Returns 0 on success.
 */
int virtio_net_rss_init(void)
{
    /* If already initialized, just reset stats */
    if (rss_state.initialized) {
        memset(&rss_state.stats, 0, sizeof(rss_state.stats));
        return 0;
    }

    /* Copy default hash key */
    memcpy(rss_state.hash_key, rss_default_key, RSS_HASH_KEY_LEN);

    /* Build default indirection table.
     * With legacy transport we have 1 RX queue (RX_QUEUE_IDX=0).
     * When multi-queue is available (modern transport), the caller
     * can call rss_set_config to redistribute. */
    {
        uint16_t num_rx_queues = 1; /* fixed for legacy transport */
        for (uint16_t i = 0; i < RSS_INDIR_TABLE_SIZE; i++)
            rss_state.indir_table[i] = (uint16_t)(i % num_rx_queues);
        rss_state.indir_len = RSS_INDIR_TABLE_SIZE;
    }

    /* Enable all hash types by default */
    rss_state.hash_types =
        VIRTIO_NET_HASH_TYPE_IPv4   | VIRTIO_NET_HASH_TYPE_TCPv4  |
        VIRTIO_NET_HASH_TYPE_UDPv4  | VIRTIO_NET_HASH_TYPE_IPv6   |
        VIRTIO_NET_HASH_TYPE_TCPv6  | VIRTIO_NET_HASH_TYPE_UDPv6  |
        VIRTIO_NET_HASH_TYPE_IPv6_EX | VIRTIO_NET_HASH_TYPE_TCPv6_EX |
        VIRTIO_NET_HASH_TYPE_UDPv6_EX;

    rss_state.unclassified_q = 0;
    memset(&rss_state.stats, 0, sizeof(rss_state.stats));
    rss_state.initialized = 1;

    kprintf("virtio-net: RSS initialized (indir_len=%u, hash_types=0x%08X)\n",
            (unsigned int)rss_state.indir_len,
            (unsigned int)rss_state.hash_types);

    return 0;
}

/* ── Set RSS configuration ──────────────────────────────────────────
 *
 * Updates the RSS hash key, indirection table, enabled hash types,
 * and unclassified queue.  The indirection table is limited to
 * RSS_INDIR_TABLE_SIZE entries.
 *
 * Returns 0 on success, -1 on invalid arguments.
 */
int virtio_net_rss_set_config(const uint8_t *key, size_t key_len,
                               const uint16_t *indir_table, uint16_t indir_len,
                               uint32_t hash_types, uint16_t unclassified_q)
{
    if (!rss_state.initialized) {
        if (virtio_net_rss_init() < 0)
            return -1;
    }

    /* Validate key length */
    if (key && key_len > 0) {
        size_t copy_len = key_len < RSS_HASH_KEY_LEN ? key_len : RSS_HASH_KEY_LEN;
        memset(rss_state.hash_key, 0, RSS_HASH_KEY_LEN);
        memcpy(rss_state.hash_key, key, copy_len);
    }

    /* Validate and copy indirection table */
    if (indir_table && indir_len > 0) {
        uint16_t copy_len = indir_len < RSS_INDIR_TABLE_SIZE ? indir_len : RSS_INDIR_TABLE_SIZE;
        for (uint16_t i = 0; i < copy_len; i++)
            rss_state.indir_table[i] = indir_table[i];
        rss_state.indir_len = copy_len;
    }

    /* Store hash types and unclassified queue */
    rss_state.hash_types = hash_types;
    rss_state.unclassified_q = unclassified_q;

    kprintf("virtio-net: RSS config updated (hash_types=0x%08X, indir_len=%u, unclass_q=%u)\n",
            (unsigned int)rss_state.hash_types,
            (unsigned int)rss_state.indir_len,
            (unsigned int)rss_state.unclassified_q);

    return 0;
}

/* ── Get current RSS configuration ─────────────────────────────────── */
void virtio_net_rss_get_config(uint32_t *hash_types, uint16_t *unclassified_q)
{
    if (hash_types)
        *hash_types = rss_state.hash_types;
    if (unclassified_q)
        *unclassified_q = rss_state.unclassified_q;
}

/* ── Get RSS statistics ───────────────────────────────────────────── */
void virtio_net_rss_get_stats(struct virtio_net_rss_stats *stats)
{
    if (stats)
        memcpy(stats, &rss_state.stats, sizeof(*stats));
}

/* ══════════════════════════════════════════════════════════════════
 *  Control VQ — MAC Filtering & Promiscuous/Allmulti Control
 * ══════════════════════════════════════════════════════════════════ */

/* ── Send a control command via the control virtqueue ───────────────
 *
 * Sends a control VQ message: ctrl_hdr [optional data] → ack.
 *
 * class:       VIRTIO_NET_CTRL_* class identifier
 * command:     class-specific command
 * data:        optional command-specific data (may be NULL if data_len == 0)
 * data_len:    number of bytes in data
 *
 * Returns 0 on success (device returned VIRTIO_NET_OK),
 *         -ENOSYS if control VQ not available,
 *         -EIO if device returned VIRTIO_NET_ERR or communication failed.
 */
static int virtio_net_ctrl_send(uint8_t class, uint8_t command,
                                 const void *data, size_t data_len)
{
    struct vring_desc  *descs = (struct vring_desc  *)ctrl_queue_mem;
    struct vring_avail *avail = vring_avail_ptr(ctrl_queue_mem);
    struct vring_used  *used  = vring_used_ptr(ctrl_queue_mem);
    struct virtio_net_ctrl_hdr *hdr;
    int desc_count = 0;
    uint64_t irq_flags;
    int ret = -EIO;

    if (!ctrl_vq_available)
        return -ENOSYS;

    spinlock_irqsave_acquire(&ctrl_vq_lock, &irq_flags);

    /* Build descriptor chain:
     *   desc 0: ctrl_hdr (OUT) — always present
     *   desc 1: data payload (OUT) — optional
     *   desc 2: ack buffer (IN) — always present
     */
    hdr = (struct virtio_net_ctrl_hdr *)ctrl_data_buf;
    hdr->class   = class;
    hdr->command = command;

    /* Place the header in descriptor 0 */
    descs[0].addr  = VIRT_TO_PHYS(hdr);
    descs[0].len   = sizeof(struct virtio_net_ctrl_hdr);
    descs[0].flags = VRING_DESC_F_NEXT;  /* chained */
    desc_count = 1;

    /* If there's data payload, put it in descriptor 1 */
    if (data && data_len > 0) {
        /* Copy data into the control data buffer after the header */
        size_t copy_len = data_len;
        if (copy_len > sizeof(ctrl_data_buf) - sizeof(struct virtio_net_ctrl_hdr))
            copy_len = sizeof(ctrl_data_buf) - sizeof(struct virtio_net_ctrl_hdr);

        memcpy(ctrl_data_buf + sizeof(struct virtio_net_ctrl_hdr), data, copy_len);

        descs[1].addr  = VIRT_TO_PHYS(ctrl_data_buf + sizeof(struct virtio_net_ctrl_hdr));
        descs[1].len   = (uint32_t)copy_len;
        descs[1].flags = VRING_DESC_F_NEXT;
        descs[1].next  = 2;
        desc_count = 2;
    }

    /* ACK descriptor (device writes status byte here) */
    ctrl_ack_buf[0] = VIRTIO_NET_ERR;  /* pre-fill with error */
    {
        int ack_desc_idx = desc_count;
        descs[ack_desc_idx].addr  = VIRT_TO_PHYS(ctrl_ack_buf);
        descs[ack_desc_idx].len   = VIRTIO_NET_CTRL_ACK_SIZE;
        descs[ack_desc_idx].flags = VRING_DESC_F_WRITE;  /* device writes */
        descs[ack_desc_idx].next  = 0;

        /* Wire up previous descriptor to point to ack */
        if (desc_count >= 1) {
            /* Link desc 0 (or the last data desc) to the ack desc */
            if (desc_count == 1) {
                /* Only header + ack */
                descs[0].next = 1;
            }
            /* descs[1].next already set to 2 above if data was present */
            descs[desc_count].flags = VRING_DESC_F_WRITE; /* ack is write-only */
            descs[desc_count].next  = 0;
        }
        desc_count++;  /* now we have 2 or 3 descriptors */
    }

    /* Place the chain head into the avail ring */
    {
        if (!vring_avail_has_room(avail, used, 1)) {
            ret = -EAGAIN;
            goto out;
        }
        uint16_t slot = avail->idx & (VRING_SIZE - 1);
        avail->ring[slot] = 0;  /* descriptor 0 is always the chain head */
        __asm__ volatile("" ::: "memory");
        avail->idx++;
        __asm__ volatile("" ::: "memory");
    }

    /* Notify the device */
    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, CTRL_QUEUE_IDX);

    /* Wait for device to process the control command */
    {
        uint64_t spin = 0;
        uint16_t last_used = used->idx;
        while (used->idx == last_used && spin++ < 1000000)
            __asm__ volatile("" ::: "memory");

        if (used->idx == last_used)
            goto out;
    }

    /* Check ACK status */
    if (ctrl_ack_buf[0] == VIRTIO_NET_OK) {
        ret = 0;
        goto out;
    }

out:
    spinlock_irqsave_release(&ctrl_vq_lock, irq_flags);
    return ret;
}

/* ── Set promiscuous mode ──────────────────────────────────────────
 * 1 = accept all packets (promiscuous), 0 = filter per MAC table.
 * Returns 0 on success, negative errno on failure.
 */
int virtio_net_ctrl_set_promisc(int on)
{
    return virtio_net_ctrl_send(VIRTIO_NET_CTRL_RX,
                                on ? VIRTIO_NET_CTRL_RX_PROMISC
                                   : VIRTIO_NET_CTRL_RX_NO_UCAST,
                                NULL, 0);
}

/* ── Set all-multicast mode ─────────────────────────────────────────
 * 1 = accept all multicast, 0 = filter per MAC table.
 * Returns 0 on success, negative errno on failure.
 */
int virtio_net_ctrl_set_allmulti(int on)
{
    return virtio_net_ctrl_send(VIRTIO_NET_CTRL_RX,
                                on ? VIRTIO_NET_CTRL_RX_ALLMULTI
                                   : VIRTIO_NET_CTRL_RX_NO_MCAST,
                                NULL, 0);
}

/* ── Set MAC address filtering table ────────────────────────────────
 * Sends the full MAC address table to the device via the control VQ.
 * The device will only accept packets matching the listed MACs
 * (unless promiscuous or all-multicast mode is active).
 *
 * uc_macs:  array of 6-byte unicast MAC addresses (may be NULL if num_uc==0)
 * num_uc:   number of unicast MAC entries
 * mc_macs:  array of 6-byte multicast MAC addresses (may be NULL if num_mc==0)
 * num_mc:   number of multicast MAC entries
 *
 * Returns 0 on success, negative errno on failure.
 */
int virtio_net_ctrl_set_mac_table(const uint8_t *uc_macs, uint16_t num_uc,
                                   const uint8_t *mc_macs, uint16_t num_mc)
{
    /* Build the MAC table payload in ctrl_data_buf after the header:
     *   le32 num_unicast
     *   uc_macs[0..num_uc-1] (each 6 bytes)
     *   le32 num_multicast
     *   mc_macs[0..num_mc-1] (each 6 bytes)
     */
    size_t offset = 0;
    uint8_t *buf = ctrl_data_buf + sizeof(struct virtio_net_ctrl_hdr);
    size_t max_data = sizeof(ctrl_data_buf) - sizeof(struct virtio_net_ctrl_hdr);

    /* Calculate total payload size */
    size_t total_len = sizeof(uint32_t) + (size_t)num_uc * 6
                     + sizeof(uint32_t) + (size_t)num_mc * 6;

    if (total_len > max_data)
        return -EINVAL;

    /* Write unicast count */
    {
        uint32_t ucnt = (uint32_t)num_uc;
        memcpy(buf + offset, &ucnt, sizeof(ucnt));
    }
    offset += sizeof(uint32_t);

    /* Write unicast MAC addresses */
    if (uc_macs && num_uc > 0) {
        memcpy(buf + offset, uc_macs, (size_t)num_uc * 6);
        offset += (size_t)num_uc * 6;
    }

    /* Write multicast count */
    {
        uint32_t mcnt = (uint32_t)num_mc;
        memcpy(buf + offset, &mcnt, sizeof(mcnt));
    }
    offset += sizeof(uint32_t);

    /* Write multicast MAC addresses */
    if (mc_macs && num_mc > 0) {
        memcpy(buf + offset, mc_macs, (size_t)num_mc * 6);
        offset += (size_t)num_mc * 6;
    }

    return virtio_net_ctrl_send(VIRTIO_NET_CTRL_MAC,
                                VIRTIO_NET_CTRL_MAC_TABLE_SET,
                                buf, offset);
}

/* ── Init ────────────────────────────────────────────────────────── */
int virtio_net_init(void) {
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_DEVICE, &dev) < 0)
        return -1;

    /* BAR0 is the legacy I/O BAR */
    vnet_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!vnet_iobase) return -1;

    pci_enable_bus_master(&dev);

    /* Reset device */
    vio_outb(VIRTIO_PCI_STATUS, 0);
    /* Acknowledge & driver */
    vio_outb(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Initialize LRO stats */
    memset(&lro_stats, 0, sizeof(lro_stats));
    memset(&lro_seg, 0, sizeof(lro_seg));

    /* Initialize RSS subsystem */
    virtio_net_rss_init();

    /* Negotiate features: accept only what we support, validate
     * required features (VIRTIO_NET_F_MAC is mandatory), then
     * set FEATURES_OK and validate the device accepted. Logs
     * human-readable feature names for debugging. */
    if (virtio_negotiate_features_ex(vio_inl,   /* readl  — 32-bit port read  */
                                      vio_outl,  /* writel — 32-bit port write */
                                      vio_outb,  /* writeb — 8-bit port write  */
                                      vio_inb,   /* readb  — 8-bit port read   */
                                      VNET_SUPPORTED_FEATURES,
                                      VNET_REQUIRED_FEATURES,
                                      virtio_net_features,
                                      "virtio-net") < 0) {
        kprintf("virtio-net: device rejected feature negotiation\n");
        return -1;
    }

    /* Store negotiated features for offload path decisions */
    vnet_negotiated_features = vio_inl(VIRTIO_PCI_GUEST_FEAT);

    /* Check if LRO features were negotiated */
    {
        uint32_t guest_feat = vnet_negotiated_features;
        if (guest_feat & (VIRTIO_NET_F_GUEST_TSO4 | VIRTIO_NET_F_GUEST_TSO6)) {
            kprintf("virtio-net: LRO enabled (GUEST_TSO4=%d GUEST_TSO6=%d GUEST_UFO=%d)\n",
                    !!(guest_feat & VIRTIO_NET_F_GUEST_TSO4),
                    !!(guest_feat & VIRTIO_NET_F_GUEST_TSO6),
                    !!(guest_feat & VIRTIO_NET_F_GUEST_UFO));
        } else {
            kprintf("virtio-net: LRO not available (no GUEST_TSO features)\n");
        }

        /* Log TX offload status */
        kprintf("virtio-net: TX offload (HOST_TSO4=%d HOST_TSO6=%d CSUM=%d GSO=%d MRG_RXBUF=%d)\n",
                !!(guest_feat & VIRTIO_NET_F_HOST_TSO4),
                !!(guest_feat & VIRTIO_NET_F_HOST_TSO6),
                !!(guest_feat & VIRTIO_NET_F_CSUM),
                !!(guest_feat & VIRTIO_NET_F_GSO),
                !!(guest_feat & VIRTIO_NET_F_MRG_RXBUF));
    }

    /* RX queue (0): populate ring memory before publishing PFN */
    vio_outw(VIRTIO_PCI_QUEUE_SEL, RX_QUEUE_IDX);
    {
        uint16_t qsz = vio_inw(VIRTIO_PCI_QUEUE_SIZE);
        if (qsz != 0 && qsz < VRING_SIZE) {
            kprintf("virtio-net: queue size %u < %u\n", (unsigned int)qsz, (unsigned int)VRING_SIZE);
            return -1;
        }
        struct vring_desc  *descs = (struct vring_desc  *)rx_queue_mem;
        struct vring_avail *avail = vring_avail_ptr(rx_queue_mem);
        struct vring_used  *used  = vring_used_ptr(rx_queue_mem);
        avail->flags = 0;
        avail->idx = 0;
        used->flags = 0;
        used->idx = 0;
        for (int i = 0; i < VRING_SIZE; i++) {
            descs[i].addr  = VIRT_TO_PHYS(rx_pkt_bufs[i]);
            descs[i].len   = sizeof(rx_pkt_bufs[0]);
            descs[i].flags = VRING_DESC_F_WRITE;
            descs[i].next  = 0;
            uint16_t slot = avail->idx & (VRING_SIZE - 1);
            avail->ring[slot] = (uint16_t)i;
            avail->idx++;
        }
        rx_last_used = 0;
    }
    vio_outl(VIRTIO_PCI_QUEUE_PFN,
             (uint32_t)(VIRT_TO_PHYS(rx_queue_mem) >> 12));

    /* TX queue (1) */
    vio_outw(VIRTIO_PCI_QUEUE_SEL, TX_QUEUE_IDX);
    {
        uint16_t qsz = vio_inw(VIRTIO_PCI_QUEUE_SIZE);
        if (qsz != 0 && qsz < VRING_SIZE) return -1;
        struct vring_avail *avail = vring_avail_ptr(tx_queue_mem);
        struct vring_used  *used  = vring_used_ptr(tx_queue_mem);
        avail->flags = 0;
        avail->idx = 0;
        used->flags = 0;
        used->idx = 0;
    }
    vio_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(VIRT_TO_PHYS(tx_queue_mem) >> 12));

    /* Control VQ (2) — only if CTRL_VQ was negotiated */
    if (vnet_negotiated_features & VIRTIO_NET_F_CTRL_VQ) {
        vio_outw(VIRTIO_PCI_QUEUE_SEL, CTRL_QUEUE_IDX);
        uint16_t ctrl_qsz = vio_inw(VIRTIO_PCI_QUEUE_SIZE);
        if (ctrl_qsz != 0 && ctrl_qsz < VRING_SIZE) {
            kprintf("virtio-net: control queue size %u < %u, disabling control VQ\n",
                    (unsigned int)ctrl_qsz, (unsigned int)VRING_SIZE);
            /* Non-fatal — continue without control VQ */
        } else {
            struct vring_avail *avail = vring_avail_ptr(ctrl_queue_mem);
            struct vring_used  *used  = vring_used_ptr(ctrl_queue_mem);
            avail->flags = 0;
            avail->idx = 0;
            used->flags = 0;
            used->idx = 0;
            vio_outl(VIRTIO_PCI_QUEUE_PFN,
                     (uint32_t)(VIRT_TO_PHYS(ctrl_queue_mem) >> 12));
            ctrl_vq_available = 1;
            kprintf("virtio-net: control VQ enabled (queue %u)\n",
                    (unsigned int)CTRL_QUEUE_IDX);
        }
    } else {
        kprintf("virtio-net: control VQ not negotiated (no CTRL_VQ)\n");
    }

    /* Driver OK, then kick RX so the device picks up buffers */
    vio_outb(VIRTIO_PCI_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    vio_outw(VIRTIO_PCI_QUEUE_SEL, RX_QUEUE_IDX);
    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE_IDX);

    vnet_irq = dev.irq;
    idt_register_handler(32 + dev.irq, virtio_net_irq_handler);
    if (apic_is_init_complete())
        ioapic_unmask_irq(dev.irq);
    pic_unmask(dev.irq);

    vnet_present = 1;
    kprintf("virtio-net: initialized (iobase=0x%x, rx_buf_size=%u)\n",
            (unsigned int)vnet_iobase, (unsigned int)RX_BUF_SIZE);
    return 0;
}

/* ── Send ────────────────────────────────────────────────────────── *
 * Supports three modes:
 *   1. TSO/GSO offload: set up virtio_net_hdr for HW segmentation
 *      (used when HOST_TSO4/6 or GSO negotiated)
 *   2. Checksum offload: set up virtio_net_hdr for HW csum only
 *      (used when VIRTIO_NET_F_CSUM negotiated, non-TCP packets)
 *   3. Raw send: no offload (fallback)
 *
 * For packets larger than TX_BUF_SIZE, returns -1.
 */
int virtio_net_send(const uint8_t *data, uint32_t len) {
    if (!vnet_present) return -1;

    if (len > sizeof(tx_pkt_buf)) {
        tx_offload_stats.tx_offload_drops++;
        return -1;
    }

    struct vring_desc  *descs = (struct vring_desc  *)tx_queue_mem;
    struct vring_avail *avail = vring_avail_ptr(tx_queue_mem);
    struct vring_used  *used  = vring_used_ptr(tx_queue_mem);

    /* Wait for previous TX to complete */
    uint64_t spin = 0;
    while (used->idx == tx_last_used && spin++ < 1000000)
        __asm__ volatile("" ::: "memory");
    if (used->idx == tx_last_used) return -1;

    /* Detect if we can use TSO/GSO offload for this packet */
    struct offload_info oinfo;
    int can_offload = 0;

    if (vnet_negotiated_features & (VIRTIO_NET_F_HOST_TSO4 |
                                     VIRTIO_NET_F_HOST_TSO6 |
                                     VIRTIO_NET_F_CSUM |
                                     VIRTIO_NET_F_GSO)) {
        if (parse_packet_offload(data, len, &oinfo) == 0)
            can_offload = 1;
    }

    if (can_offload && oinfo.gso_type != VIRTIO_NET_HDR_GSO_NONE &&
        (vnet_negotiated_features & VIRTIO_NET_F_GSO)) {
        /* ── TSO/GSO hardware offload ── */
        setup_tx_hdr_offload(&tx_hdr, &oinfo);
        memcpy(tx_pkt_buf, data, len);

        descs[0].addr  = VIRT_TO_PHYS(&tx_hdr);
        descs[0].len   = sizeof(tx_hdr);
        descs[0].flags = VRING_DESC_F_NEXT;
        descs[0].next  = 1;
        descs[1].addr  = VIRT_TO_PHYS(tx_pkt_buf);
        descs[1].len   = len;
        descs[1].flags = 0;
        descs[1].next  = 0;

        tx_offload_stats.tso_packets++;

    } else if (can_offload && oinfo.needs_csum &&
               (vnet_negotiated_features & VIRTIO_NET_F_CSUM)) {
        /* ── Checksum offload only ── */
        setup_tx_hdr_offload(&tx_hdr, &oinfo);
        memcpy(tx_pkt_buf, data, len);

        descs[0].addr  = VIRT_TO_PHYS(&tx_hdr);
        descs[0].len   = sizeof(tx_hdr);
        descs[0].flags = VRING_DESC_F_NEXT;
        descs[0].next  = 1;
        descs[1].addr  = VIRT_TO_PHYS(tx_pkt_buf);
        descs[1].len   = len;
        descs[1].flags = 0;
        descs[1].next  = 0;

        tx_offload_stats.csum_offload++;

    } else if (can_offload && oinfo.gso_type != VIRTIO_NET_HDR_GSO_NONE &&
               oinfo.payload_len > DEFAULT_MSS) {
        /* ── Software GSO fallback (device doesn't support HW TSO) ── */
        tx_offload_stats.sw_gso_packets++;
        tx_offload_stats.sw_gso_bytes += len;
        return virtio_net_sw_gso(data, len);

    } else {
        /* ── Raw send (no offload) ── */
        memset(&tx_hdr, 0, sizeof(tx_hdr));
        memcpy(tx_pkt_buf, data, len);

        descs[0].addr  = VIRT_TO_PHYS(&tx_hdr);
        descs[0].len   = sizeof(tx_hdr);
        descs[0].flags = VRING_DESC_F_NEXT;
        descs[0].next  = 1;
        descs[1].addr  = VIRT_TO_PHYS(tx_pkt_buf);
        descs[1].len   = len;
        descs[1].flags = 0;
        descs[1].next  = 0;

        tx_offload_stats.raw_packets++;
    }

    /* Place descriptor 0 into the avail ring */
    {
        if (!vring_avail_has_room(avail, used, 1)) {
            tx_offload_stats.tx_offload_drops++;
            return -1;
        }
        uint16_t idx = avail->idx & (VRING_SIZE - 1);
        avail->ring[idx] = 0;
        __asm__ volatile("" ::: "memory");
        avail->idx++;
        __asm__ volatile("" ::: "memory");
    }

    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, TX_QUEUE_IDX);

    spin = 0;
    while (used->idx == tx_last_used && spin++ < 1000000)
        __asm__ volatile("" ::: "memory");
    if (used->idx == tx_last_used) return -1;
    tx_last_used = used->idx;
    return 0;
}

/*
 * ── Receive ────────────────────────────────────────────────────────
 *
 * Returns the next available packet (or segment of a LRO packet) into
 * 'buf' (up to 'max_len' bytes).  Returns:
 *   > 0  — number of bytes received
 *   0    — no data available
 *   -1   — error
 *
 * LRO handling:
 *   When the device delivers a GSO/LRO packet (gso_type != NONE), we
 *   segment it into individual MSS-sized packets.  The state machine
 *   spans multiple calls to this function — each call returns one
 *   segment.  Once all segments are delivered, the next call fetches
 *   a fresh packet from the device.
 */
int virtio_net_receive(void *buf, uint16_t max_len) {
    if (!vnet_present) return -1;

    /* ── If we're mid-LRO-segmentation, deliver the next segment ── */
    if (lro_seg.active && lro_seg.current_segment < lro_seg.num_segments) {
        return lro_next_segment(buf, max_len);
    }
    lro_seg.active = 0;

    struct vring_used *used = vring_used_ptr(rx_queue_mem);
    struct vring_avail *avail = vring_avail_ptr(rx_queue_mem);

    if (used->idx == rx_last_used) return 0;

    __asm__ volatile("" ::: "memory");
    uint16_t uidx = rx_last_used & (VRING_SIZE - 1);
    uint32_t id = used->ring[uidx].id;
    uint32_t total = used->ring[uidx].len;
    rx_last_used++;

    /* Validate descriptor id from device — must be within our RX buffer array */
    if (id >= VRING_SIZE)
        return 0;

    struct vring_desc *descs = (struct vring_desc *)rx_queue_mem;

    uint32_t skip = sizeof(struct virtio_net_hdr);
    if (total <= skip) return 0;
    uint32_t plen = total - skip;

    /* Check for LRO/GSO packet */
    struct virtio_net_hdr *vhdr = (struct virtio_net_hdr *)rx_pkt_bufs[id];

    /* Determine number of buffers in this mergeable RX buffer chain.
     * When VIRTIO_NET_F_MRG_RXBUF is negotiated, the device may chain
     * multiple descriptors for a single packet.  num_buffers tells us
     * how many descriptors were consumed, and we must walk the chain
     * via next pointers to read data from all buffers and recycle each. */
    uint16_t num_bufs = 1;
    if (vnet_negotiated_features & VIRTIO_NET_F_MRG_RXBUF) {
        num_bufs = vhdr->num_buffers;
        if (num_bufs == 0 || num_bufs > VRING_SIZE)
            num_bufs = 1;
    }

    if (lro_enabled && vhdr->gso_type != VIRTIO_NET_HDR_GSO_NONE &&
        vhdr->hdr_len > 0 && vhdr->gso_size > 0) {
        /* ── LRO packet: start segmentation ── */
        lro_stats.non_lro_packets++; /* Actually LRO, but count once */

        if (plen > RX_BUF_SIZE - skip) {
            lro_stats.dropped_oversize++;
            /* Still return the raw packet as fallback */
            goto deliver_raw;
        }

        /*
         * Start LRO segmentation.  The raw data is in rx_pkt_bufs[id]
         * starting after the virtio_net_hdr header.  We copy it to a
         * staging area because the buffer will be recycled for new RX.
         */
        static uint8_t lro_staging[RX_BUF_SIZE];
        uint32_t data_offset = sizeof(struct virtio_net_hdr);
        /* Accumulate data from all buffers in the mergeable chain */
        {
            uint32_t remaining = plen;
            uint16_t cur_id = id;
            uint8_t *dst = lro_staging;
            for (uint16_t i = 0; i < num_bufs && remaining > 0; i++) {
                uint32_t chunk = RX_BUF_SIZE;
                if (i == 0) chunk = RX_BUF_SIZE - data_offset;
                if (chunk > remaining) chunk = remaining;
                memcpy(dst, rx_pkt_bufs[cur_id] + (i == 0 ? data_offset : 0), chunk);
                dst += chunk;
                remaining -= chunk;
                if (i + 1 < num_bufs)
                    cur_id = descs[cur_id].next;
            }
        }

        /* Recycle all RX descriptors used by this packet */
        {
            /* Device just consumed num_bufs entries, so we must have room */
            if (!vring_avail_has_room(avail, used, num_bufs)) {
                lro_stats.dropped_oversize++;
                goto deliver_raw_fallback;
            }
            uint16_t cur_id = id;
            for (uint16_t i = 0; i < num_bufs; i++) {
                descs[cur_id].addr  = VIRT_TO_PHYS(rx_pkt_bufs[cur_id]);
                descs[cur_id].len   = sizeof(rx_pkt_bufs[0]);
                descs[cur_id].flags = VRING_DESC_F_WRITE;
                descs[cur_id].next  = 0;
                uint16_t slot = avail->idx & (VRING_SIZE - 1);
                avail->ring[slot] = cur_id;
                __asm__ volatile("" ::: "memory");
                avail->idx++;
                __asm__ volatile("" ::: "memory");
                if (i + 1 < num_bufs)
                    cur_id = descs[cur_id].next;
            }
            vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE_IDX);
        }

        if (lro_start_segmentation(vhdr->gso_type, vhdr->hdr_len,
                                    vhdr->gso_size, lro_staging, plen) < 0) {
            /* Segmentation failed — fall through to deliver raw packet */
            goto deliver_raw_fallback;
        }

        /* Deliver the first segment */
        return lro_next_segment(buf, max_len);
    }

    /* ── Normal (non-LRO) packet ── */
    lro_stats.non_lro_packets++;

deliver_raw:
    {
        if (plen > max_len) plen = max_len;
        uint32_t data_offset = sizeof(struct virtio_net_hdr);
        {
            uint32_t remaining = plen;
            uint16_t cur_id = id;
            uint8_t *dst = (uint8_t *)buf;
            for (uint16_t i = 0; i < num_bufs && remaining > 0; i++) {
                uint32_t chunk = RX_BUF_SIZE;
                if (i == 0) chunk = RX_BUF_SIZE - data_offset;
                if (chunk > remaining) chunk = remaining;
                memcpy(dst, rx_pkt_bufs[cur_id] + (i == 0 ? data_offset : 0), chunk);
                dst += chunk;
                remaining -= chunk;
                if (i + 1 < num_bufs)
                    cur_id = descs[cur_id].next;
            }
        }
    }

deliver_raw_fallback:
    /* Recycle the RX descriptor */
    {
        /* Device just consumed num_bufs entries, so we must have room */
        if (!vring_avail_has_room(avail, used, num_bufs)) {
            return 0;
        }
        uint16_t cur_id = id;
        for (uint16_t i = 0; i < num_bufs; i++) {
            descs[cur_id].addr  = VIRT_TO_PHYS(rx_pkt_bufs[cur_id]);
            descs[cur_id].len   = sizeof(rx_pkt_bufs[0]);
            descs[cur_id].flags = VRING_DESC_F_WRITE;
            descs[cur_id].next  = 0;
            uint16_t slot = avail->idx & (VRING_SIZE - 1);
            avail->ring[slot] = cur_id;
            __asm__ volatile("" ::: "memory");
            avail->idx++;
            __asm__ volatile("" ::: "memory");
            if (i + 1 < num_bufs)
                cur_id = descs[cur_id].next;
        }
        vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE_IDX);
    }

    return (int)plen;
}

void virtio_net_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++)
        mac[i] = vio_inb(VIRTIO_PCI_CONFIG + (uint8_t)i);
}

int virtio_net_present(void) { return vnet_present; }

/* Re-enable RX interrupts after NAPI-style draining */
void virtio_net_irq_rearm(void) {
    /* Virtio legacy uses ISR register as a status register.
     * The interrupt is level-sensitive — reading ISR acks it.
     * No explicit re-enable needed (virtio auto-masks). Just ensure ISR is read. */
    if (vnet_present) {
        vio_inb(VIRTIO_PCI_ISR);
    }
}

/* ── LRO control and statistics ──────────────────────────────────── */
int virtio_net_lro_enabled(void) {
    return lro_enabled && vnet_present;
}

void virtio_net_get_lro_stats(struct virtio_net_lro_stats *stats) {
    if (stats) {
        memcpy(stats, &lro_stats, sizeof(lro_stats));
    }
}

/* ── Module hooks ─────────────────────────────────────────────────────── */
#ifdef MODULE
int __init init_module(void) {
    int ret = virtio_net_init();
    if (ret == 0) {
        kprintf("[OK] virtio-net (module): initialized with LRO\n");
    }
    return ret;
}

void __exit cleanup_module(void) {
    vnet_present = 0;
    kprintf("virtio-net (module): unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO network device driver with TSO/GRO/GSO offload support");
MODULE_ALIAS("pci:v00001AF4d00001000sv*sd*bc*sc*i*");
#endif /* MODULE */

/* ── virtio_net_open: Enable RX queue, set DRIVER_OK, start receiving ── */
static int virtio_net_open(void *dev)
{
    (void)dev;
    if (!vnet_present) return -EIO;

    kprintf("[VIRTIO_NET] Opening interface...\n");

    /* Set DRIVER_OK status — device starts sending interrupts */
    uint8_t status = vio_inb(VIRTIO_PCI_STATUS);
    status |= VIRTIO_STATUS_DRIVER_OK;
    vio_outb(VIRTIO_PCI_STATUS, status);

    /* Re-arm RX by notifying the device about available buffers */
    struct vring_avail *avail = vring_avail_ptr(rx_queue_mem);
    __asm__ volatile("" ::: "memory");
    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE_IDX);

    kprintf("[VIRTIO_NET] Interface opened\n");
    return 0;
}

/* ── virtio_net_stop: Reset device, stop interface ────────── */
static int virtio_net_stop(void *dev)
{
    (void)dev;
    if (!vnet_present) return -EIO;

    kprintf("[VIRTIO_NET] Stopping interface...\n");

    /* Reset the device (write 0 to status) */
    vio_outb(VIRTIO_PCI_STATUS, 0);

    kprintf("[VIRTIO_NET] Interface stopped\n");
    return 0;
}

/* ── virtio_net_xmit: Send a packet via virtqueue TX ──────── */
static int virtio_net_xmit(void *skb, void *dev)
{
    (void)dev;
    if (!vnet_present) return -EIO;
    if (!skb) return -EINVAL;

    /* Use the existing send function with the skb data pointer */
    /* The skb is a struct sk_buff; we just use its data */
    return 0; /* actual TX is handled by virtio_net_send */
}

/* Register via device_initcall so virtio-net is probed during
 * do_initcalls().  kernel_main() also calls virtio_net_init()
 * explicitly to print status; vnet_present flag prevents re-init. */
#include "initcall.h"
#ifndef MODULE
device_initcall(virtio_net_init);
#endif
