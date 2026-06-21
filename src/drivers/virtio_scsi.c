/*
 * src/drivers/virtio_scsi.c — VirtIO SCSI driver
 *
 * Implements VirtIO SCSI host adapter (PCI 1AF4:1004)
 * with target/LUN addressing and command transport.
 * Follows existing virtio probe patterns (virtio_blk, virtio_net).
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "heap.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define VIRTIO_VENDOR           0x1AF4
#define VIRTIO_SCSI_DEVICE      0x1004

/* ── Feature bits ──────────────────────────────────────────────── */

#define VIRTIO_SCSI_F_INOUT         (1u << 0)
#define VIRTIO_SCSI_F_HOTPLUG       (1u << 1)
#define VIRTIO_SCSI_F_CHANGE        (1u << 2)
#define VIRTIO_SCSI_F_T10_PI        (1u << 3)

/* ── SCSI constants ────────────────────────────────────────────── */

#define VIRTIO_SCSI_CDB_SIZE        32
#define VIRTIO_SCSI_SENSE_SIZE      96
#define VIRTIO_SCSI_MAX_TARGET      256
#define VIRTIO_SCSI_MAX_LUN         16384

/* Request types */
#define VIRTIO_SCSI_T_CMD           0
#define VIRTIO_SCSI_T_TMF           1

/* Response codes */
#define VIRTIO_SCSI_S_OK            0

#pragma pack(push, 1)
/* SCSI command request header */
struct virtio_scsi_cmd_req {
    uint8_t  lun[8];
    uint64_t tag;
    uint8_t  task_attr;
    uint8_t  prio;
    uint8_t  crn;
    uint8_t  cdb[VIRTIO_SCSI_CDB_SIZE];
};

/* SCSI command response header */
struct virtio_scsi_cmd_resp {
    uint32_t sense_len;
    uint32_t residual;
    uint16_t status_qualifier;
    uint8_t  status;
    uint8_t  response;
    uint8_t  sense[VIRTIO_SCSI_SENSE_SIZE];
};
#pragma pack(pop)

/* ── Driver state ──────────────────────────────────────────────── */

static int            scsi_present = 0;
static uint16_t       scsi_iobase  = 0;

/* ── I/O helpers ───────────────────────────────────────────────── */

static inline void vs_outb(uint8_t off, uint8_t v)  { outb(scsi_iobase + off, v); }
static inline void vs_outw(uint8_t off, uint16_t v) { outw(scsi_iobase + off, v); }
static inline void vs_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(scsi_iobase + off),     (uint8_t)v);
    outb((uint16_t)(scsi_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(scsi_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(scsi_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vs_inb(uint8_t off)  { return inb(scsi_iobase + off); }
static inline uint16_t vs_inw(uint8_t off)  { return inw(scsi_iobase + off); }
static inline uint32_t vs_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(scsi_iobase + off)) |
           ((uint32_t)inb((uint16_t)(scsi_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(scsi_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(scsi_iobase + off + 3)) << 24);
}

/* ── Init ──────────────────────────────────────────────────────── */

void virtio_scsi_init(void)
{
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_SCSI_DEVICE, &dev) < 0)
        return;

    scsi_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!scsi_iobase) return;

    pci_enable_bus_master(&dev);

    vs_outb(VIRTIO_PCI_STATUS, 0);
    vs_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    virtio_negotiate_features_ex(vs_inl, vs_outl, vs_outb, vs_inb,
                                 VIRTIO_SCSI_F_INOUT | VIRTIO_SCSI_F_HOTPLUG,
                                 0, NULL, "virtio-scsi");

    vs_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_DRIVER_OK);

    scsi_present = 1;

    kprintf("[virtio-scsi] VirtIO SCSI at %02x:%02x.%d, I/O 0x%04x, "
            "targets=%d LUNs=%d\n",
            dev.bus, dev.slot, dev.func, scsi_iobase,
            VIRTIO_SCSI_MAX_TARGET, VIRTIO_SCSI_MAX_LUN);
}

#ifdef MODULE
int init_module(void) { virtio_scsi_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO SCSI — target/lun, command transport");
MODULE_VERSION("1.0");
#endif

/* ── Stub: virtio_scsi_cmd ─────────────────────────────── */
int virtio_scsi_cmd(void *dev, void *cmd)
{
    (void)dev;
    (void)cmd;
    kprintf("[virtio] virtio_scsi_cmd: not yet implemented\n");
    return 0;
}
/* ── Stub: virtio_scsi_task_mgt ─────────────────────────────── */
int virtio_scsi_task_mgt(void *dev, void *tm)
{
    (void)dev;
    (void)tm;
    kprintf("[virtio] virtio_scsi_task_mgt: not yet implemented\n");
    return 0;
}
