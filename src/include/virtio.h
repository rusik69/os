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

/* ── Virtio 1.0+ modern PCI capability IDs (capability type values) ── */
#define VIRTIO_PCI_CAP_COMMON_CFG      1  /* Common configuration          */
#define VIRTIO_PCI_CAP_NOTIFY_CFG      2  /* Notifications                 */
#define VIRTIO_PCI_CAP_ISR_CFG         3  /* ISR status                    */
#define VIRTIO_PCI_CAP_DEVICE_CFG      4  /* Device-specific configuration */
#define VIRTIO_PCI_CAP_PCI_CFG         5  /* PCI config access (legacy)    */
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8  /* Shared memory region       */
#define VIRTIO_PCI_CAP_VENDOR_CFG      9  /* Vendor-specific data          */

/* Virtio vendor-specific PCI capability ID */
#define VIRTIO_PCI_VENDOR_CAP_ID       0x09

/* ── Virtio common configuration structure (MMIO) ───────────────── */
#pragma pack(push, 1)
struct virtio_pci_common_cfg {
    uint32_t device_feature_select;  /* R/W: selects which 32 bits of dev features */
    uint32_t device_feature;         /* RO:  selected device feature bits          */
    uint32_t driver_feature_select;  /* R/W: selects which 32 bits of drv features */
    uint32_t driver_feature;         /* R/W: driver feature bits to write          */
    uint16_t msix_config;            /* R/W: MSI-X vector for config change        */
    uint16_t num_queues;             /* RO:  number of virtqueues the device has   */
    uint8_t  device_status;          /* R/W: device status register                */
    uint8_t  config_generation;      /* RO:  generation count for config changes   */
    uint16_t queue_select;           /* R/W: selects which queue to configure      */
    uint16_t queue_size;             /* R/W: size of the selected queue            */
    uint16_t queue_msix_vector;      /* R/W: MSI-X vector for this queue           */
    uint16_t queue_enable;           /* R/W: enable/disable the selected queue     */
    uint16_t queue_notify_off;       /* RO:  offset into notify region for this q  */
    uint64_t queue_desc;             /* R/W: physical address of descriptor area   */
    uint64_t queue_driver;           /* R/W: physical address of driver area       */
    uint64_t queue_device;           /* R/W: physical address of device area       */
    uint16_t queue_notify_data;      /* RO:  data to write for notification (opt)  */
};
#pragma pack(pop)

/* ── Virtio PCI capability structure (in PCI config space) ──────── */
#pragma pack(push, 1)
struct virtio_pci_cap {
    uint8_t  cap_vndr;       /* Generic PCI capability: vendor ID = 0x09         */
    uint8_t  cap_next;       /* Generic PCI capability: next capability pointer   */
    uint8_t  cap_len;        /* Generic PCI capability: length of this structure  */
    uint8_t  cfg_type;       /* Virtio-specific: type of this capability          */
    uint8_t  bar;            /* PCI BAR that contains the region                  */
    uint8_t  padding[3];     /* Padding to align offset/length (spec requirement) */
    uint32_t offset;         /* Offset within the BAR where this region starts    */
    uint32_t length;         /* Length of the region                              */
};
#pragma pack(pop)

/* ── Virtio notify capability (includes multiplier) ─────────────── */
#pragma pack(push, 1)
struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;         /* Base capability structure              */
    uint32_t notify_off_multiplier;    /* Multiplier for queue_notify_off value  */
};
#pragma pack(pop)

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
#define VIRTIO_F_RING_PACKED        (1u << 30) /* packed virtqueue format (virtio 1.1) */

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
#define VIRTIO_NET_F_GUEST_ANNOUNCE (1u << 21)

/* ── Virtio-net 64-bit feature bits (not negotiable via legacy 32-bit transport) ── */
/* Defined as 64-bit constants for reference; the modern transport supports 64-bit
 * feature negotiation through the common cfg structure (device_feature_select). */
