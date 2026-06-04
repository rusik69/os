#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"
#include "printf.h"
#include "string.h"

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
#define VIRTIO_NET_F_GUEST_TSO4     (1u << 7)  /* guest can receive TSOv4 (LRO) */
#define VIRTIO_NET_F_GUEST_TSO6     (1u << 8)  /* guest can receive TSOv6 (LRO) */
#define VIRTIO_NET_F_GUEST_ECN      (1u << 9)  /* guest can receive TSO with ECN */
#define VIRTIO_NET_F_GUEST_UFO      (1u << 10) /* guest can receive UFO (LRO) */
#define VIRTIO_NET_F_HOST_TSO4      (1u << 11) /* host can receive TSOv4 */
#define VIRTIO_NET_F_HOST_TSO6      (1u << 12) /* host can receive TSOv6 */
#define VIRTIO_NET_F_HOST_ECN       (1u << 13) /* host can receive TSO with ECN */
#define VIRTIO_NET_F_HOST_UFO       (1u << 14) /* host can receive UFO */
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

/* ── Feature bits / name table for human-readable diagnostics ── */
#define VIRTIO_FEATURE_NAME(f)  (f & 1u) /* sentinel: caller must mask bit */

struct virtio_feature_entry {
    uint32_t bit;
    const char *name;
};

#define VIRTIO_FEATURE(_bit) { .bit = (_bit), .name = #_bit }

static const struct virtio_feature_entry virtio_common_features[] = {
    VIRTIO_FEATURE(VIRTIO_F_NOTIFY_ON_EMPTY),
    VIRTIO_FEATURE(VIRTIO_F_RING_INDIRECT_DESC),
    VIRTIO_FEATURE(VIRTIO_F_RING_EVENT_IDX),
    { .bit = 0, .name = NULL } /* terminator */
};

static const struct virtio_feature_entry virtio_net_features[] = {
    VIRTIO_FEATURE(VIRTIO_NET_F_CSUM),
    VIRTIO_FEATURE(VIRTIO_NET_F_GUEST_CSUM),
    VIRTIO_FEATURE(VIRTIO_NET_F_MAC),
    VIRTIO_FEATURE(VIRTIO_NET_F_GSO),
    VIRTIO_FEATURE(VIRTIO_NET_F_GUEST_TSO4),
    VIRTIO_FEATURE(VIRTIO_NET_F_GUEST_TSO6),
    VIRTIO_FEATURE(VIRTIO_NET_F_GUEST_ECN),
    VIRTIO_FEATURE(VIRTIO_NET_F_GUEST_UFO),
    VIRTIO_FEATURE(VIRTIO_NET_F_HOST_TSO4),
    VIRTIO_FEATURE(VIRTIO_NET_F_HOST_TSO6),
    VIRTIO_FEATURE(VIRTIO_NET_F_HOST_ECN),
    VIRTIO_FEATURE(VIRTIO_NET_F_HOST_UFO),
    VIRTIO_FEATURE(VIRTIO_NET_F_MRG_RXBUF),
    VIRTIO_FEATURE(VIRTIO_NET_F_STATUS),
    VIRTIO_FEATURE(VIRTIO_NET_F_CTRL_VQ),
    VIRTIO_FEATURE(VIRTIO_NET_F_CTRL_RX),
    VIRTIO_FEATURE(VIRTIO_NET_F_CTRL_VLAN),
    VIRTIO_FEATURE(VIRTIO_NET_F_GUEST_ANNOUNCE),
    { .bit = 0, .name = NULL }
};

static const struct virtio_feature_entry virtio_blk_features[] = {
    VIRTIO_FEATURE(VIRTIO_BLK_F_SIZE_MAX),
    VIRTIO_FEATURE(VIRTIO_BLK_F_SEG_MAX),
    VIRTIO_FEATURE(VIRTIO_BLK_F_GEOMETRY),
    VIRTIO_FEATURE(VIRTIO_BLK_F_RO),
    VIRTIO_FEATURE(VIRTIO_BLK_F_BLK_SIZE),
    VIRTIO_FEATURE(VIRTIO_BLK_F_FLUSH),
    VIRTIO_FEATURE(VIRTIO_BLK_F_TOPOLOGY),
    VIRTIO_FEATURE(VIRTIO_BLK_F_CONFIG_WCE),
    VIRTIO_FEATURE(VIRTIO_BLK_F_DISCARD),
    VIRTIO_FEATURE(VIRTIO_BLK_F_WRITE_ZEROES),
    { .bit = 0, .name = NULL }
};

/* ── Feature dependency table ────────────────────────────────────
 * Some virtio features require other features to be negotiated first.
 * Format: { feature_bit, required_bit, "description" }
 */
struct virtio_feature_dep {
    uint32_t feature;
    uint32_t required;
    const char *desc;
};

