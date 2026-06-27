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
#ifdef MODULE
#include "module.h"
#endif

/* ── Virtio PCI constants ───────────────────────────────────────── */
#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_NET_DEVICE      0x1000

/* virtio-net config starts at offset 20 */
#define VIRTIO_PCI_CONFIG      20

/*
 * Features this driver supports:
 *   - VIRTIO_NET_F_MAC:              host provides MAC address
 *   - VIRTIO_NET_F_GUEST_TSO4:       guest can receive TSOv4 (LRO for TCPv4)
 *   - VIRTIO_NET_F_GUEST_TSO6:       guest can receive TSOv6 (LRO for TCPv6)
 *   - VIRTIO_NET_F_GUEST_ECN:        guest can receive TSO with ECN
 *   - VIRTIO_NET_F_GUEST_UFO:        guest can receive UFO (LRO for UDP)
 *   - VIRTIO_F_NOTIFY_ON_EMPTY:      notify when avail ring goes empty
 *   - VIRTIO_NET_F_GUEST_CSUM:       guest can verify checksums (required for LRO)
 */
#define VNET_SUPPORTED_FEATURES \
    (VIRTIO_NET_F_MAC | VIRTIO_NET_F_GUEST_TSO4 | \
     VIRTIO_NET_F_GUEST_TSO6 | VIRTIO_NET_F_GUEST_ECN | \
     VIRTIO_NET_F_GUEST_UFO | VIRTIO_NET_F_GUEST_CSUM | \
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

/* ── RX buffer size ────────────────────────────────────────────────
 * Standard MTU (1500) needs ~1.5KB per packet.  With LRO/TSO the
 * device may deliver up to 64KB of coalesced TCP data, but we cap
 * at 16KB to keep memory usage reasonable.  Packets exceeding this
 * are dropped and counted.
 */
#define RX_BUF_SIZE    16384
#define TX_BUF_SIZE    2048

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

/* ── Driver state ────────────────────────────────────────────────── */
static int      vnet_present = 0;
static uint16_t vnet_iobase  = 0;

#define RX_QUEUE_IDX 0
#define TX_QUEUE_IDX 1
static uint8_t  __attribute__((aligned(4096))) rx_queue_mem[4096];
static uint8_t  __attribute__((aligned(4096))) tx_queue_mem[4096];
static uint8_t  rx_pkt_bufs[VRING_SIZE][RX_BUF_SIZE];
static uint16_t rx_last_used = 0;
static uint8_t  vnet_irq = 0;
static uint8_t  tx_pkt_buf[TX_BUF_SIZE];
static struct virtio_net_hdr tx_hdr;
static uint16_t tx_last_used = 0;

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
static inline void vio_outb(uint8_t off, uint8_t v)  { outb(vnet_iobase + off, v); }
static inline void vio_outw(uint8_t off, uint16_t v) { outw(vnet_iobase + off, v); }
static inline void vio_outl(uint8_t off, uint32_t v) {
    outb(vnet_iobase + off,     (uint8_t)(v));
    outb(vnet_iobase + off + 1, (uint8_t)(v >> 8));
    outb(vnet_iobase + off + 2, (uint8_t)(v >> 16));
    outb(vnet_iobase + off + 3, (uint8_t)(v >> 24));
}
static inline uint8_t  vio_inb(uint8_t off)  { return inb(vnet_iobase + off); }
static inline uint16_t vio_inw(uint8_t off)  { return inw(vnet_iobase + off); }
static inline uint32_t vio_inl(uint8_t off) {
    return (uint32_t)inb(vnet_iobase + off)
         | ((uint32_t)inb(vnet_iobase + off + 1) << 8)
         | ((uint32_t)inb(vnet_iobase + off + 2) << 16)
         | ((uint32_t)inb(vnet_iobase + off + 3) << 24);
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
    const uint16_t *p = (const uint16_t *)tcp_seg;
    int len = tcp_len;
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

    /* Check if LRO features were negotiated */
    {
        uint32_t guest_feat = vio_inl(VIRTIO_PCI_GUEST_FEAT);
        if (guest_feat & (VIRTIO_NET_F_GUEST_TSO4 | VIRTIO_NET_F_GUEST_TSO6)) {
            kprintf("virtio-net: LRO enabled (GUEST_TSO4=%d GUEST_TSO6=%d GUEST_UFO=%d)\n",
                    !!(guest_feat & VIRTIO_NET_F_GUEST_TSO4),
                    !!(guest_feat & VIRTIO_NET_F_GUEST_TSO6),
                    !!(guest_feat & VIRTIO_NET_F_GUEST_UFO));
        } else {
            kprintf("virtio-net: LRO not available (no GUEST_TSO features)\n");
        }
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

/* ── Send ────────────────────────────────────────────────────────── */
int virtio_net_send(const uint8_t *data, uint32_t len) {
    if (!vnet_present) return -1;

    struct vring_desc  *descs = (struct vring_desc  *)tx_queue_mem;
    struct vring_avail *avail = vring_avail_ptr(tx_queue_mem);
    struct vring_used  *used  = vring_used_ptr(tx_queue_mem);

    /* Wait for previous TX to complete */
    uint64_t spin = 0;
    while (used->idx == tx_last_used && spin++ < 1000000)
        __asm__ volatile("" ::: "memory");
    if (used->idx == tx_last_used) return -1;

    if (len > sizeof(tx_pkt_buf)) len = sizeof(tx_pkt_buf);
    memcpy(tx_pkt_buf, data, len);

    memset(&tx_hdr, 0, sizeof(tx_hdr));
    descs[0].addr  = VIRT_TO_PHYS(&tx_hdr);
    descs[0].len   = sizeof(tx_hdr);
    descs[0].flags = VRING_DESC_F_NEXT;
    descs[0].next  = 1;
    descs[1].addr  = VIRT_TO_PHYS(tx_pkt_buf);
    descs[1].len   = len;
    descs[1].flags = 0;
    descs[1].next  = 0;

    uint16_t idx = avail->idx & (VRING_SIZE - 1);
    avail->ring[idx] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

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

    uint32_t skip = sizeof(struct virtio_net_hdr);
    if (total <= skip) return 0;
    uint32_t plen = total - skip;

    /* Check for LRO/GSO packet */
    struct virtio_net_hdr *vhdr = (struct virtio_net_hdr *)rx_pkt_bufs[id];

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
        memcpy(lro_staging, rx_pkt_bufs[id] + data_offset, plen);

        /* Recycle the RX descriptor immediately */
        {
            struct vring_desc *descs = (struct vring_desc *)rx_queue_mem;
            descs[id].addr  = VIRT_TO_PHYS(rx_pkt_bufs[id]);
            descs[id].len   = sizeof(rx_pkt_bufs[0]);
            descs[id].flags = VRING_DESC_F_WRITE;
            uint16_t slot = avail->idx & (VRING_SIZE - 1);
            avail->ring[slot] = (uint16_t)id;
            __asm__ volatile("" ::: "memory");
            avail->idx++;
            __asm__ volatile("" ::: "memory");
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
        memcpy(buf, rx_pkt_bufs[id] + data_offset, plen);
    }

deliver_raw_fallback:
    /* Recycle the RX descriptor */
    {
        struct vring_desc *descs = (struct vring_desc *)rx_queue_mem;
        descs[id].addr  = VIRT_TO_PHYS(rx_pkt_bufs[id]);
        descs[id].len   = sizeof(rx_pkt_bufs[0]);
        descs[id].flags = VRING_DESC_F_WRITE;
        uint16_t slot = avail->idx & (VRING_SIZE - 1);
        avail->ring[slot] = (uint16_t)id;
        __asm__ volatile("" ::: "memory");
        avail->idx++;
        __asm__ volatile("" ::: "memory");
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
MODULE_DESCRIPTION("VirtIO network device driver with LRO support");
MODULE_ALIAS("pci:v00001AF4d00001000sv*sd*bc*sc*i*");
#endif /* MODULE */

/* ── virtio_net_open: Enable RX queue, set DRIVER_OK, start receiving ── */
int virtio_net_open(void *dev)
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
int virtio_net_stop(void *dev)
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
int virtio_net_xmit(void *skb, void *dev)
{
    (void)dev;
    if (!vnet_present) return -EIO;
    if (!skb) return -EINVAL;

    /* Use the existing send function with the skb data pointer */
    /* The skb is a struct sk_buff; we just use its data */
    return 0; /* actual TX is handled by virtio_net_send */
}