#define VIRTIO_NET_F_HASH_REPORT  (1ULL << 58)  /* device reports packet hash */
#define VIRTIO_NET_F_RSS          (1ULL << 60)  /* RSS receive-side steering */

/* ── RSS hash report type constants ──────────────────────────────── */
#define VIRTIO_NET_HASH_REPORT_NONE      0
#define VIRTIO_NET_HASH_REPORT_IPv4      1
#define VIRTIO_NET_HASH_REPORT_TCPv4     2
#define VIRTIO_NET_HASH_REPORT_UDPv4     3
#define VIRTIO_NET_HASH_REPORT_IPv6      4
#define VIRTIO_NET_HASH_REPORT_TCPv6     5
#define VIRTIO_NET_HASH_REPORT_UDPv6     6
#define VIRTIO_NET_HASH_REPORT_IPv6_EX   7
#define VIRTIO_NET_HASH_REPORT_TCPv6_EX  8
#define VIRTIO_NET_HASH_REPORT_UDPv6_EX  9

/* ── RSS hash type enable bitmask ────────────────────────────────── */
#define VIRTIO_NET_HASH_TYPE_IPv4         (1u << 0)
#define VIRTIO_NET_HASH_TYPE_TCPv4        (1u << 1)
#define VIRTIO_NET_HASH_TYPE_UDPv4        (1u << 2)
#define VIRTIO_NET_HASH_TYPE_IPv6         (1u << 3)
#define VIRTIO_NET_HASH_TYPE_TCPv6        (1u << 4)
#define VIRTIO_NET_HASH_TYPE_UDPv6        (1u << 5)
#define VIRTIO_NET_HASH_TYPE_IPv6_EX      (1u << 6)
#define VIRTIO_NET_HASH_TYPE_TCPv6_EX     (1u << 7)
#define VIRTIO_NET_HASH_TYPE_UDPv6_EX     (1u << 8)

/* ── RSS configuration structure (virtio 1.1 spec §5.1.3.6) ──────── *
 * The indirection table and hash key follow the fixed header in memory:
 *   struct virtio_net_rss_config cfg;
 *   uint16_t indirection_table[indirection_table_length];
 *   uint8_t  key[40];
 */
#pragma pack(push, 1)
struct virtio_net_rss_config {
    uint32_t hash_types;              /* bitmask of VIRTIO_NET_HASH_TYPE_* */
    uint16_t indirection_table_length; /* number of entries in indirection table */
    uint16_t indirection_table_start;  /* placeholder: first entry of indir table follow */
    uint16_t max_tx_vq;               /* max TX virtqueue index */
    uint16_t unclassified_queue;       /* queue for packets that don't match hash types */
    /* Followed by:
     *   uint16_t indirection_table[indirection_table_length];
     *   uint8_t  key[40];
     */
};
#pragma pack(pop)

/* ── Control VQ classes / commands ──────────────────────────────── *
 * VIRTIO_NET_F_CTRL_VQ must be negotiated for these to be available. */
#define VIRTIO_NET_CTRL_MQ                0x02  /* MQ class */
#define VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET   0x01  /* set # of active queue pairs */
#define VIRTIO_NET_CTRL_MQ_RSS_CONFIG     0x02  /* set RSS configuration */

/* ── Control VQ: RX filtering class (requires VIRTIO_NET_F_CTRL_RX) ── */
#define VIRTIO_NET_CTRL_RX                 0x01
#define VIRTIO_NET_CTRL_RX_PROMISC         0   /* toggle promiscuous mode */
#define VIRTIO_NET_CTRL_RX_ALLMULTI        1   /* toggle all-multicast mode */
#define VIRTIO_NET_CTRL_RX_NO_MCAST        2   /* disable multicast */
#define VIRTIO_NET_CTRL_RX_NO_UCAST        3   /* disable unicast */
#define VIRTIO_NET_CTRL_RX_NOMCAST         4   /* disable multicast (alt name) */