#define VIRTIO_DEP(_feat, _req, _desc) { .feature = (_feat), .required = (_req), .desc = (_desc) }

static const struct virtio_feature_dep virtio_dependencies[] = {
    /* Indirect descriptors require event index (per spec recommendation) */
    VIRTIO_DEP(VIRTIO_F_RING_INDIRECT_DESC, VIRTIO_F_RING_EVENT_IDX,
               "RING_INDIRECT_DESC depends on RING_EVENT_IDX"),
    /* Mergeable RX buffers require guest checksum offload */
    VIRTIO_DEP(VIRTIO_NET_F_MRG_RXBUF, VIRTIO_NET_F_GUEST_CSUM,
               "MRG_RXBUF depends on GUEST_CSUM"),
    { .feature = 0, .required = 0, .desc = NULL }
};

/* ── Internal: format feature bits into human-readable string ────
 * Writes a comma-separated list of known feature names into 'buf'
 * of size 'bufsz'. Unknown bits are formatted as hex "0x%X".
 * Returns the number of chars written (excluding NUL).
 * The table array must be terminated by { .bit = 0, .name = NULL }.
 */
static inline int virtio_format_features(char *buf, size_t bufsz,
                                          uint32_t bits,
                                          const struct virtio_feature_entry *table,
                                          const struct virtio_feature_entry *common_table)
{
    int pos = 0;
    int first = 1;
    uint32_t remaining = bits;

    if (!buf || bufsz == 0) return 0;

    buf[0] = '\0';

    /* Helper to emit a single feature name */
#define EMIT_FEATURE(name) do { \
        if (!first) { \
            if ((size_t)pos + 2 > bufsz) goto done; \
            buf[pos++] = ','; buf[pos++] = ' '; \
        } \
        size_t nlen = strlen(name); \
        if ((size_t)pos + nlen + 1 > bufsz) goto done; \
        memcpy(buf + pos, name, nlen); \
        pos += (int)nlen; \
        first = 0; \
    } while(0)

    /* Check common table first, then device-specific table */
    for (const struct virtio_feature_entry *e = common_table; e && e->name && e->bit; e++) {
        if (bits & e->bit) {
            EMIT_FEATURE(e->name);
            remaining &= ~e->bit;
        }
    }
    for (const struct virtio_feature_entry *e = table; e && e->name && e->bit; e++) {
        if (bits & e->bit) {
            EMIT_FEATURE(e->name);
            remaining &= ~e->bit;
        }
    }

    /* Any unknown bits remaining? */
    for (int b = 0; b < 32; b++) {
        if (remaining & (1u << b)) {
            char hex[12];
            int hlen = snprintf(hex, sizeof(hex), "0x%X", 1u << b);
            if (hlen > 0) {
                if (!first) {
                    if ((size_t)pos + 2 > bufsz) goto done;
                    buf[pos++] = ','; buf[pos++] = ' ';
                }
                if ((size_t)pos + (size_t)hlen + 1 > bufsz) goto done;
                memcpy(buf + pos, hex, (size_t)hlen);
                pos += hlen;
                first = 0;
            }
            remaining &= ~(1u << b);
        }
    }

#undef EMIT_FEATURE

done:
    if ((size_t)pos < bufsz)
        buf[pos] = '\0';
    else if (bufsz > 0)
        buf[bufsz - 1] = '\0';
    return pos;
}

/* ── Validate feature dependencies ───────────────────────────────
 * Checks that all required dependent features are set in 'negotiated'.
 * Returns 0 if all dependencies satisfied, or writes a message into
 * err_buf (size err_sz) and returns -1 on first violation.
 */
static inline int virtio_check_dependencies(uint32_t negotiated,
                                             char *err_buf, size_t err_sz)
{
    for (const struct virtio_feature_dep *d = virtio_dependencies;
         d->feature != 0; d++) {
        if ((negotiated & d->feature) && !(negotiated & d->required)) {
            if (err_buf && err_sz > 0) {
                int n = snprintf(err_buf, err_sz,
                                 "Feature dependency violation: %s",
                                 d->desc ? d->desc : "unspecified");
                if (n < 0 || (size_t)n >= err_sz)
                    err_buf[err_sz - 1] = '\0';
            }
            return -1;
        }
    }
    return 0;
}

/* ── Feature negotiation helper (enhanced) ─────────────────────────
 * Reads host features from offset 0x00, validates required features,
 * negotiates with the device, sets FEATURES_OK, and validates.
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
 * supported:   All features this driver CAN work with (OR of feature bits).
 * required:    Subset of 'supported' that MUST be offered by the device.
 * feat_table:  Feature name table for this device type (may be NULL).
 *              Common feature names are always resolved.
 * driver_name: Human-readable driver name for log messages (e.g. "virtio-blk").
 *
 * Returns 0 on success, -1 on failure (with kprintf diagnostics).
 * ──────────────────────────────────────────────────────────────── */

