/*
 * src/drivers/vmw_pvscsi.c — VMware PVSCSI adapter driver
 *
 * Implements the VMware PVSCSI (paravirtual SCSI) adapter.
 * Uses PCI vendor 0x15AD, device 0x07C0 (VMware PVSCSI).
 * Provides ring-based command submission and interrupt handling.
 * Follows existing PCI probe patterns.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "pmm.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── PCI IDs ───────────────────────────────────────────────────── */

#define VMWARE_VENDOR           0x15AD
#define PVSCSI_DEVICE           0x07C0   /* VMware PVSCSI */
#define PVSCSI_DEVICE_LEGACY    0x0790   /* legacy */

/* ── Register offsets (MMIO BAR0) ──────────────────────────────── */

#define PVSCSI_REG_OFFSET_COMMAND       0x000
#define PVSCSI_REG_OFFSET_INTR_STATUS   0x004
#define PVSCSI_REG_OFFSET_INTR_MASK     0x008
#define PVSCSI_REG_OFFSET_VERSION       0x00C
#define PVSCSI_REG_OFFSET_REQ_RING      0x010
#define PVSCSI_REG_OFFSET_REQ_RING_SIZE 0x014
#define PVSCSI_REG_OFFSET_CMP_RING      0x018
#define PVSCSI_REG_OFFSET_CMP_RING_SIZE 0x01C
#define PVSCSI_REG_OFFSET_MSG_RING      0x020
#define PVSCSI_REG_OFFSET_MSG_RING_SIZE 0x024

/* ── Commands ──────────────────────────────────────────────────── */

#define PVSCSI_CMD_SETUP_RINGS          0x01
#define PVSCSI_CMD_RESET                0x02
#define PVSCSI_CMD_ABORT_CMD            0x03
#define PVSCSI_CMD_CONFIG               0x04

/* ── Ring sizes ────────────────────────────────────────────────── */

#define PVSCSI_REQ_RING_SIZE    32
#define PVSCSI_CMP_RING_SIZE    32
#define PVSCSI_MSG_RING_SIZE    8

/* ── Ring entry structures ─────────────────────────────────────── */

#pragma pack(push, 1)
struct pvscsi_req_desc {
    uint64_t context;
    uint64_t data_len;
    uint64_t data_addr;
    uint8_t  cdb[16];
    uint8_t  cdb_len;
    uint8_t  tag;
    uint8_t  lun[8];
    uint8_t  attribute;
    uint8_t  prio;
    uint16_t reserved;
};

struct pvscsi_cmp_desc {
    uint64_t context;
    uint64_t data_len;
    uint8_t  scsi_status;
    uint8_t  host_status;
    uint8_t  device_status;
    uint8_t  sense_len;
    uint8_t  sense[96];
    uint32_t reserved;
};

struct pvscsi_msg_desc {
    uint32_t type;
    uint32_t flags;
    uint64_t data;
};
#pragma pack(pop)

/* ── Driver state ──────────────────────────────────────────────── */

static int            pvscsi_present = 0;
static void          *pvscsi_mmio    = NULL;
static uint64_t       pvscsi_mmio_phys = 0;
static uint32_t       pvscsi_version = 0;
static int            pvscsi_irq     = 0;

/* ── MMIO accessors ────────────────────────────────────────────── */

static inline uint32_t pvscsi_readl(uint32_t offset)
{
    return *(volatile uint32_t *)((uintptr_t)pvscsi_mmio + offset);
}

static inline void pvscsi_writel(uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)pvscsi_mmio + offset) = val;
}

/* ── Ring setup ────────────────────────────────────────────────── */

static int pvscsi_setup_rings(void)
{
    pvscsi_writel(PVSCSI_REG_OFFSET_REQ_RING_SIZE, PVSCSI_REQ_RING_SIZE);
    pvscsi_writel(PVSCSI_REG_OFFSET_CMP_RING_SIZE, PVSCSI_CMP_RING_SIZE);
    pvscsi_writel(PVSCSI_REG_OFFSET_MSG_RING_SIZE, PVSCSI_MSG_RING_SIZE);
    pvscsi_writel(PVSCSI_REG_OFFSET_COMMAND, PVSCSI_CMD_SETUP_RINGS);
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

void vmw_pvscsi_init(void)
{
    struct pci_device dev;

    /* Try both standard and legacy device IDs */
    if (pci_find_device(VMWARE_VENDOR, PVSCSI_DEVICE, &dev) < 0 &&
        pci_find_device(VMWARE_VENDOR, PVSCSI_DEVICE_LEGACY, &dev) < 0)
        return;

    pvscsi_mmio_phys = dev.bar[0] & ~0xF;
    pvscsi_mmio = (void *)(uint64_t)pvscsi_mmio_phys;

    pci_enable_bus_master(&dev);

    pvscsi_version = pvscsi_readl(PVSCSI_REG_OFFSET_VERSION);
    pvscsi_irq = dev.irq;

    pvscsi_setup_rings();

    pvscsi_present = 1;

    kprintf("[vmw-pvscsi] VMware PVSCSI at %02x:%02x.%d, "
            "MMIO 0x%llx, version=%u, IRQ=%d\n",
            dev.bus, dev.slot, dev.func,
            (unsigned long long)pvscsi_mmio_phys,
            pvscsi_version, pvscsi_irq);
}

#ifdef MODULE
int init_module(void) { vmw_pvscsi_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VMware PVSCSI adapter — ring, interrupt");
MODULE_VERSION("1.0");
#endif

/* ── Stub: vmw_pvscsi_queue_cmd ─────────────────────────────── */
int vmw_pvscsi_queue_cmd(void *dev, void *cmd)
{
    (void)dev;
    (void)cmd;
    kprintf("[vmw] vmw_pvscsi_queue_cmd: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: vmw_pvscsi_complete_cmd ─────────────────────────────── */
int vmw_pvscsi_complete_cmd(void *dev)
{
    (void)dev;
    kprintf("[vmw] vmw_pvscsi_complete_cmd: not yet implemented\n");
    return -ENOSYS;
}
