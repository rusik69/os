#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"

/* ── Virtio legacy PCI register offsets ────────────────────────── */
#define VIRTIO_PCI_HOST_FEAT      0x00  /* R   device feature bits    */
#define VIRTIO_PCI_GUEST_FEAT     0x04  /* RW  driver feature bits    */
#define VIRTIO_PCI_QUEUE_PFN      0x08  /* RW  queue phys page number */
#define VIRTIO_PCI_QUEUE_SIZE     0x0C  /* R   queue size             */
#define VIRTIO_PCI_QUEUE_SEL      0x0E  /* RW  queue selector         */
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10  /* W   queue notify           */
#define VIRTIO_PCI_STATUS         0x12  /* RW  device status          */
#define VIRTIO_PCI_ISR            0x13  /* R   ISR status             */

/* ── Device status bits ─────────────────────────────────────────── */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* ── Common feature bits (always valid) ─────────────────────────── */
#define VIRTIO_F_NOTIFY_ON_EMPTY    (1u << 24) /* notify when avail ring goes empty */
#define VIRTIO_F_RING_INDIRECT_DESC (1u << 28) /* indirect descriptors */
#define VIRTIO_F_RING_EVENT_IDX     (1u << 29) /* used ring event suppression */

/* ── Virtio-net feature bits ────────────────────────────────────── */
#define VIRTIO_NET_F_CSUM           (1u << 0)  /* host checksums */
#define VIRTIO_NET_F_GUEST_CSUM     (1u << 1)  /* guest checksums */
#define VIRTIO_NET_F_MAC            (1u << 5)  /* host provides MAC */
#define VIRTIO_NET_F_GSO            (1u << 6)  /* generic segmentation offload */
#define VIRTIO_NET_F_MRG_RXBUF      (1u << 15) /* mergeable receive buffers */
#define VIRTIO_NET_F_STATUS         (1u << 16) /* link status */
#define VIRTIO_NET_F_CTRL_VQ        (1u << 17) /* control channel */
#define VIRTIO_NET_F_CTRL_RX        (1u << 18) /* rx filtering */
#define VIRTIO_NET_F_CTRL_VLAN      (1u << 19) /* VLAN filtering */
#define VIRTIO_NET_F_GUEST_ANNOUNCE (1u << 21) /* guest announces changes */

/* ── Virtio-blk feature bits ────────────────────────────────────── */
#define VIRTIO_BLK_F_SIZE_MAX       (1u << 1)  /* max segment size */
#define VIRTIO_BLK_F_SEG_MAX        (1u << 2)  /* max segments per request */
#define VIRTIO_BLK_F_GEOMETRY       (1u << 4)  /* disk geometry */
#define VIRTIO_BLK_F_RO             (1u << 5)  /* read-only */
#define VIRTIO_BLK_F_BLK_SIZE       (1u << 6)  /* block size (non-512) */
#define VIRTIO_BLK_F_FLUSH          (1u << 9)  /* cache flush command */
#define VIRTIO_BLK_F_TOPOLOGY       (1u << 10) /* optimal I/O alignment */
#define VIRTIO_BLK_F_CONFIG_WCE     (1u << 11) /* write cache enable in config */
#define VIRTIO_BLK_F_DISCARD        (1u << 12) /* discard command */
#define VIRTIO_BLK_F_WRITE_ZEROES   (1u << 13) /* write zeroes command */

/* ── Feature negotiation helper ──────────────────────────────────────
 * Reads host features from offset 0x00, ANDs with supported_features,
 * writes back to guest features at offset 0x04, then sets FEATURES_OK
 * and validates the device accepted them.
 *
 * The callbacks take a PCI register offset (not the full I/O address).
 * Each driver's own static inline helpers close over its iobase variable,
 * so this helper just passes relative register offsets.
 *
 * readl:  function that reads 32 bits from (iobase + offset).
 * writel: function that writes 32 bits to  (iobase + offset).
 * writeb: function that writes  8 bits to  (iobase + offset).
 * readb:  function that reads   8 bits from (iobase + offset).
 *
 * Returns 0 on success, -1 if device rejected the negotiated features.
 * ──────────────────────────────────────────────────────────────── */

static inline int virtio_negotiate_features(
    uint32_t (*readl)(uint8_t off),
    void     (*writel)(uint8_t off, uint32_t v),
    void     (*writeb)(uint8_t off, uint8_t v),
    uint8_t  (*readb)(uint8_t off),
    uint32_t supported_features)
{
    /* Read host features (32-bit) */
    uint32_t host_feat = readl(VIRTIO_PCI_HOST_FEAT);

    /* Negotiate: subset of what we support ∩ what the device offers */
    uint32_t guest_feat = host_feat & supported_features;

    /* Write guest features */
    writel(VIRTIO_PCI_GUEST_FEAT, guest_feat);

    /* Memory barrier before status write */
    __asm__ volatile("" ::: "memory");

    /* Set FEATURES_OK status bit (device validates features now) */
    uint8_t status = readb(VIRTIO_PCI_STATUS);
    status |= VIRTIO_STATUS_FEATURES_OK;
    writeb(VIRTIO_PCI_STATUS, status);

    /* Memory barrier before reading status back */
    __asm__ volatile("" ::: "memory");

    /* Verify device accepted the negotiated features */
    status = readb(VIRTIO_PCI_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        /* Device rejected features — this is a fatal error */
        return -1;
    }

    return 0;
}

#endif /* VIRTIO_H */
