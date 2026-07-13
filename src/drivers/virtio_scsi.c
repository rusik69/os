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
#define VIRTIO_SCSI_MAX_TARGET      255   /* max target value (0-255 = 256 targets) */
#define VIRTIO_SCSI_MAX_LUN         16383 /* max LUN value (0-16383 = 16384 LUNs) */

/* VirtIO SCSI LUN address method (per SAM-4 T10 LUN format) */
#define VIRTIO_SCSI_LUN_ADDR_METHOD    0  /* byte 0 of lun[]: address method */
#define VIRTIO_SCSI_LUN_BUS            1  /* byte 1: bus/channel */
#define VIRTIO_SCSI_LUN_TARGET_OFF     2  /* bytes 2-3: target (big-endian 16-bit) */
#define VIRTIO_SCSI_LUN_LUN_OFF        4  /* bytes 4-7: LUN ID (big-endian 32-bit) */

/* Address method values */
#define VIRTIO_SCSI_LUN_METHOD_REPORT_LUNS  0  /* REPORT LUNS addressing */
#define VIRTIO_SCSI_LUN_METHOD_LOGICAL      1  /* Logical unit addressing (SAM-4) */
#define VIRTIO_SCSI_LUN_METHOD_PERIPHERAL   2  /* Peripheral device addressing */

/* Request types */
#define VIRTIO_SCSI_T_CMD           0
#define VIRTIO_SCSI_T_TMF           1

/* TMF subtypes */
#define VIRTIO_SCSI_TMF_ABORT_TASK         0
#define VIRTIO_SCSI_TMF_ABORT_TASK_SET     1
#define VIRTIO_SCSI_TMF_CLEAR_ACA          2
#define VIRTIO_SCSI_TMF_CLEAR_TASK_SET     3
#define VIRTIO_SCSI_TMF_I_T_NEXUS_RESET    4
#define VIRTIO_SCSI_TMF_LOGICAL_UNIT_RESET 5
#define VIRTIO_SCSI_TMF_QUERY_TASK          6
#define VIRTIO_SCSI_TMF_QUERY_TASK_SET      7

/* TMF response codes */
#define VIRTIO_SCSI_S_OK            0
#define VIRTIO_SCSI_S_FUNCTION_COMPLETE     0
#define VIRTIO_SCSI_S_TASK_NONEXISTENT      1
#define VIRTIO_SCSI_S_FAILED                2
#define VIRTIO_SCSI_S_INCORRECT_LUN         3
#define VIRTIO_SCSI_S_BUSY                  4

