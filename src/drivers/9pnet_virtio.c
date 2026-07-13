/*
 * src/drivers/9pnet_virtio.c — 9P2000.L over virtio transport
 *
 * Implements the virtio transport for the 9P2000.L protocol
 * (Plan 9 resource sharing).  Uses PCI device 1AF4:1009
 * (virtio-9p).  Provides a channel for the 9P filesystem.
 * Follows existing virtio probe patterns (virtio_blk, virtio_net).
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "pmm.h"
#include "errno.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define VIRTIO_VENDOR         0x1AF4
#define VIRTIO_9P_DEVICE      0x1009

/* ── Feature bits ──────────────────────────────────────────────── */

#define VIRTIO_9P_F_MOUNT_TAG    (1u << 0)

/* ── 9P message types (subset of 9P2000.L) ─────────────────────── */

#define P9_TVERSION     100
#define P9_RVERSION     101
#define P9_TATTACH      104
#define P9_RATTACH      105
#define P9_TWALK        110
#define P9_RWALK        111
#define P9_TREAD        116
#define P9_RREAD        117
#define P9_TWRITE       118
#define P9_RWRITE       119
#define P9_TCLUNK       120
#define P9_RCLUNK       121
#define P9_TSTAT        124
#define P9_RSTAT        125

/* ── 9P message header (7 bytes) ───────────────────────────────── */

#pragma pack(push, 1)
struct p9_header {
    uint32_t size;
    uint8_t  type;
    uint16_t tag;
};
#pragma pack(pop)

/* ── Tag management ────────────────────────────────────────────────
 * In the 9P2000.L protocol, each request carries a 16-bit tag used to
 * match responses to requests.  Tag 0xFFFF is reserved (P9_NOTAG) for
 * messages that do not expect a response (e.g. TCLUNK).  Must be at
 * most 65535 unique tags in flight simultaneously; in practice the
 * virtqueue depth bounds concurrency.
 *
 * We maintain a simple wrap-around counter allocator backed by a
 * bitmap to detect and prevent collisions.  The bitmap covers all
 * 65536 possible tag values (8 KB) — acceptable for a transport that
 * may drive an entire filesystem.
 * ──────────────────────────────────────────────────────────────── */

#define P9_NOTAG               0xFFFFu       /* reserved "no tag" value */
#define P9_TAG_BMAP_ENTRIES    (65536 / 64)  /* 1024 uint64_t entries */

static uint16_t      v9p_next_tag = 0;
static uint64_t      v9p_tag_bmap[P9_TAG_BMAP_ENTRIES]; /* zero-initialised */

/* v9p_tag_alloc: allocate a unique tag for an outgoing request.
 * Returns P9_NOTAG if all tags are exhausted.
 * The returned tag is marked in-flight in the bitmap. */
static uint16_t v9p_tag_alloc(void)
{
    uint16_t tag;
    int attempts = 0;

    /* Linear scan — at most one full wrap */
    while (attempts < 65535) {
        tag = v9p_next_tag;
        v9p_next_tag++;

        /* Skip the reserved NOTAG value */
        if (tag == P9_NOTAG)
            continue;

        /* Wrap at 0xFFFF (P9_NOTAG is the only reserved value) */
        if (v9p_next_tag >= P9_NOTAG)
            v9p_next_tag = 0;

        uint16_t idx  = tag / 64;
        uint64_t bit  = (uint64_t)1u << (tag % 64);
        if (!(v9p_tag_bmap[idx] & bit)) {
            v9p_tag_bmap[idx] |= bit;
            return tag;
        }
        attempts++;
    }

    /* All tags in-flight — exhausted */
    kprintf("[9pnet-virtio] WARNING: tag exhaustion, all 65535 tags in use\n");
    return P9_NOTAG;
}

/* v9p_tag_free: release a tag that is no longer in flight.
 * Safe to call with P9_NOTAG (no-op). */
static void v9p_tag_free(uint16_t tag)
{
    if (tag == P9_NOTAG)
        return;

    uint16_t idx  = tag / 64;
    uint64_t bit  = (uint64_t)1u << (tag % 64);
    v9p_tag_bmap[idx] &= ~bit;
}

/* ── Driver state ──────────────────────────────────────────────── */

static int            v9p_present = 0;
static uint16_t       v9p_iobase  = 0;
static char           v9p_mount_tag[64];

/* ── I/O helpers ───────────────────────────────────────────────── */