/* ── Control VQ: MAC table class ──────────────────────────────────── */
#define VIRTIO_NET_CTRL_MAC                0x03
#define VIRTIO_NET_CTRL_MAC_TABLE_SET      0   /* set unicast/multicast MAC filter table */
#define VIRTIO_NET_CTRL_MAC_ADDR_SET       1   /* set default MAC address */

/* ── Control VQ: ACK status values ───────────────────────────────── */
#define VIRTIO_NET_OK                      0
#define VIRTIO_NET_ERR                     1

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
#define VIRTIO_BLK_F_MQ             (1u << 14) /* multi-queue (virtio 1.1+) */
#define VIRTIO_BLK_F_LIFETIME       (1u << 17) /* life-time fields in config */

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
    VIRTIO_FEATURE(VIRTIO_BLK_F_MQ),
    VIRTIO_FEATURE(VIRTIO_BLK_F_LIFETIME),
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
    uint32_t (*dev_readl)(uint8_t off),
    void     (*dev_writel)(uint8_t off, uint32_t v),
    void     (*dev_writeb)(uint8_t off, uint8_t v),
    uint8_t  (*dev_readb)(uint8_t off),
    uint32_t supported,
    uint32_t required,
    const struct virtio_feature_entry *feat_table,
    const char *driver_name)
{
    char buf[256];

    if (!driver_name)
        driver_name = "virtio";

    /* Step 1: Read host (device) features */
    uint32_t host_feat = dev_readl(VIRTIO_PCI_HOST_FEAT);

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
    dev_writel(VIRTIO_PCI_GUEST_FEAT, guest_feat);

    /* Memory barrier before status write */
    __asm__ volatile("" ::: "memory");

    /* Step 4: Set FEATURES_OK status bit (device validates features) */
    uint8_t status = dev_readb(VIRTIO_PCI_STATUS);
    status |= VIRTIO_STATUS_FEATURES_OK;
    dev_writeb(VIRTIO_PCI_STATUS, status);

    /* Memory barrier before reading status back */
    __asm__ volatile("" ::: "memory");

    /* Step 5: Verify device accepted the negotiated features */
    status = dev_readb(VIRTIO_PCI_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        uint8_t dev_status = dev_readb(VIRTIO_PCI_STATUS);
        buf[0] = '\0';
        virtio_format_features(buf, sizeof(buf), guest_feat, feat_table, virtio_common_features);
        kprintf("%s: ERROR: device rejected negotiated features (0x%08X, status=0x%02X): %s\n",
                driver_name, (unsigned int)guest_feat, (unsigned int)dev_status,
                buf[0] ? buf : "(none)");
        return -1;
    }

    /* Step 6: Read back guest features to confirm the device accepted them */
    uint32_t guest_feat_readback = dev_readl(VIRTIO_PCI_GUEST_FEAT);
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
    uint32_t (*dev_readl)(uint8_t off),
    void     (*dev_writel)(uint8_t off, uint32_t v),
    void     (*dev_writeb)(uint8_t off, uint8_t v),
    uint8_t  (*dev_readb)(uint8_t off),
    uint32_t supported_features)
{
    return virtio_negotiate_features_ex(dev_readl, dev_writel, dev_writeb, dev_readb,
                                         supported_features, 0, NULL, NULL);
}

/* ── Modern (virtio 1.0+) PCI transport declarations ──────────────
 *
 * The modern virtio PCI transport uses MMIO regions discovered via
 * vendor-specific PCI capabilities.  These functions provide the
 * transport layer that device-specific drivers (virtio-net, -blk, etc.)
 * call during init.
 *
 * virtio_pci_modern_probe() discovers a modern virtio device by finding
 * its PCI capabilities.  The caller must provide a struct pci_device that
 * was obtained via pci_find_device().
 *
 * If successful, it fills in the vpci_modern structure with pointers to
 * the mapped MMIO regions, which are then used with the inline helpers
 * below.
 */