static inline int virtio_negotiate_features_ex(
    uint32_t (*readl)(uint8_t off),
    void     (*writel)(uint8_t off, uint32_t v),
    void     (*writeb)(uint8_t off, uint8_t v),
    uint8_t  (*readb)(uint8_t off),
    uint32_t supported,
    uint32_t required,
    const struct virtio_feature_entry *feat_table,
    const char *driver_name)
{
    char buf[256];

    if (!driver_name)
        driver_name = "virtio";

    /* Step 1: Read host (device) features */
    uint32_t host_feat = readl(VIRTIO_PCI_HOST_FEAT);

    /* Log host features */
    buf[0] = '\0';
    virtio_format_features(buf, sizeof(buf), host_feat, feat_table, virtio_common_features);
    kprintf("%s: device features (0x%08X): %s\n", driver_name,
            (unsigned int)host_feat, buf[0] ? buf : "(none)");

    /* Step 2: Validate required features are present */
    uint32_t missing_required = host_feat & required;
    if (missing_required != required) {
        uint32_t missing = required & ~host_feat;
        buf[0] = '\0';
        virtio_format_features(buf, sizeof(buf), missing, feat_table, virtio_common_features);
        kprintf("%s: ERROR: device missing required features (0x%08X): %s\n",
                driver_name, (unsigned int)missing, buf[0] ? buf : "(unknown)");
        return -1;
    }

    /* Step 3: Negotiate features: supported ∩ device */
    uint32_t guest_feat = host_feat & supported;

    /* Write guest features */
    writel(VIRTIO_PCI_GUEST_FEAT, guest_feat);

    /* Memory barrier before status write */
    __asm__ volatile("" ::: "memory");

    /* Step 4: Set FEATURES_OK status bit (device validates features) */
    uint8_t status = readb(VIRTIO_PCI_STATUS);
    status |= VIRTIO_STATUS_FEATURES_OK;
    writeb(VIRTIO_PCI_STATUS, status);

    /* Memory barrier before reading status back */
    __asm__ volatile("" ::: "memory");

    /* Step 5: Verify device accepted the negotiated features */
    status = readb(VIRTIO_PCI_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        uint8_t dev_status = readb(VIRTIO_PCI_STATUS);
        buf[0] = '\0';
        virtio_format_features(buf, sizeof(buf), guest_feat, feat_table, virtio_common_features);
        kprintf("%s: ERROR: device rejected negotiated features (0x%08X, status=0x%02X): %s\n",
                driver_name, (unsigned int)guest_feat, (unsigned int)dev_status,
                buf[0] ? buf : "(none)");
        return -1;
    }

    /* Step 6: Read back guest features to confirm the device accepted them */
    uint32_t guest_feat_readback = readl(VIRTIO_PCI_GUEST_FEAT);
    if (guest_feat_readback != guest_feat) {
        kprintf("%s: WARNING: guest features readback mismatch: wrote 0x%08X, read 0x%08X\n",
                driver_name, (unsigned int)guest_feat, (unsigned int)guest_feat_readback);
        /* This is not necessarily fatal — some legacy devices may not preserve
         * the write-back of guest features. Accept what we read back. */
        guest_feat = guest_feat_readback & supported;
    }

    /* Step 7: Check feature dependencies */
    char dep_err[128];
    if (virtio_check_dependencies(guest_feat, dep_err, sizeof(dep_err)) < 0) {
        kprintf("%s: WARNING: %s — continuing (device may misbehave)\n",
                driver_name, dep_err);
    }

    /* Log negotiated features */
    buf[0] = '\0';
    virtio_format_features(buf, sizeof(buf), guest_feat, feat_table, virtio_common_features);
    kprintf("%s: negotiated features (0x%08X): %s\n", driver_name,
            (unsigned int)guest_feat, buf[0] ? buf : "(none)");

    return 0;
}

/* ── Legacy wrapper for backward compatibility ───────────────────
 * Thin wrapper that calls virtio_negotiate_features_ex with
 * required=0, feat_table=NULL, driver_name=NULL.
 * Returns 0 on success, -1 on failure.
 */

static inline int virtio_negotiate_features(
    uint32_t (*readl)(uint8_t off),
    void     (*writel)(uint8_t off, uint32_t v),
    void     (*writeb)(uint8_t off, uint8_t v),
    uint8_t  (*readb)(uint8_t off),
    uint32_t supported_features)
{
    return virtio_negotiate_features_ex(readl, writel, writeb, readb,
                                         supported_features, 0, NULL, NULL);
}

#endif /* VIRTIO_H */