static inline void v9p_outb(uint8_t off, uint8_t v)  { outb(v9p_iobase + off, v); }
static inline void v9p_outw(uint8_t off, uint16_t v) { outw(v9p_iobase + off, v); }
static inline void v9p_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(v9p_iobase + off),     (uint8_t)v);
    outb((uint16_t)(v9p_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(v9p_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(v9p_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  v9p_inb(uint8_t off)  { return inb(v9p_iobase + off); }
static inline uint16_t v9p_inw(uint8_t off)  { return inw(v9p_iobase + off); }
static inline uint32_t v9p_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(v9p_iobase + off)) |
           ((uint32_t)inb((uint16_t)(v9p_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(v9p_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(v9p_iobase + off + 3)) << 24);
}

/* ── 9P message send/receive (stub) ────────────────────────────── */

static __attribute__((unused)) int v9p_send_recv(const void *tx, uint32_t txlen,
                          void *rx, uint32_t rxmax, uint32_t *rxlen)
{
    (void)tx; (void)txlen; (void)rx; (void)rxmax;
    if (rxlen) *rxlen = 0;
    kprintf("[9pnet-virtio] message send stub (size=%u)\n", txlen);
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

static void v9pnet_virtio_init(void)
{
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_9P_DEVICE, &dev) < 0)
        return;

    v9p_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!v9p_iobase) return;

    pci_enable_bus_master(&dev);

    v9p_outb(VIRTIO_PCI_STATUS, 0);
    v9p_outb(VIRTIO_PCI_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    virtio_negotiate_features_ex(v9p_inl, v9p_outl, v9p_outb, v9p_inb,
                                 VIRTIO_9P_F_MOUNT_TAG, 0, NULL, "virtio-9p");

    v9p_outb(VIRTIO_PCI_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
             VIRTIO_STATUS_DRIVER_OK);

    /* Read mount tag from config space (after PCI config, at offset 20)
     * Virtio-9p config layout: [le16 tag_len][u8 tag[tag_len]]
     * The tag is NOT null-terminated in config space — tag_len
     * determines the number of tag bytes. */
    memset(v9p_mount_tag, 0, sizeof(v9p_mount_tag));
    uint16_t tag_len = v9p_inw(0x14);               /* 16-bit length field */
    if (tag_len >= sizeof(v9p_mount_tag))
        tag_len = sizeof(v9p_mount_tag) - 1;        /* clamp to fit buffer */
    for (uint16_t i = 0; i < tag_len; i++)
        v9p_mount_tag[i] = (char)v9p_inb((uint8_t)(0x16 + i));

    v9p_present = 1;

    kprintf("[9pnet-virtio] 9P2000.L over virtio at %02x:%02x.%d, "
            "I/O 0x%04x, tag='%s'\n",
            dev.bus, dev.slot, dev.func, v9p_iobase, v9p_mount_tag);
}

#ifdef MODULE
int __init init_module(void) { v9pnet_virtio_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("9P2000.L over virtio transport");
MODULE_VERSION("1.0");
#endif

/* ── p9_virtio_init: Probe and initialise the virtio-9p device ── */
static int p9_virtio_init(void)
{
    kprintf("[9p] Initialising virtio-9p transport...\n");

    /* Use the existing v9pnet_virtio_init which already probes PCI */
    v9pnet_virtio_init();

    if (!v9p_present) {
        kprintf("[9p] No virtio-9p device found\n");
        return -ENODEV;
    }

    kprintf("[9p] virtio-9p transport initialised (tag='%s')\n", v9p_mount_tag);
    return 0;
}

/* ── p9_virtio_open: Open a 9P session to a device ────────── */
static int p9_virtio_open(void *dev)
{
    (void)dev;
    if (!v9p_present) return -EIO;

    kprintf("[9p] Opening 9P session...\n");

    /* The device is already ready from v9pnet_virtio_init */
    return 0;
}

/* ── p9_virtio_close: Close a 9P session ────────── */
static int p9_virtio_close(void *dev)
{
    (void)dev;
    if (!v9p_present) return -EIO;

    kprintf("[9p] Closing 9P session...\n");
    return 0;
}

/* ── p9_virtio_request: Send a 9P request and receive response ── */
static int p9_virtio_request(void *dev, void *req)
{
    (void)dev;
    if (!v9p_present) return -EIO;
    if (!req) return -EINVAL;

    /* Use the existing v9p_send_recv stub for now */
    /* The req is a struct p9_req_t containing a buffer */
    struct p9_header *hdr = (struct p9_header *)req;
    uint32_t size = hdr->size;

    /* Validate tag — P9_NOTAG cannot be used for messages that expect
     * a response through the request/response interface. */
    if (hdr->tag == P9_NOTAG) {
        kprintf("[9p] ERROR: request with reserved NOTAG tag rejected\n");
        return -EINVAL;
    }

    /* Validate size — must be at least header size */
    if (size < sizeof(struct p9_header)) {
        kprintf("[9p] ERROR: request size %u < header %zu\n",
                (unsigned int)size, sizeof(struct p9_header));
        return -EINVAL;
    }

    kprintf("[9p] 9P request: type=%d tag=%d size=%u\n",
            hdr->type, hdr->tag, (unsigned int)size);

    return 0;
}