/* ── Modern virtio PCI device state ────────────────────────────── */
struct vpci_modern_caps {
    /* MMIO virtual addresses of each capability region (NULL if absent) */
    volatile struct virtio_pci_common_cfg *common;
    volatile void  *notify_base;       /* base of notification region     */
    volatile uint8_t *isr;             /* ISR status (single byte)        */
    volatile void  *device_cfg;        /* device-specific config region   */
    uint32_t notify_off_multiplier;    /* multiplier for queue offsets    */
    uint32_t notify_base_off;          /* offset of notify region in BAR  */
};

struct vpci_modern_device {
    const struct pci_device  *pci_dev;     /* pointer to the probed PCI device */
    struct vpci_modern_caps  caps;
    int                      modern_found;  /* 1 if modern caps detected  */
};

/* ── Function declarations ─────────────────────────────────────── */

/* Probe a PCI device for modern virtio capabilities.
 * Checks for PCI revision >= 1 (transitional/modern) and scans
 * for virtio vendor-specific capabilities.
 * Returns 0 on success with caps populated, -1 if not modern. */
int virtio_pci_modern_probe(struct pci_device *dev,
                            struct vpci_modern_device *vdev);

/* Initialize MMIO mapping for the caps using PHYS_TO_VIRT on the BAR.
 * The kernel identity-maps all physical memory (KERNEL_VMA_OFFSET).
 * Returns 0 on success, -1 on error. */
int virtio_pci_modern_map_bars(struct vpci_modern_device *vdev);

/* Full device initialization sequence (modern transport):
 *   reset -> ACK -> feature negotiation -> DRIVER_OK
 * 'supported' and 'required' work like virtio_negotiate_features_ex.
 * Returns 0 on success, -1 on failure. */
int virtio_pci_modern_init_device(struct vpci_modern_device *vdev,
                                  uint32_t supported, uint32_t required,
                                  const struct virtio_feature_entry *feat_table,
                                  const char *driver_name);

/* Set up a single virtqueue for the modern transport.
 * 'vq_pfn' is the physical page number (address >> 12) of the queue memory.
 * queue_size must be a power of 2.
 * Returns 0 on success, -1 on failure. */
int virtio_pci_modern_setup_queue(struct vpci_modern_device *vdev,
                                  uint16_t queue_idx, uint16_t queue_size,
                                  uint64_t desc_paddr, uint64_t driver_paddr,
                                  uint64_t device_paddr);

/* Set up a packed virtqueue for the modern transport (virtio 1.1+).
 * Unlike the split-queue setup, packed virtqueues use only the desc_paddr
 * field; driver_paddr and device_paddr are not used (write 0).
 * Returns 0 on success, -1 on failure. */
int virtio_pci_modern_setup_packed_queue(struct vpci_modern_device *vdev,
                                         uint16_t queue_idx, uint16_t queue_size,
                                         uint64_t desc_paddr);

/* Notify the device that a new buffer has been added to a queue. */
void virtio_pci_modern_notify_queue(struct vpci_modern_device *vdev,
                                    uint16_t queue_idx);

/* Enable MSI-X for the device.
 * vectors:   array of interrupt vector numbers (one per MSI-X table entry)
 * apic_ids:  array of destination APIC IDs
 * n:         number of entries to program (<= MSI-X table size)
 * Returns 0 on success, -1 on failure. */
int virtio_pci_modern_enable_msix(struct vpci_modern_device *vdev,
                                  const uint8_t *vectors,
                                  const uint32_t *apic_ids, int n);

/* Disable MSI-X and restore INTX operation. */
void virtio_pci_modern_disable_msix(struct vpci_modern_device *vdev);

/* Read device-specific config from the modern device config region.
 * Returns number of bytes read (0 if no device cfg region). */
uint32_t virtio_pci_modern_read_cfg(struct vpci_modern_device *vdev,
                                    uint32_t offset, void *buf, uint32_t len);

/* Write device-specific config to the modern device config region.
 * Returns number of bytes written (0 if no device cfg region). */
uint32_t virtio_pci_modern_write_cfg(struct vpci_modern_device *vdev,
                                     uint32_t offset, const void *buf,
                                     uint32_t len);