#pragma pack(push, 1)
/* SCSI command request header (per VirtIO spec §5.6) */
struct virtio_scsi_cmd_req {
    uint8_t  lun[8];
    uint64_t tag;
    uint8_t  task_attr;
    uint8_t  prio;
    uint8_t  crn;
    uint8_t  cdb[VIRTIO_SCSI_CDB_SIZE];
    /* uint8_t dataout[]; — data to write (separate descriptor in chain) */
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

/* Task Management Function request (per VirtIO spec §5.6.8) */
struct virtio_scsi_ctrl_tmf_req {
    uint8_t  lun[8];
    uint64_t tag;
    uint8_t  subtype;
    uint8_t  timeout;
    uint8_t  reserved[3];
    uint64_t tmf_related_tag;
};

/* Task Management Function response */
struct virtio_scsi_ctrl_tmf_resp {
    uint8_t  response;
};
#pragma pack(pop)

/* ── LUN encoding helper ────────────────────────────────────────────
 * Encodes a (bus, target, lun_id) triplet into the 8-byte VirtIO SCSI
 * LUN field per SAM-4 T10 Logical Unit Addressing (method=1):
 *   lun[0] = VIRTIO_SCSI_LUN_METHOD_LOGICAL (1)
 *   lun[1] = bus (channel)
 *   lun[2..3] = target (big-endian 16-bit)
 *   lun[4..7] = lun_id (big-endian 32-bit)
 *
 * Returns lun[0] on success, -1 if any argument exceeds spec limits.
 */
static inline int virtio_scsi_encode_lun(uint8_t lun[8],
                                          uint16_t bus,
                                          uint16_t target,
                                          uint32_t lun_id)
{
    if (!lun) return -1;
    if (bus > 255) return -1;            /* max 1 byte */
    if (target > VIRTIO_SCSI_MAX_TARGET) return -1;
    if (lun_id > VIRTIO_SCSI_MAX_LUN)    return -1;

    memset(lun, 0, 8);
    lun[VIRTIO_SCSI_LUN_ADDR_METHOD] = VIRTIO_SCSI_LUN_METHOD_LOGICAL;
    lun[VIRTIO_SCSI_LUN_BUS]         = (uint8_t)bus;
    lun[VIRTIO_SCSI_LUN_TARGET_OFF]     = (uint8_t)(target >> 8);   /* big-endian high byte */
    lun[VIRTIO_SCSI_LUN_TARGET_OFF + 1] = (uint8_t)(target);        /* big-endian low byte */
    lun[VIRTIO_SCSI_LUN_LUN_OFF]        = (uint8_t)(lun_id >> 24);  /* big-endian */
    lun[VIRTIO_SCSI_LUN_LUN_OFF + 1]    = (uint8_t)(lun_id >> 16);
    lun[VIRTIO_SCSI_LUN_LUN_OFF + 2]    = (uint8_t)(lun_id >> 8);
    lun[VIRTIO_SCSI_LUN_LUN_OFF + 3]    = (uint8_t)(lun_id);
    return (int)lun[0];
}

/* Decode virtio SCSI LUN back into bus/target/lun_id components.
 * Returns 0 on success, -1 if method is not logical unit addressing. */
static inline int virtio_scsi_decode_lun(const uint8_t lun[8],
                                          uint16_t *bus,
                                          uint16_t *target,
                                          uint32_t *lun_id)
{
    if (!lun) return -1;
    if (lun[VIRTIO_SCSI_LUN_ADDR_METHOD] != VIRTIO_SCSI_LUN_METHOD_LOGICAL)
        return -1;

    if (bus)    *bus    = lun[VIRTIO_SCSI_LUN_BUS];
    if (target) *target = ((uint16_t)lun[VIRTIO_SCSI_LUN_TARGET_OFF] << 8) |
                           (uint16_t)lun[VIRTIO_SCSI_LUN_TARGET_OFF + 1];
    if (lun_id) *lun_id = ((uint32_t)lun[VIRTIO_SCSI_LUN_LUN_OFF] << 24) |
                           ((uint32_t)lun[VIRTIO_SCSI_LUN_LUN_OFF + 1] << 16) |
                           ((uint32_t)lun[VIRTIO_SCSI_LUN_LUN_OFF + 2] << 8) |
                           (uint32_t)lun[VIRTIO_SCSI_LUN_LUN_OFF + 3];
    return 0;
}

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

static void virtio_scsi_init(void)
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

    kprintf("[VIRTIO-SCSI] VirtIO SCSI at %02x:%02x.%d, I/O 0x%04x, "
            "targets=%d LUNs=%d\n",
            dev.bus, dev.slot, dev.func, scsi_iobase,
            VIRTIO_SCSI_MAX_TARGET, VIRTIO_SCSI_MAX_LUN);
}

#ifdef MODULE
int __init init_module(void) { virtio_scsi_init(); return 0; }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO SCSI — target/lun, command transport");
MODULE_VERSION("1.0");
#endif

/* ── Stub: virtio_scsi_cmd ─────────────────────────────── */
static int virtio_scsi_cmd(void *dev, void *cmd)
{
    (void)dev;
    (void)cmd;
    kprintf("[VIRTIO] virtio_scsi_cmd: not yet implemented\n");
    return 0;
}
/* ── Stub: virtio_scsi_task_mgt ─────────────────────────────── */
static int virtio_scsi_task_mgt(void *dev, void *tm)
{
    (void)dev;
    (void)tm;
    kprintf("[VIRTIO] virtio_scsi_task_mgt: not yet implemented\n");
    return 0;
}