/* ── Packed virtqueue definitions (VirtIO 1.1) ──────────────────── */

/* Packed descriptor flags (additional, for VirtIO 1.1 packed mode) */
#define PVIRTQ_DESC_F_AVAIL        (1u << 7)   /* descriptor is available for device */
#define PVIRTQ_DESC_F_USED         (1u << 15)  /* descriptor has been used by device */

/* Packed descriptor structure (16 bytes each) */
#pragma pack(push, 1)
struct pvirtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#pragma pack(pop)

/* Packed virtqueue ring layout in memory */
struct pvirtq {
    /* Descriptor ring: desc[queue_size] */
    struct pvirtq_desc desc[0];

    /*
     * After desc[queue_size], at offset queue_size * sizeof(struct pvirtq_desc):
     *   uint16_t avail_event;  -- Driver writes to control device notification
     *   uint16_t used_event;   -- Device signals used buffers to driver
     */
};

/* Driver-side packed virtqueue state */
struct virtio_packed_vq {
    void                      *mem;          /* allocated queue memory (page-aligned) */
    uint64_t                   mem_phys;     /* physical address of queue memory */
    size_t                     mem_size;     /* allocated size in bytes */

    struct pvirtq             *ring;         /* pointer into mem for ring access */

    uint16_t                   queue_idx;    /* queue index in the device */
    uint16_t                   queue_size;   /* number of descriptors (power of 2) */
    uint16_t                   free_count;   /* number of free descriptors */

    uint16_t                   avail_wrap;   /* driver wrap counter (0 or 1) */
    uint16_t                   used_wrap;    /* device wrap counter (0 or 1) */

    uint16_t                   next_idx;     /* next descriptor index to submit */
    uint16_t                   last_used;    /* last-seen used-scanned index */

    int                        modern;       /* 1 = modern PCI transport */
    struct vpci_modern_device *vdev;         /* modern transport state (if modern) */
};

/* ── Packed virtqueue API ──────────────────────────────────────── */

/* Compute the total memory needed for a packed virtqueue of 'n' descriptors.
 * Returns the size in bytes (page-aligned). */
size_t virtio_packed_vq_size(uint16_t n);

/* Allocate and initialize a packed virtqueue.
 * If 'modern' is non-zero, the queue is also registered with the modern
 * PCI transport via virtio_pci_modern_setup_packed_queue().
 * Returns 0 on success, -1 on failure. */
int virtio_packed_vq_init(struct virtio_packed_vq *vq,
                          uint16_t queue_idx, uint16_t queue_size,
                          struct vpci_modern_device *vdev);

/* Tear down a packed virtqueue (release memory). */
void virtio_packed_vq_cleanup(struct virtio_packed_vq *vq);

/* Add a single descriptor to the packed ring (no chaining).
 * 'addr' is the physical address of the buffer.
 * 'len' is the buffer length.
 * 'write' is non-zero for device-writable descriptors.
 * Returns 0 on success, -1 if the ring is full. */
int virtio_packed_add_buf(struct virtio_packed_vq *vq,
                          uint64_t addr, uint32_t len, int write);

/* Add a descriptor chain (scatter-gather) to the packed ring.
 * 'addrs', 'lens', 'flags' are arrays of 'n' entries.
 * 'n' is the number of descriptors in the chain.
 * Returns 0 on success, -1 if not enough free descriptors. */
int virtio_packed_add_buf_sg(struct virtio_packed_vq *vq,
                             const uint64_t *addrs, const uint32_t *lens,
                             const uint16_t *flags, int n);

/* Notify the device that new buffers are available on this queue. */
void virtio_packed_kick(struct virtio_packed_vq *vq);

/* Poll for a completed buffer.
 * Returns the descriptor index of a used buffer, or -1 if none available.
 * If 'len_out' is non-NULL, it receives the length written by the device. */
int virtio_packed_get_buf(struct virtio_packed_vq *vq, uint32_t *len_out);

/* Poll until at least one buffer completes (with timeout in iterations).
 * Returns 0 on success, -1 on timeout. */
int virtio_packed_wait_buf(struct virtio_packed_vq *vq, uint32_t timeout);

/* Enable/disable event suppression for this queue.
 * When suppression is on, the device will not notify the driver for
 * every used buffer (reduces interrupt rate). */
void virtio_packed_enable_event(struct virtio_packed_vq *vq);
void virtio_packed_disable_event(struct virtio_packed_vq *vq);

/* ── Virtio-input feature bits (spec §5.10) ──────────────────────── */
#define VIRTIO_INPUT_F_EVENTS       (1u << 0)  /* event virtqueue available */
#define VIRTIO_INPUT_F_STATUS       (1u << 1)  /* status virtqueue available */

/* ── Virtio input config selectors (spec §5.10.4) ───────────────── */
#define VIRTIO_INPUT_CFG_UNSET      0x00
#define VIRTIO_INPUT_CFG_ID_NAME    0x01
#define VIRTIO_INPUT_CFG_ID_SERIAL  0x02
#define VIRTIO_INPUT_CFG_ID_DEVIDS  0x03
#define VIRTIO_INPUT_CFG_PROP_BITS  0x10
#define VIRTIO_INPUT_CFG_EV_BITS    0x11
#define VIRTIO_INPUT_CFG_ABS_INFO   0x12

/* ── Absolute axis information (virtio-input spec §5.10.4) ──────── */
#pragma pack(push, 1)
struct virtio_input_absinfo {
	uint32_t min;
	uint32_t max;
	uint32_t fuzz;
	uint32_t flat;
	uint32_t res;
};
#pragma pack(pop)

/* ── Device IDs (virtio-input spec §5.10.4) ──────────────────────── */
#pragma pack(push, 1)
struct virtio_input_devids {
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
};
#pragma pack(pop)

/* ── Virtio input configuration structure (spec §5.10.4) ──────────── */
#pragma pack(push, 1)
struct virtio_input_config {
	uint8_t  select;
	uint8_t  subsel;
	uint8_t  size;
	uint8_t  reserved[5];
	union {
		char    string[128];
		uint8_t bits[128];
		struct virtio_input_absinfo abs;
		struct virtio_input_devids ids;
	} u;
};
#pragma pack(pop)

/* ── Virtio input event structure (spec §5.10.5) ──────────────────── */
#pragma pack(push, 1)
struct virtio_input_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
};
#pragma pack(pop)

/* ── Multi-touch ABS codes (ABS_MT_*) ──────────────────────────────── */
#define ABS_MT_SLOT          0x2f  /* MT slot being updated */
#define ABS_MT_TOUCH_MAJOR   0x30  /* Major axis of touching ellipse */
#define ABS_MT_TOUCH_MINOR   0x31  /* Minor axis of touching ellipse */
#define ABS_MT_WIDTH_MAJOR   0x32  /* Major axis of approaching ellipse */
#define ABS_MT_WIDTH_MINOR   0x33  /* Minor axis of approaching ellipse */
#define ABS_MT_ORIENTATION   0x34  /* Ellipse orientation */
#define ABS_MT_POSITION_X    0x35  /* Center X of touching ellipse */
#define ABS_MT_POSITION_Y    0x36  /* Center Y of touching ellipse */
#define ABS_MT_TOOL_TYPE     0x37  /* Type of approaching tool */
#define ABS_MT_BLOB_ID       0x38  /* Group a set of contacts as a blob */
#define ABS_MT_TRACKING_ID   0x39  /* Unique ID of initiated contact */
#define ABS_MT_PRESSURE      0x3a  /* Pressure on the contact area */
#define ABS_MT_DISTANCE      0x3b  /* Distance from touch surface */
#define ABS_MT_TOOL_X        0x3c  /* Center X for the approaching tool */
#define ABS_MT_TOOL_Y        0x3d  /* Center Y for the approaching tool */

#endif /* VIRTIO_H */
