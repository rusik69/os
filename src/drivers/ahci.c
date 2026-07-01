/*
 * AHCI (Advanced Host Controller Interface) driver with NCQ and Port Multiplier support
 *
 * Supports SATA devices with multi-slot interrupt-driven I/O.
 * Handles Port Multiplier (PM) devices behind a single physical port.
 *
 * Uses Native Command Queuing (NCQ) for up to 31 concurrent commands.
 * Slot 0 is reserved for non-NCQ commands (IDENTIFY, etc.).
 * Slots 1-31 are used for NCQ READ/WRITE FPDMA QUEUED.
 *
 * Port Multiplier architecture:
 *   Physical ports with PORT_CMD.PMP=1 have a PM attached.
 *   Up to 15 devices (PM Ports 0-14) can connect via the PM.
 *   Each PM device registers as a separate block device but shares
 *   the physical port's command list, slots, and PRDT entries.
 *   The PM Port number is encoded in the FIS pmport_c field.
 */
#include "ahci.h"
#include "blockdev.h"
#include "pci.h"
#include "pmm.h"
#include "heap.h"
#include "vmm.h"
#include "string.h"
#include "err.h"
#include "printf.h"
#include "io.h"
#include "idt.h"
#include "apic.h"
#include "pic.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── HBA register offsets ───────────────────────────────────────────── */
#define HBA_CAP_OFFSET   0x00   /* Host Capabilities */
#define HBA_GHC_OFFSET   0x04   /* Global Host Control */
#define HBA_IS_OFFSET    0x08   /* Interrupt Status */
#define HBA_PI_OFFSET    0x0C   /* Ports Implemented */
#define HBA_VS_OFFSET    0x10   /* Version */
#define HBA_CAP2_OFFSET  0x2C   /* Host Capabilities Extended */
#define HBA_PORT_BASE    0x100  /* First port register block */
#define HBA_PORT_SIZE    0x80   /* Per-port register stride */

/* CAP2 bits */
#define CAP2_SNCQ       (1u << 1)   /* Supports NCQ (controller-wide) */

/* Port register offsets */
#define PORT_CLB    0x00   /* Command List Base Address (low) */
#define PORT_CLBU   0x04   /* Command List Base Address (high) */
#define PORT_FB     0x08   /* FIS Base Address (low) */
#define PORT_FBU    0x0C   /* FIS Base Address (high) */
#define PORT_IS     0x10   /* Interrupt Status */
#define PORT_IE     0x14   /* Interrupt Enable */
#define PORT_CMD    0x18   /* Command and Status */
#define PORT_TFD    0x20   /* Task File Data */
#define PORT_SIG    0x24   /* Signature */
#define PORT_SSTS   0x28   /* SATA Status */
#define PORT_SCTL   0x2C   /* SATA Control */
#define PORT_SERR   0x30   /* SATA Error */
#define PORT_SACT   0x34   /* SATA Active (NCQ) */
#define PORT_CI     0x38   /* Command Issue */

/* GHC bits */
#define GHC_AE      (1u << 31)  /* AHCI Enable */
#define GHC_IE      (1u << 1)   /* Interrupt Enable */

/* PORT_CMD bits */
#define PORT_CMD_ST     (1u << 0)   /* Start */
#define PORT_CMD_FRE    (1u << 4)   /* FIS Receive Enable */
#define PORT_CMD_FR     (1u << 14)  /* FIS Receive Running */
#define PORT_CMD_CR     (1u << 15)  /* Command List Running */
#define PORT_CMD_PMP    (1u << 17)  /* Port Multiplier Attached */

/* PORT_IS bits */
#define PORT_IS_D2H     (1u << 0)   /* D2H Register FIS */
#define PORT_IS_SDBS    (1u << 2)   /* Set Device Bits (NCQ completion) */
#define PORT_IS_PCS     (1u << 6)   /* PhyRdy Change */
#define PORT_IS_ERR     (1u << 7)   /* Error (also used for non-NCQ completions) */
#define PORT_IS_PR_MASK (0x1F)      /* Polling required bits */

/* PORT_TFD bits */
#define PORT_TFD_BSY    (1u << 7)
#define PORT_TFD_DRQ    (1u << 3)
#define PORT_TFD_ERR    (1u << 0)

/* SSTS detection */
#define SSTS_DET_MASK    0x0F
#define SSTS_DET_PRESENT 0x03

/* FIS types */
#define FIS_TYPE_REG_H2D  0x27
#define FIS_TYPE_SETDEV  0xA1  /* Set Device Bits (device to host) */

/* ATA commands */
#define ATA_CMD_READ_DMA_EX          0x25
#define ATA_CMD_WRITE_DMA_EX         0x35
#define ATA_CMD_READ_FPDMA_QUEUED    0x60
#define ATA_CMD_WRITE_FPDMA_QUEUED   0x61
#define ATA_CMD_IDENTIFY             0xEC
#define ATA_CMD_DATA_SET_MGMT        0x06  /* DATA SET MANAGEMENT (DSM) */
#define ATA_DSM_TRIM                 0x01  /* TRIM bit in feature register */

/* ── Constants ──────────────────────────────────────────────────────── */
#define AHCI_SLOT_COUNT    32
#define AHCI_NCQ_SLOTS     (AHCI_SLOT_COUNT - 1)  /* 31 NCQ slots */
#define AHCI_PRDT_ENTRIES  1
#define AHCI_DATA_FRAME_SECTORS 8  /* 4KB data buffer = 8 sectors */
#define AHCI_MAX_PHYS_PORTS     32

/* ── Data structures ────────────────────────────────────────────────── */

/* Register FIS – Host to Device (20 bytes) */
struct fis_reg_h2d {
    uint8_t  fis_type;
    uint8_t  pmport_c;     /* bits 3:0 = PM Port, bit 7 = C (update) */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;
    uint8_t  countl, counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv[4];
} __attribute__((packed));

/* Command Header (32 bytes) */
struct ahci_cmd_hdr {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} __attribute__((packed));

/* PRDT Entry (16 bytes) */
struct ahci_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc;
} __attribute__((packed));

/* Command Table */
struct ahci_cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    struct ahci_prdt_entry prdt[AHCI_PRDT_ENTRIES];
} __attribute__((packed));

/* Received FIS area */
struct ahci_recv_fis {
    uint8_t dsfis[28];
    uint8_t pad0[4];
    uint8_t psfis[20];
    uint8_t pad1[12];
    uint8_t rfis[20];
    uint8_t pad2[4];
    uint8_t sdbfis[8];
    uint8_t ufis[64];
    uint8_t rsv[96];
} __attribute__((packed));

/* Per-slot state */
struct ahci_slot {
    struct blk_request *req;          /* current request, NULL if free */
    uint64_t            cmd_tbl_phys; /* physical address of command table */
    void               *cmd_tbl_virt; /* kernel virtual address of command table */
    uint64_t            data_buf_phys;/* physical address of data buffer (4KB) */
    void               *data_buf_virt;/* kernel virtual address of data buffer */
};

/* Per-port state — represents one addressable device (direct-attached or PM sub-port) */
struct ahci_port {
    int                 present;
    int                 port_num;          /* physical port index (0-31) */
    int                 pm_port;           /* PM Port (-1 = direct, 0-14 = via PM) */
    int                 is_pm;             /* 1 if this is a PM sub-port entry */
    uint32_t            sector_count;
    int                 ncq_capable;       /* word 76 bit 8 */
    int                 blockdev_id;
    uint8_t             irq_line;
    struct ahci_slot    slots[AHCI_SLOT_COUNT];
    uint64_t            cmd_list_phys;
    uint64_t            recv_fis_phys;
    uint32_t            inflight_mask;     /* bits set for issued slots */
    uint32_t            tag_bitmap;        /* O(1) tag alloc: 0=free, 1=allocated */
    int                 ncq_priority;      /* N:0=simple, 1=deterministic, 2=high */
};

/* ── Driver state ───────────────────────────────────────────────────── */
static int              ahci_present      = 0;
static uint64_t         hba_base          = 0;
static struct ahci_port ahci_ports[AHCI_MAX_PHYS_PORTS * (1 + AHCI_MAX_PM_PORTS)];
static int              ahci_port_count   = 0;
static spinlock_t       ahci_lock;

/* ── MMIO helpers ───────────────────────────────────────────────────── */
static inline uint32_t hba_read(uint32_t off) {
    return *(volatile uint32_t *)(hba_base + off);
}
static inline void hba_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(hba_base + off) = val;
}
static inline uint32_t port_read(int p, uint32_t off) {
    return hba_read(HBA_PORT_BASE + (uint32_t)p * HBA_PORT_SIZE + off);
}
static inline void port_write(int p, uint32_t off, uint32_t val) {
    hba_write(HBA_PORT_BASE + (uint32_t)p * HBA_PORT_SIZE + off, val);
}

static void busy_wait(volatile int n) { while (n-- > 0) __asm__("pause"); }

/* ── Port start/stop ────────────────────────────────────────────────── */
static void port_stop(int p) {
    uint32_t cmd = port_read(p, PORT_CMD);
    cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    port_write(p, PORT_CMD, cmd);
    int timeout = 50000;
    while ((port_read(p, PORT_CMD) & (PORT_CMD_CR | PORT_CMD_FR)) && --timeout)
        busy_wait(10);
}

static void port_start(int p) {
    int timeout = 50000;
    while ((port_read(p, PORT_CMD) & PORT_CMD_CR) && --timeout)
        busy_wait(10);
    uint32_t cmd = port_read(p, PORT_CMD);
    cmd |= PORT_CMD_FRE | PORT_CMD_ST;
    port_write(p, PORT_CMD, cmd);
}

/* ── Tag bitmap (O(1) slot allocation) ───────────────────────────────── */
/* Bit 0 = slot 0 (non-NCQ), bits 1-31 = NCQ slots.
 * per-port tag_bitmap tracks which tags are currently free. */
#define TAG_BIT_SLOT0    (1u << 0)

static inline int tag_bitmap_alloc(uint32_t *bitmap, int start, int end) {
    unsigned long bits = ~(*bitmap);
    if (bits == 0) return -1;
    /* ffs = find first set (1-indexed); subtract 1 for 0-indexed slot.
     * But we restrict to [start, end) range. */
    for (int i = start; i < end; i++) {
        if (!(*bitmap & (1u << i))) {
            *bitmap |= (1u << i);
            return i;
        }
    }
    return -1;
}

static inline void tag_bitmap_free(uint32_t *bitmap, int slot) {
    *bitmap &= ~(1u << slot);
}

static inline int tag_bitmap_is_allocated(uint32_t bitmap, int slot) {
    return (bitmap & (1u << slot)) ? 1 : 0;
}

/* ── Slot management ────────────────────────────────────────────────── */
static int ahci_find_free_slot(struct ahci_port *port) {
    /* Check CI (commands in progress) and SACT (NCQ active).
     * A free slot has bits clear in both. For NCQ we use slots 1..31. */
    uint32_t ci   = port_read(port->port_num, PORT_CI);
    uint32_t sact = port_read(port->port_num, PORT_SACT);
    uint32_t hw_busy = ci | sact;

    /* Sync the tag bitmap with hardware state: any slot that was released
     * by hardware but still marked in our bitmap gets cleared. */
    uint32_t stale = port->tag_bitmap & ~hw_busy;
    port->tag_bitmap &= ~stale;

    /* Try NCQ slots (1-31) first via tag bitmap */
    int slot = tag_bitmap_alloc(&port->tag_bitmap, 1, AHCI_SLOT_COUNT);
    if (slot >= 0) return slot;

    /* Fall back to slot 0 for non-NCQ commands */
    if (!(hw_busy & TAG_BIT_SLOT0) && !(port->tag_bitmap & TAG_BIT_SLOT0)) {
        port->tag_bitmap |= TAG_BIT_SLOT0;
        return 0;
    }
    return -1;
}

static void ahci_free_slot(struct ahci_port *port, int slot) {
    tag_bitmap_free(&port->tag_bitmap, slot);
}

/* ── Build an NCQ command ───────────────────────────────────────────── */
static void ahci_build_ncq_cmd(struct ahci_port *port, int slot,
                                struct blk_request *req) {
    struct ahci_slot *s = &port->slots[slot];
    struct ahci_cmd_hdr *hdr = (struct ahci_cmd_hdr *)
        PHYS_TO_VIRT(port->cmd_list_phys) + slot;
    struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)s->cmd_tbl_virt;
    int is_write = (req->flags & BLK_REQ_WRITE) ? 1 : 0;
    uint64_t lba = req->lba;
    uint32_t count = req->count;

    memset(hdr, 0, sizeof(*hdr));
    memset(tbl, 0, sizeof(*tbl));

    /* Copy data buffer for writes */
    if (is_write) {
        memcpy(s->data_buf_virt, req->buf, (size_t)count * AHCI_SECTOR_SIZE);
    }

    /* Command Header */
    hdr->flags = sizeof(struct fis_reg_h2d) / 4;  /* CFL in DWords */
    if (is_write) hdr->flags |= (1u << 6);         /* W bit */
    hdr->prdtl  = 1;
    hdr->ctba   = (uint32_t)(s->cmd_tbl_phys & 0xFFFFFFFF);
    hdr->ctbau  = (uint32_t)(s->cmd_tbl_phys >> 32);

    /* Command FIS — set PM Port if this is a PM device */
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    if (port->is_pm && port->pm_port >= 0) {
        /* PM Port in bits 3:0, C=1 (update) in bit 7 */
        fis->pmport_c = 0x80 | (uint8_t)(port->pm_port & 0x0F);
    } else {
        fis->pmport_c = 0x80;  /* direct-attached, C=1, PM Port=0 */
    }
    fis->command  = is_write ? ATA_CMD_WRITE_FPDMA_QUEUED
                             : ATA_CMD_READ_FPDMA_QUEUED;
    fis->device   = (1u << 6);  /* LBA mode */
    fis->lba0 = (uint8_t)(lba >>  0);
    fis->lba1 = (uint8_t)(lba >>  8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);

    /* NCQ: countl encodes TAG (bits 7:3) + count high bits (bits 2:0)
     * counth is count low 8 bits.
     * Sector count max 65535 per NCQ command, but our buffer limits to 8. */
    if (count > AHCI_DATA_FRAME_SECTORS) count = AHCI_DATA_FRAME_SECTORS;
    fis->countl = (uint8_t)((slot & 0x1F) << 3) | (uint8_t)((count >> 8) & 0x07);
    fis->counth = (uint8_t)(count & 0xFF);
    /* NCQ priority: 0=simple, 1=deterministic, 2=high */
    fis->icc = (uint8_t)(port->ncq_priority & 0x07);
    fis->control = 0;

    /* PRDT entry */
    tbl->prdt[0].dba  = (uint32_t)(s->data_buf_phys & 0xFFFFFFFF);
    tbl->prdt[0].dbau = (uint32_t)(s->data_buf_phys >> 32);
    tbl->prdt[0].rsv  = 0;
    tbl->prdt[0].dbc  = (uint32_t)count * AHCI_SECTOR_SIZE - 1;

    s->req = req;
}

/* ── Build a non-NCQ command (IDENTIFY, etc.) ───────────────────────── */
static void ahci_build_raw_cmd(struct ahci_port *port, int slot,
                                uint8_t ata_cmd, uint32_t lba, uint8_t count,
                                uint64_t data_phys, int is_write, int fis_len_dw) {
    struct ahci_cmd_hdr *hdr = (struct ahci_cmd_hdr *)
        PHYS_TO_VIRT(port->cmd_list_phys) + slot;
    struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)
        port->slots[slot].cmd_tbl_virt;

    memset(hdr, 0, sizeof(*hdr));
    memset(tbl, 0, sizeof(*tbl));

    hdr->flags  = (uint16_t)fis_len_dw;
    if (is_write) hdr->flags |= (1u << 6);
    hdr->prdtl  = 1;
    hdr->ctba   = (uint32_t)(port->slots[slot].cmd_tbl_phys & 0xFFFFFFFF);
    hdr->ctbau  = (uint32_t)(port->slots[slot].cmd_tbl_phys >> 32);

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    if (port->is_pm && port->pm_port >= 0) {
        fis->pmport_c = 0x80 | (uint8_t)(port->pm_port & 0x0F);
    } else {
        fis->pmport_c = 0x80;
    }
    fis->command  = ata_cmd;
    fis->device   = (1u << 6);
    fis->lba0 = (uint8_t)(lba >>  0);
    fis->lba1 = (uint8_t)(lba >>  8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = 0;
    fis->lba5 = 0;
    fis->countl = count;
    fis->counth = 0;

    tbl->prdt[0].dba  = (uint32_t)(data_phys & 0xFFFFFFFF);
    tbl->prdt[0].dbau = (uint32_t)(data_phys >> 32);
    tbl->prdt[0].rsv  = 0;
    tbl->prdt[0].dbc  = (uint32_t)count * AHCI_SECTOR_SIZE - 1;
}

/* ── Issue a command (set CI bit, non-NCQ) ──────────────────────────── */
static int ahci_issue_non_ncq(struct ahci_port *port, int slot) {
    int p = port->port_num;
    /* Clear errors */
    port_write(p, PORT_SERR, port_read(p, PORT_SERR));
    port_write(p, PORT_IS,   port_read(p, PORT_IS));

    port_write(p, PORT_CI, (1u << slot));

    /* Poll for completion */
    int timeout = 2000000;
    while (--timeout) {
        uint32_t ci  = port_read(p, PORT_CI);
        uint32_t tfd = port_read(p, PORT_TFD);
        if (tfd & PORT_TFD_ERR) return -1;
        if (!(ci & (1u << slot))) break;
        busy_wait(10);
    }
    if (timeout == 0) return -2;
    return 0;
}

/* ── Issue NCQ commands to the HBA ──────────────────────────────────── */
static void ahci_issue_ncq(struct ahci_port *port, int slot) {
    int p = port->port_num;
    uint32_t bit = (1u << slot);

    /* Set SActive first, then CI */
    port_write(p, PORT_SERR, port_read(p, PORT_SERR));
    port_write(p, PORT_IS,   port_read(p, PORT_IS));

    port_write(p, PORT_SACT, port_read(p, PORT_SACT) | bit);
    __asm__ volatile("mfence" ::: "memory");
    port_write(p, PORT_CI,   port_read(p, PORT_CI) | bit);
    __asm__ volatile("mfence" ::: "memory");
}

/* ── Drain request queue: submit pending requests to AHCI ───────────── */
static void ahci_drain_queue(struct ahci_port *port) {
    struct blk_request_queue *q = blockdev_get_queue(port->blockdev_id);
    if (!q) return;

    for (;;) {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&ahci_lock, &irq_flags);

        int slot = ahci_find_free_slot(port);
        if (slot < 0) {
            spinlock_irqsave_release(&ahci_lock, irq_flags);
            return; /* no free slots */
        }

        struct blk_request *req;
        if (!blk_request_dequeue(q, &req)) {
            spinlock_irqsave_release(&ahci_lock, irq_flags);
            return; /* queue empty */
        }

        /* For non-NCQ (slot 0 or device doesn't support NCQ) */
        if (slot == 0 || !port->ncq_capable) {
            if (req->flags & BLK_REQ_DISCARD) {
                /* ATA DATA SET MANAGEMENT - TRIM command (non-data NCQ not supported).
                 * Build a non-NCQ command using feature=TRIM, command=DATA_SET_MGMT.
                 * The data buffer contains LBA range entries (8 bytes each):
                 *   bits 47:0  = LBA
                 *   bits 63:48 = sector count (0 means 65536)
                 * We use slot 0 for non-NCQ TRIM. */
                struct trim_entry {
                    uint64_t range;
                } __attribute__((packed));

                /* Build the TRIM range entry: LBA in low 48 bits, count in high 16 bits */
                uint64_t lba = req->lba;
                uint32_t count = req->count;
                if (count > 65535) count = 65535; /* clamp to max per entry */
                struct trim_entry entry;
                entry.range = lba | ((uint64_t)(count == 65535 ? 0 : count) << 48);

                /* Copy the range entry to the slot's data buffer */
                memcpy(port->slots[slot].data_buf_virt, &entry, sizeof(entry));

                /* Build the command FIS with DSM opcode and TRIM feature */
                ahci_build_raw_cmd(port, slot,
                                   ATA_CMD_DATA_SET_MGMT,
                                   0, 0,  /* lba=0, count=0 (not used for DSM) */
                                   port->slots[slot].data_buf_phys,
                                   1,     /* is_write=1 (device receives data) */
                                   sizeof(struct fis_reg_h2d) / 4);

                /* Set feature register to TRIM (0x01) */
                struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)
                    port->slots[slot].cmd_tbl_virt;
                struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
                fis->featurel = ATA_DSM_TRIM;

                /* TRIM data transfer is 8 bytes (one range entry) */
                tbl->prdt[0].dbc = sizeof(struct trim_entry) - 1;

                /* Submit synchronously */
                port->slots[slot].req = req;
                spinlock_irqsave_release(&ahci_lock, irq_flags);

                int ret = ahci_issue_non_ncq(port, slot);
                req->result = ret;
                port->slots[slot].req = NULL;
                blk_request_done(req);
            } else {
                ahci_build_raw_cmd(port, slot,
                                   (req->flags & BLK_REQ_READ) ? ATA_CMD_READ_DMA_EX
                                                                : ATA_CMD_WRITE_DMA_EX,
                                   (uint32_t)req->lba, (uint8_t)req->count,
                                   port->slots[slot].data_buf_phys,
                                   (req->flags & BLK_REQ_WRITE) ? 1 : 0,
                                   sizeof(struct fis_reg_h2d) / 4);
                /* Copy data to DMA buffer for writes */
                if (req->flags & BLK_REQ_WRITE) {
                    memcpy(port->slots[slot].data_buf_virt, req->buf,
                           (size_t)req->count * AHCI_SECTOR_SIZE);
                }

                /* Submit synchronous for slot 0 (non-NCQ) */
                port->slots[slot].req = req;
                spinlock_irqsave_release(&ahci_lock, irq_flags);

                /* Signal activity LED for this port */
                ahci_led_activity(port->port_num);

                /* Non-NCQ: issue synchronously (will poll briefly), then complete */
                int ret = ahci_issue_non_ncq(port, slot);

                /* Copy data back for reads */
                if (ret == 0 && (req->flags & BLK_REQ_READ)) {
                    memcpy(req->buf, port->slots[slot].data_buf_virt,
                           (size_t)req->count * AHCI_SECTOR_SIZE);
                }

                req->result = ret;
                port->slots[slot].req = NULL;
                blk_request_done(req);
            }
        } else {
            /* NCQ: build and issue asynchronously */
            ahci_build_ncq_cmd(port, slot, req);
            port->inflight_mask |= (1u << slot);
            spinlock_irqsave_release(&ahci_lock, irq_flags);

            /* Signal activity LED for this port */
            ahci_led_activity(port->port_num);

            ahci_issue_ncq(port, slot);
        }
    }
}

/* ── Interrupt handler ──────────────────────────────────────────────── */
static void ahci_irq_handler(struct interrupt_frame *frame) {
    (void)frame;

    uint32_t is = hba_read(HBA_IS_OFFSET);
    if (!is) return;

    /* Protect slot management in IRQ context.
     * This is a top-half handler (registered via idt_register_handler),
     * so we must use the IRQ-safe spinlock variant. */
    uint64_t __ahci_flags;
    spinlock_irqsave_acquire(&ahci_lock, &__ahci_flags);

    for (int p = 0; p < 32; p++) {
        if (!(is & (1u << p))) continue;

        uint32_t port_is = port_read(p, PORT_IS);
        uint32_t tfd = port_read(p, PORT_TFD);

        /* Clear interrupt status */
        port_write(p, PORT_IS, port_is);
        hba_write(HBA_IS_OFFSET, (1u << p));

        if (port_is & PORT_IS_ERR) {
            /* Error interrupt — read SERR, then attempt NCQ error recovery.
             * The error recovery will abort all pending NCQ commands on this
             * port and issue a soft reset. */
            uint32_t serr = port_read(p, PORT_SERR);
            port_write(p, PORT_SERR, serr);
            kprintf("AHCI port %d error: IS=0x%x TFD=0x%x SERR=0x%x\n",
                    p, port_is, tfd, serr);

            /* Release lock before error recovery (it sleeps/waits) */
            spinlock_irqsave_release(&ahci_lock, __ahci_flags);
            ahci_ncq_recover_port(p);
            spinlock_irqsave_acquire(&ahci_lock, &__ahci_flags);
        }

        if (port_is & PORT_IS_SDBS) {
            /* NCQ completion: check which slots completed.
             * We iterate all port entries (direct + PM sub-ports) and
             * let each one handle its own inflight slots. */
            uint32_t ci   = port_read(p, PORT_CI);
            uint32_t sact = port_read(p, PORT_SACT);
            uint32_t all_done = ~(ci | sact);  /* completed slots */

            for (int i = 0; i < ahci_port_count; i++) {
                struct ahci_port *port = &ahci_ports[i];
                if (!port->present || port->port_num != p)
                    continue;

                uint32_t done = port->inflight_mask & all_done;
                if (!done) continue;

                for (int slot = 1; slot < AHCI_SLOT_COUNT; slot++) {
                    if (done & (1u << slot)) {
                        struct ahci_slot *s = &port->slots[slot];
                        struct blk_request *req = s->req;
                        if (req) {
                            s->req = NULL;
                            port->inflight_mask &= ~(1u << slot);
                            ahci_free_slot(port, slot);

                            /* Signal activity LED */
                            ahci_led_activity(port->port_num);

                            /* Check for errors */
                            if (tfd & PORT_TFD_ERR) {
                                req->result = -1;
                            } else {
                                req->result = 0;
                                /* Copy data back for reads */
                                if (req->flags & BLK_REQ_READ) {
                                    memcpy(req->buf, s->data_buf_virt,
                                           (size_t)req->count * AHCI_SECTOR_SIZE);
                                }
                            }
                            blk_request_done(req);
                        }
                    }
                }
            }
        }

        if (port_is & PORT_IS_D2H) {
            /* Non-NCQ D2H completion — handled inline in ahci_issue_non_ncq */
        }
    }

    /* Release lock before draining — ahci_drain_queue acquires the
     * lock internally and calling it while already holding it would
     * cause a self-deadlock. */
    spinlock_irqsave_release(&ahci_lock, __ahci_flags);

    /* Drain all PM sub-ports associated with each physical port.
     * ahci_drain_queue handles its own IRQ-safe locking. */
    for (int p = 0; p < 32; p++) {
        if (!(is & (1u << p))) continue;
        for (int i = 0; i < ahci_port_count; i++) {
            if (ahci_ports[i].present && ahci_ports[i].port_num == p) {
                ahci_drain_queue(&ahci_ports[i]);
            }
        }
    }

    /* Ack IRQ to IOAPIC/PIC */
    if (ahci_port_count > 0)
        irq_ack(ahci_ports[0].irq_line);
}

/* ── submit_fn called from block I/O layer (async) ──────────────────── */
static int ahci_submit_fn(struct blk_request *req) {
    (void)req;
    /* For async NCQ driver, submission is handled by ahci_drain_queue().
     * This should never be called directly via the sync path because
     * we register with BLK_DRIVER_ASYNC. */
    return -1;
}

/* ── idle_fn called by block I/O layer ──────────────────────────────── */
static int ahci_idle_fn(struct blk_request_queue *q, int force_flush) {
    (void)force_flush;
    /* Find our port from the queue's dev_id */
    for (int i = 0; i < ahci_port_count; i++) {
        if (ahci_ports[i].blockdev_id == q->dev_id) {
            ahci_drain_queue(&ahci_ports[i]);
            return 0;
        }
    }
    return -1;
}

/* ── AHCI NCQ API ───────────────────────────────────────────────────── */

/* CAP2.SNCQ — check if controller supports NCQ at all */
int ahci_has_ncq_cap(void)
{
    uint32_t cap2 = hba_read(HBA_CAP2_OFFSET);
    return (cap2 & CAP2_SNCQ) ? 1 : 0;
}

/**
 * ahci_ncq_read — submit an NCQ READ FPDMA QUEUED command to a port.
 * Returns 0 on success, -1 on error.
 */
int ahci_ncq_read(int port_num, int pm_port, uint32_t lba, uint8_t count,
                   void *buf)
{
    /* Find the port by physical port number and PM port */
    struct ahci_port *port = NULL;
    for (int i = 0; i < ahci_port_count; i++) {
        if (ahci_ports[i].present &&
            ahci_ports[i].port_num == port_num &&
            ahci_ports[i].pm_port == pm_port) {
            port = &ahci_ports[i];
            break;
        }
    }
    if (!port || !port->ncq_capable)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);

    /* Find a free NCQ slot (1-31) */
    uint32_t ci   = port_read(port->port_num, PORT_CI);
    uint32_t sact = port_read(port->port_num, PORT_SACT);
    uint32_t busy = ci | sact;

    int slot = -1;
    for (int i = 1; i < AHCI_SLOT_COUNT; i++) {
        if (!(busy & (1u << i))) { slot = i; break; }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&ahci_lock, irq_flags);
        return -1;
    }

    /* Build a single blk_request to use the existing NCQ path */
    struct blk_request *req = blk_request_alloc();
    if (!req) {
        spinlock_irqsave_release(&ahci_lock, irq_flags);
        return -1;
    }

    req->lba = lba;
    req->count = count;
    req->buf = buf;
    req->flags = BLK_REQ_READ;

    /* Copy data buffer into the slot's DMA buffer (direction: device→host) */
    /* For reads, the slot data buf will be written by the device */

    ahci_build_ncq_cmd(port, slot, req);
    port->inflight_mask |= (1u << slot);
    port->slots[slot].req = req;
    spinlock_irqsave_release(&ahci_lock, irq_flags);

    ahci_issue_ncq(port, slot);

    /* Wait for completion */
    int timeout = 10000000;
    while (timeout--) {
        if (port->slots[slot].req == NULL) {
            /* Completed — data is already copied by IRQ handler */
            blk_request_free(req);
            return 0;
        }
        __asm__ volatile("pause");
    }

    /* Timeout */
    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);
    port->slots[slot].req = NULL;
    port->inflight_mask &= ~(1u << slot);
    ahci_free_slot(port, slot);
    spinlock_irqsave_release(&ahci_lock, irq_flags);
    blk_request_free(req);
    return -1;
}

/**
 * ahci_ncq_write — submit an NCQ WRITE FPDMA QUEUED command.
 * Returns 0 on success, -1 on error.
 */
int ahci_ncq_write(int port_num, int pm_port, uint32_t lba, uint8_t count,
                    const void *buf)
{
    /* Find the port */
    struct ahci_port *port = NULL;
    for (int i = 0; i < ahci_port_count; i++) {
        if (ahci_ports[i].present &&
            ahci_ports[i].port_num == port_num &&
            ahci_ports[i].pm_port == pm_port) {
            port = &ahci_ports[i];
            break;
        }
    }
    if (!port || !port->ncq_capable)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);

    uint32_t ci   = port_read(port->port_num, PORT_CI);
    uint32_t sact = port_read(port->port_num, PORT_SACT);
    uint32_t busy = ci | sact;

    int slot = -1;
    for (int i = 1; i < AHCI_SLOT_COUNT; i++) {
        if (!(busy & (1u << i))) { slot = i; break; }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&ahci_lock, irq_flags);
        return -1;
    }

    struct blk_request *req = blk_request_alloc();
    if (!req) {
        spinlock_irqsave_release(&ahci_lock, irq_flags);
        return -1;
    }

    req->lba = lba;
    req->count = count;
    req->buf = (void *)buf;
    req->flags = BLK_REQ_WRITE;

    ahci_build_ncq_cmd(port, slot, req);
    port->inflight_mask |= (1u << slot);
    port->slots[slot].req = req;
    spinlock_irqsave_release(&ahci_lock, irq_flags);

    ahci_issue_ncq(port, slot);

    /* Wait for completion */
    int timeout = 10000000;
    while (timeout--) {
        if (port->slots[slot].req == NULL) {
            blk_request_free(req);
            return 0;
        }
        __asm__ volatile("pause");
    }

    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);
    port->slots[slot].req = NULL;
    port->inflight_mask &= ~(1u << slot);
    ahci_free_slot(port, slot);
    spinlock_irqsave_release(&ahci_lock, irq_flags);
    blk_request_free(req);
    return -1;
}

/**
 * ahci_ncq_completion_poll — poll for NCQ completions on a physical port.
 * Checks the SDB FIS and completes any finished NCQ commands.
 * This can be called from a timer or idle loop instead of waiting for IRQs.
 * Returns the number of commands completed, or 0 if none.
 */
int ahci_ncq_completion_poll(int port_num)
{
    uint32_t port_is = port_read(port_num, PORT_IS);
    if (!(port_is & PORT_IS_SDBS))
        return 0;

    /* Clear the SDBS interrupt status bit */
    port_write(port_num, PORT_IS, port_is);

    uint32_t ci   = port_read(port_num, PORT_CI);
    uint32_t sact = port_read(port_num, PORT_SACT);
    uint32_t all_done = ~(ci | sact);
    int completed = 0;

    for (int i = 0; i < ahci_port_count; i++) {
        struct ahci_port *p = &ahci_ports[i];
        if (!p->present || p->port_num != port_num)
            continue;

        uint32_t done = p->inflight_mask & all_done;
        if (!done) continue;

        for (int slot = 1; slot < AHCI_SLOT_COUNT; slot++) {
            if (done & (1u << slot)) {
                struct ahci_slot *s = &p->slots[slot];
                struct blk_request *req = s->req;
                if (req) {
                    s->req = NULL;
                    p->inflight_mask &= ~(1u << slot);
                    ahci_free_slot(p, slot);

                    /* Signal activity LED on completion */
                    ahci_led_activity(port_num);

                    uint32_t tfd = port_read(port_num, PORT_TFD);
                    if (tfd & PORT_TFD_ERR) {
                        req->result = -1;
                    } else {
                        req->result = 0;
                        /* Copy data back for reads */
                        if (req->flags & BLK_REQ_READ) {
                            memcpy(req->buf, s->data_buf_virt,
                                   (size_t)req->count * AHCI_SECTOR_SIZE);
                        }
                    }
                    blk_request_done(req);
                    completed++;
                }
            }
        }
    }

    return completed;
}

/* ── NCQ Error Recovery ───────────────────────────────────────────── */

/* READ LOG EXT — log page 0x10 (NCQ Queue Error Log) */
#define ATA_LOG_NCQ_QUEUE_ERROR 0x10
#define NCQ_ERROR_LOG_SIZE      512

/* RECEIVE FPDMA QUEUED sub-commands */
#define NCQ_RECV_DMA_SENSE       0x00

/**
 * ahci_ncq_read_log_ext — read a log page from the device via READ LOG EXT.
 * @port_num: physical port number
 * @log_page: log page address (e.g. 0x10 for NCQ error log)
 * @buf:      512-byte output buffer
 * Returns 0 on success, -1 on error.
 */
static int ahci_ncq_read_log_ext(int port_num, uint8_t log_page, void *buf)
{
    /* Use port's slot 0 for this non-NCQ command */
    struct ahci_port *port = NULL;
    for (int i = 0; i < ahci_port_count; i++) {
        if (ahci_ports[i].present && ahci_ports[i].port_num == port_num) {
            port = &ahci_ports[i];
            break;
        }
    }
    if (!port) return -1;

    /* Set up the buffer for log data */
    memset(port->slots[0].data_buf_virt, 0, 512);

    /* Build READ LOG EXT command via non-NCQ path.
     * READ LOG EXT (0x2F): reads 512-byte log page.
     * feature(low) = log_page, count = 1 (sector) */
    ahci_build_raw_cmd(port, 0, 0x2F, 0, 0,
                       port->slots[0].data_buf_phys, 0,
                       sizeof(struct fis_reg_h2d) / 4);

    /* Set log page address in feature register and count=1 */
    {
        struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)
            port->slots[0].cmd_tbl_virt;
        struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
        fis->featurel = log_page;
        fis->featureh = 0;
        fis->countl = 1;
        fis->counth = 0;
        tbl->prdt[0].dbc = 511;  /* 512 bytes - 1 */
    }

    int ret = ahci_issue_non_ncq(port, 0);
    if (ret == 0) {
        memcpy(buf, port->slots[0].data_buf_virt, 512);
    }
    return ret;
}

/**
 * ahci_ncq_recover_port — perform NCQ error recovery on a physical port.
 * Reads the NCQ Queue Error Log, identifies the failing command tag,
 * clears the error state, and issues a soft reset if needed.
 * @port_num: physical port number
 * Returns 0 on recovery success, -1 on failure.
 */
int ahci_ncq_recover_port(int port_num)
{
    uint8_t error_log[512];
    int ret = ahci_ncq_read_log_ext(port_num, ATA_LOG_NCQ_QUEUE_ERROR,
                                     error_log);
    if (ret < 0) {
        kprintf("AHCI: NCQ error recovery failed to read log on port %d\n",
                port_num);
        /* Fall through to port reset */
        goto do_reset;
    }

    /* NCQ Queue Error Log (ATA spec, log page 0x10):
     * bytes 0-1: NCQ Tag of the first failed command
     * byte 2:    error flags
     * byte 3:    error count
     * bytes 4-7: LBA of the failing command (low)
     * bytes 8-11: LBA (high)
     * byte 12:   SActive (bits 0-7)
     * byte 13:   SActive (bits 8-15)
     * byte 14:   SActive (bits 16-23)
     * byte 15:   SActive (bits 24-31) */
    uint16_t failed_tag = (uint16_t)error_log[0] | ((uint16_t)error_log[1] << 8);
    uint8_t err_flags = error_log[2];

    kprintf("AHCI: NCQ error on port %d tag=%u flags=0x%02x err_count=%u\n",
            port_num,
            (unsigned int)failed_tag,
            (unsigned int)err_flags,
            (unsigned int)error_log[3]);

    /* Check if we should issue RECEIVE FPDMA QUEUED to get sense data */
    if (err_flags & 0x01) {
        /* Sense data available — would issue RECEIVE FPDMA QUEUED here.
         * For now, just log it and reset the port. */
        kprintf("AHCI: NCQ sense data available on port %d\n", port_num);
    }

    /* Abort all in-flight requests for this port */
    for (int i = 0; i < ahci_port_count; i++) {
        struct ahci_port *p = &ahci_ports[i];
        if (!p->present || p->port_num != port_num)
            continue;

        for (int s = 0; s < AHCI_SLOT_COUNT; s++) {
            struct blk_request *req = p->slots[s].req;
            if (req) {
                p->slots[s].req = NULL;
                tag_bitmap_free(&p->tag_bitmap, s);
                req->result = -EIO;
                blk_request_done(req);
            }
        }
        p->inflight_mask = 0;
        p->tag_bitmap = 0;
    }

do_reset:
    /* Perform a soft reset on the port: stop and restart */
    {
        int timeout = 500000;
        uint32_t cmd = port_read(port_num, PORT_CMD);
        cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
        port_write(port_num, PORT_CMD, cmd);
        while ((port_read(port_num, PORT_CMD) &
                (PORT_CMD_CR | PORT_CMD_FR)) && --timeout) {
            __asm__ volatile("pause");
        }

        port_write(port_num, PORT_SERR, 0xFFFFFFFF);
        port_write(port_num, PORT_IS, 0xFFFFFFFF);

        timeout = 500000;
        while ((port_read(port_num, PORT_CMD) & PORT_CMD_CR) && --timeout)
            __asm__ volatile("pause");

        cmd = port_read(port_num, PORT_CMD);
        cmd |= PORT_CMD_FRE | PORT_CMD_ST;
        port_write(port_num, PORT_CMD, cmd);
    }

    kprintf("AHCI: NCQ error recovery complete on port %d\n", port_num);
    return 0;
}

/* ── NCQ Priority Management ───────────────────────────────────────── */

/**
 * ahci_ncq_set_priority — set the NCQ priority level for a port.
 * @port_num: physical port number
 * @priority: 0=simple, 1=deterministic, 2=high
 * Returns 0 on success, -1 if port not found or invalid priority.
 */
int ahci_ncq_set_priority(int port_num, int priority)
{
    if (priority < 0 || priority > 2)
        return -1;

    for (int i = 0; i < ahci_port_count; i++) {
        if (ahci_ports[i].present && ahci_ports[i].port_num == port_num) {
            ahci_ports[i].ncq_priority = priority;
            return 0;
        }
    }
    return -1;
}

/**
 * ahci_ncq_get_priority — get the NCQ priority level for a port.
 * Returns priority (0-2), or -1 if port not found.
 */
int ahci_ncq_get_priority(int port_num)
{
    for (int i = 0; i < ahci_port_count; i++) {
        if (ahci_ports[i].present && ahci_ports[i].port_num == port_num) {
            return ahci_ports[i].ncq_priority;
        }
    }
    return -1;
}

/* ── NCQ Queue Status ──────────────────────────────────────────────── */

/**
 * ahci_ncq_queue_status — query NCQ queue state for a physical port.
 * @port_num:   physical port number
 * @out_active:  output: bitmask of active (in-flight) NCQ slots
 * @out_free:    output: bitmask of free NCQ slots
 * Returns 0 on success, -1 if port not found.
 */
int ahci_ncq_queue_status(int port_num, uint32_t *out_active,
                           uint32_t *out_free)
{
    for (int i = 0; i < ahci_port_count; i++) {
        struct ahci_port *p = &ahci_ports[i];
        if (p->present && p->port_num == port_num) {
            uint32_t sact = port_read(port_num, PORT_SACT);
            uint32_t ci   = port_read(port_num, PORT_CI);
            uint32_t active = ci | sact;

            if (out_active)
                *out_active = active;
            if (out_free)
                *out_free = (~active) & 0xFFFFFFFEU; /* exclude slot 0 */
            return 0;
        }
    }
    return -1;
}

/* ── PM (Port Multiplier) SCR Access ─────────────────────────────────── */

/* AHCI CAP bit definitions for PM support */
#define CAP_SPM         (1u << 17)  /* Supports Port Multiplier */
#define CAP_PSC         (1u << 4)   /* Port Multiplier SCR Access */

/* PM port encoding in PORT_SCTL bits 27:24 */
#define SCTL_PMP_SHIFT  24
#define SCTL_PMP_MASK   ((uint32_t)0x0F << SCTL_PMP_SHIFT)

/*
 * On AHCI 1.3+ controllers, PM port SCR access is provided by writing
 * the PM port number to bits 27:24 of PORT_SCTL. Subsequent reads from
 * PORT_SSTS, PORT_SERR, and PORT_SACT return that PM port's registers.
 *
 * Controllers with CAP.PSC=1 support this. We check at runtime via the
 * HBA_CAP register and fall back cleanly when not supported.
 */

/**
 * ahci_pm_scr_read — read a PM port's SCR register.
 * Reads SStatus, SError, SControl, or SActive for a specific PM port.
 * Returns 0 on success, -1 if unsupported or invalid args.
 */
int ahci_pm_scr_read(int phys_port, int pm_port, int scr_addr,
                      uint32_t *val)
{
    if (!val || pm_port < 0 || pm_port > AHCI_PM_PORT_SELF || scr_addr > 3)
        return -1;

    /* Check if controller supports PM SCR access */
    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    if (!(cap & CAP_PSC))
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);

    /* Save original SCTL, write PM port to bits 27:24 */
    uint32_t orig_sctl = port_read(phys_port, PORT_SCTL);
    uint32_t sctl = (orig_sctl & ~SCTL_PMP_MASK)
                  | ((uint32_t)(pm_port & 0x0F) << SCTL_PMP_SHIFT);
    port_write(phys_port, PORT_SCTL, sctl);
    __asm__ volatile("mfence" ::: "memory");

    /* Read the selected SCR register */
    switch (scr_addr) {
    case AHCI_SCR_STATUS:
        *val = port_read(phys_port, PORT_SSTS);
        break;
    case AHCI_SCR_ERROR:
        *val = port_read(phys_port, PORT_SERR);
        break;
    case AHCI_SCR_CONTROL:
        *val = port_read(phys_port, PORT_SCTL) & 0x0F0F0F0F;
        break;
    case AHCI_SCR_ACTIVE:
        *val = port_read(phys_port, PORT_SACT);
        break;
    default:
        port_write(phys_port, PORT_SCTL, orig_sctl);
        spinlock_irqsave_release(&ahci_lock, irq_flags);
        return -1;
    }

    /* Restore original SCTL */
    port_write(phys_port, PORT_SCTL, orig_sctl);
    __asm__ volatile("mfence" ::: "memory");

    spinlock_irqsave_release(&ahci_lock, irq_flags);
    return 0;
}

/**
 * ahci_pm_scr_write — write a PM port's SCR register.
 * Typically used for clearing SError or writing SControl (DET, SPD, IPM).
 * Returns 0 on success, -1 if unsupported or invalid args.
 */
int ahci_pm_scr_write(int phys_port, int pm_port, int scr_addr,
                       uint32_t val)
{
    if (pm_port < 0 || pm_port > AHCI_PM_PORT_SELF || scr_addr > 2)
        return -1;
    if (scr_addr == AHCI_SCR_ACTIVE)
        return -1;  /* SActive is read-only via this path */

    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    if (!(cap & CAP_PSC))
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);

    uint32_t orig_sctl = port_read(phys_port, PORT_SCTL);
    uint32_t sctl = (orig_sctl & ~SCTL_PMP_MASK)
                  | ((uint32_t)(pm_port & 0x0F) << SCTL_PMP_SHIFT);
    port_write(phys_port, PORT_SCTL, sctl);
    __asm__ volatile("mfence" ::: "memory");

    switch (scr_addr) {
    case AHCI_SCR_ERROR:
        port_write(phys_port, PORT_SERR, val);
        break;
    case AHCI_SCR_CONTROL:
        /* Preserve PMP field, write control bits only */
        val = (val & 0x0F0F0F0F) | (sctl & SCTL_PMP_MASK);
        port_write(phys_port, PORT_SCTL, val);
        break;
    default:
        port_write(phys_port, PORT_SCTL, orig_sctl);
        spinlock_irqsave_release(&ahci_lock, irq_flags);
        return -1;
    }

    __asm__ volatile("mfence" ::: "memory");
    port_write(phys_port, PORT_SCTL, orig_sctl);
    __asm__ volatile("mfence" ::: "memory");

    spinlock_irqsave_release(&ahci_lock, irq_flags);
    return 0;
}

/* ── PM Port Detection & Reset ──────────────────────────────────────── */

/**
 * ahci_pm_port_detect — check if a PM port has a device connected.
 * Uses PM SCR access to read SStatus.DET for the PM port.
 * Returns 1 if device present, 0 if absent, -1 on error.
 */
int ahci_pm_port_detect(int phys_port, int pm_port)
{
    uint32_t ssts;
    if (ahci_pm_scr_read(phys_port, pm_port, AHCI_SCR_STATUS, &ssts) < 0)
        return -1;
    return ((ssts & SSTS_DET_MASK) == SSTS_DET_PRESENT) ? 1 : 0;
}

/**
 * ahci_pm_port_reset — perform a COMRESET on a specific PM port.
 * Cycles DET in the port's SControl register (0→1→0).
 * Returns 0 on success, -1 on failure.
 */
int ahci_pm_port_reset(int phys_port, int pm_port)
{
    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    if (!(cap & CAP_PSC))
        return -1;

    /* Write DET=0 to current SControl to initiate reset */
    if (ahci_pm_scr_write(phys_port, pm_port, AHCI_SCR_CONTROL,
                           (uint32_t)SCTL_DET_INIT << 0) < 0)
        return -1;

    /* Wait for COMRESET to complete — check for device presence */
    __asm__ volatile("mfence" ::: "memory");
    for (int i = 0; i < 10000; i++) {
        uint32_t ssts;
        if (ahci_pm_scr_read(phys_port, pm_port, AHCI_SCR_STATUS, &ssts) == 0) {
            if ((ssts & SSTS_DET_MASK) == SSTS_DET_PRESENT)
                return 0;
        }
        __asm__ volatile("pause");
    }

    return -1;
}

/**
 * ahci_pm_get_port_map — get bitmask of PM ports with connected devices.
 * Checks SStatus.DET for each PM port 0-14.
 * Returns the number of connected ports, or -1 on error.
 */
int ahci_pm_get_port_map(int phys_port, uint32_t *port_map)
{
    if (!port_map)
        return -1;

    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    if (!(cap & CAP_PSC))
        return -1;

    *port_map = 0;
    int count = 0;

    for (int pm = 0; pm < AHCI_MAX_PM_PORTS; pm++) {
        uint32_t ssts;
        if (ahci_pm_scr_read(phys_port, pm, AHCI_SCR_STATUS, &ssts) == 0) {
            if ((ssts & SSTS_DET_MASK) == SSTS_DET_PRESENT) {
                *port_map |= (1u << pm);
                count++;
            }
        }
    }

    return count;
}

/* ── PM Info & Diagnostics ──────────────────────────────────────────── */

/**
 * ahci_pm_get_info — read PM capabilities from GSCR registers.
 * Uses SCR read at PM port 15 (the PM itself) to get revision and port count.
 * Returns 0 on success, -1 if not supported.
 */
int ahci_pm_get_info(int phys_port, uint32_t *revision,
                      uint32_t *port_count)
{
    if (!revision || !port_count)
        return -1;

    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    if (!(cap & CAP_PSC))
        return -1;

    /*
     * PM GSCR registers are accessed via SCR read at PM port 15
     * with the GSCR address encoded in bits 27:24. This encoding
     * is identical to per-port SCR access, but at PM port 15 the
     * read returns the PM's internal status registers.
     *
     * GSCR 0: PM revision
     * GSCR 1: Number of PM ports - 1
     */
    uint32_t rev, nports;

    if (ahci_pm_scr_read(phys_port, AHCI_PM_PORT_SELF,
                          AHCI_PM_GSCR_REVISION, &rev) < 0)
        return -1;
    if (ahci_pm_scr_read(phys_port, AHCI_PM_PORT_SELF,
                          AHCI_PM_GSCR_PORT_COUNT, &nports) < 0)
        return -1;

    *revision   = rev;
    *port_count = (nports & 0x0F) + 1;  /* GSCR 1 = N-1 encoding */

    return 0;
}

/**
 * ahci_pm_dump_info — print PM diagnostic information to kernel log.
 */
void ahci_pm_dump_info(int phys_port)
{
    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    int has_psc = (cap & CAP_PSC) ? 1 : 0;

    kprintf("  AHCI PM diagnostics (physical port %d):\n", phys_port);
    kprintf("    CAP.PSC (PM SCR access): %s\n",
            has_psc ? "yes" : "no");

    uint32_t port_map;
    int connected = ahci_pm_get_port_map(phys_port, &port_map);
    if (connected >= 0) {
        kprintf("    Connected PM port map: 0x%04x (%d ports)\n",
                (unsigned int)port_map, connected);
    } else {
        uint32_t ssts = port_read(phys_port, PORT_SSTS);
        kprintf("    PM SCR not available; SSTS=0x%08x\n",
                (unsigned int)ssts);
    }

    /* Log per-PM-port status */
    for (int pm = 0; pm < AHCI_MAX_PM_PORTS; pm++) {
        uint32_t ssts, serr;
        if (ahci_pm_scr_read(phys_port, pm, AHCI_SCR_STATUS, &ssts) == 0) {
            ahci_pm_scr_read(phys_port, pm, AHCI_SCR_ERROR, &serr);
            uint8_t det = ssts & SSTS_DET_MASK;
            if (det != SSTS_DET_NONE) {
                kprintf("    PM port %2d: DET=%u SPD=%u IPM=%u SERR=0x%04x\n",
                        pm,
                        (unsigned int)det,
                        (unsigned int)((ssts >> 4) & 0x0F),
                        (unsigned int)((ssts >> 8) & 0x0F),
                        (unsigned int)(serr));
            }
        }
    }
}

/* ── ALPM (Aggressive Link Power Management) ─────────────────────────── */

/* CAP register bits for power management */
#define CAP_SPS         (1u << 28)  /* Supports Partial State */
#define CAP_SSS         (1u << 27)  /* Supports Slumber State */
#define CAP_SXS         (1u << 19)  /* Supports Device Sleep */

/* SCTL.IPM — Interface Power Management (bits 8-9) */
#define SCTL_IPM_SHIFT  8
#define SCTL_IPM_MASK   ((uint32_t)0x03 << SCTL_IPM_SHIFT)
#define SCTL_IPM_NONE      ((uint32_t)0x00 << SCTL_IPM_SHIFT) /* No restriction */
#define SCTL_IPM_NOPARTIAL ((uint32_t)0x01 << SCTL_IPM_SHIFT) /* Disable Partial */
#define SCTL_IPM_NOSLUMBER ((uint32_t)0x02 << SCTL_IPM_SHIFT) /* Disable Slumber */
#define SCTL_IPM_NOBOTH    ((uint32_t)0x03 << SCTL_IPM_SHIFT) /* Disable both */

/* SCTL.ASP — Aggressive Slumber/Partial (bit 10) */
#define SCTL_ASP        (1u << 10)

/* SSTS.IPM — Interface Power Management status (bits 8-11) */
#define SSTS_IPM_SHIFT  8
#define SSTS_IPM_MASK   ((uint32_t)0x0F << SSTS_IPM_SHIFT)
#define SSTS_IPM_ACTIVE   (0x01u << SSTS_IPM_SHIFT)
#define SSTS_IPM_PARTIAL  (0x02u << SSTS_IPM_SHIFT)
#define SSTS_IPM_SLUMBER  (0x06u << SSTS_IPM_SHIFT)
#define SSTS_IPM_DEVSLEEP (0x08u << SSTS_IPM_SHIFT)

/* Policy-to-SCTL.IPM lookup */
static const uint32_t alpm_policy_ipm[] = {
    [AHCI_ALPM_POLICY_PERFORMANCE]   = SCTL_IPM_NOBOTH,
    [AHCI_ALPM_POLICY_ALLOW_PARTIAL] = SCTL_IPM_NOSLUMBER,
    [AHCI_ALPM_POLICY_ALLOW_SLUMBER] = SCTL_IPM_NOPARTIAL,
    [AHCI_ALPM_POLICY_ALLOW_BOTH]    = SCTL_IPM_NONE,
    [AHCI_ALPM_POLICY_AGGRESSIVE]    = SCTL_IPM_NONE,
};

/**
 * ahci_alpm_is_supported — check if the controller supports Partial/Slumber.
 * Returns bitmask: bit 0 = Partial supported, bit 1 = Slumber supported.
 */
int ahci_alpm_is_supported(void)
{
    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    int support = 0;
    if (cap & CAP_SPS)
        support |= 1;
    if (cap & CAP_SSS)
        support |= 2;
    return support;
}

/**
 * ahci_alpm_has_devsleep — check if the controller supports Device Sleep.
 * Returns 1 if supported, 0 if not.
 */
int ahci_alpm_has_devsleep(void)
{
    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    return (cap & CAP_SXS) ? 1 : 0;
}

/**
 * ahci_alpm_set_policy — set the ALPM policy for a physical port.
 * @port_num: physical port number (0-31)
 * @policy:   one of AHCI_ALPM_POLICY_* constants
 * Returns 0 on success, -1 if port invalid or policy unsupported.
 */
int ahci_alpm_set_policy(int port_num, int policy)
{
    if (port_num < 0 || port_num > 31)
        return -1;
    if (policy < AHCI_ALPM_POLICY_PERFORMANCE ||
        policy > AHCI_ALPM_POLICY_AGGRESSIVE)
        return -1;

    uint32_t sctl = port_read(port_num, PORT_SCTL);

    /* Clear IPM field + ASP bit */
    sctl &= ~(SCTL_IPM_MASK | SCTL_ASP);

    if (policy == AHCI_ALPM_POLICY_AGGRESSIVE) {
        /* Aggressive: no IPM restriction + enable ASP */
        sctl |= SCTL_IPM_NONE | SCTL_ASP;
    } else {
        sctl |= alpm_policy_ipm[policy];
    }

    port_write(port_num, PORT_SCTL, sctl);
    __asm__ volatile("mfence" ::: "memory");
    return 0;
}

/**
 * ahci_alpm_get_policy — get the current ALPM policy for a physical port.
 * Returns the policy constant on success, -1 on error.
 */
int ahci_alpm_get_policy(int port_num)
{
    if (port_num < 0 || port_num > 31)
        return -1;

    uint32_t sctl = port_read(port_num, PORT_SCTL);
    uint32_t ipm = sctl & SCTL_IPM_MASK;
    uint32_t asp = sctl & SCTL_ASP;

    /* Reverse lookup: match IPM field to policy constant */
    for (int p = AHCI_ALPM_POLICY_PERFORMANCE; p <= AHCI_ALPM_POLICY_AGGRESSIVE; p++) {
        if (p == AHCI_ALPM_POLICY_AGGRESSIVE) {
            /* Aggressive = no IPM restriction + ASP set */
            if (ipm == SCTL_IPM_NONE && asp)
                return AHCI_ALPM_POLICY_AGGRESSIVE;
        } else if (ipm == alpm_policy_ipm[p]) {
            return p;
        }
    }
    return AHCI_ALPM_POLICY_PERFORMANCE;  /* default if no match */
}

/**
 * ahci_alpm_get_state — get the current link power state for a physical port.
 * Returns AHCI_ALPM_STATE_* on success, -1 on error.
 */
int ahci_alpm_get_state(int port_num)
{
    if (port_num < 0 || port_num > 31)
        return -1;

    uint32_t ssts = port_read(port_num, PORT_SSTS);
    uint32_t ipm = ssts & SSTS_IPM_MASK;

    if (ipm == SSTS_IPM_ACTIVE)
        return AHCI_ALPM_STATE_ACTIVE;
    if (ipm == SSTS_IPM_PARTIAL)
        return AHCI_ALPM_STATE_PARTIAL;
    if (ipm == SSTS_IPM_SLUMBER)
        return AHCI_ALPM_STATE_SLUMBER;
    if (ipm == SSTS_IPM_DEVSLEEP)
        return AHCI_ALPM_STATE_DEVSLEEP;

    /* If DET shows no device, report active as safe default */
    return AHCI_ALPM_STATE_ACTIVE;
}

/**
 * ahci_alpm_set_aggressive — enable/disable aggressive link power transitions.
 * @port_num: physical port number
 * @enable:   1 = enable ASP, 0 = disable ASP
 * Returns 0 on success, -1 on error.
 */
int ahci_alpm_set_aggressive(int port_num, int enable)
{
    if (port_num < 0 || port_num > 31)
        return -1;

    uint32_t sctl = port_read(port_num, PORT_SCTL);
    if (enable)
        sctl |= SCTL_ASP;
    else
        sctl &= ~SCTL_ASP;
    port_write(port_num, PORT_SCTL, sctl);
    __asm__ volatile("mfence" ::: "memory");
    return 0;
}

/**
 * ahci_alpm_wake — wake the link from Partial/Slumber to active.
 * Sets the policy to PERFORMANCE (disable power saving) and waits
 * for the link to report active state.
 * @port_num: physical port number
 * Returns 0 if already active or wake successful, -1 on timeout.
 */
int ahci_alpm_wake(int port_num)
{
    if (port_num < 0 || port_num > 31)
        return -1;

    /* Check if already active */
    int state = ahci_alpm_get_state(port_num);
    if (state == AHCI_ALPM_STATE_ACTIVE)
        return 0;

    /* Set PERFORMANCE policy (disable both Partial and Slumber) */
    ahci_alpm_set_policy(port_num, AHCI_ALPM_POLICY_PERFORMANCE);

    /* Wake the link by toggling DET via SCTL */
    uint32_t sctl = port_read(port_num, PORT_SCTL);
    sctl &= ~(SCTL_IPM_MASK | SCTL_ASP);
    sctl |= SCTL_IPM_NOBOTH;  /* disable all power saving */
    sctl |= (uint32_t)SCTL_DET_INIT;  /* initiate COMRESET to wake */
    port_write(port_num, PORT_SCTL, sctl);
    __asm__ volatile("mfence" ::: "memory");

    /* Wait for link to wake */
    for (int i = 0; i < 10000; i++) {
        uint32_t ssts = port_read(port_num, PORT_SSTS);
        uint8_t det = ssts & SSTS_DET_MASK;
        if (det == SSTS_DET_PRESENT) {
            /* Now clear DET back to normal operation */
            uint32_t cur = port_read(port_num, PORT_SCTL);
            cur &= ~(uint32_t)0x0F;  /* clear DET field */
            port_write(port_num, PORT_SCTL, cur);
            __asm__ volatile("mfence" ::: "memory");
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

/**
 * ahci_alpm_init_port — initialize ALPM for a physical port during setup.
 * Sets a sensible default policy (PERFORMANCE = no power saving by default).
 * @port_num: physical port number
 * Returns 0 on success, -1 on error.
 */
int ahci_alpm_init_port(int port_num)
{
    if (port_num < 0 || port_num > 31)
        return -1;

    /* Default to PERFORMANCE policy — disable power saving during init
     * to ensure reliable device detection and IDENTIFY. The policy
     * can be changed later at runtime. */
    if (ahci_alpm_set_policy(port_num, AHCI_ALPM_POLICY_PERFORMANCE) < 0)
        return -1;

    /* Clear any pending interrupt status that might have been set
     * during power state transitions */
    port_write(port_num, PORT_SERR, 0xFFFFFFFF);
    port_write(port_num, PORT_IS, PORT_IS_PCS);  /* PhyRdy change */
    return 0;
}

/* ── AHCI LED Management (External Message Interface) ───────────────── */

/* CAP bit for LED management support (AHCI 1.3, Section 3.1.1) */
#define CAP_LED         (1u << 7)

/* External Message Interface registers (AHCI 1.3, Section 10.9) */
#define HBA_EM_LOC_OFFSET  0x40    /* EM Location */
#define HBA_EM_CTL_OFFSET  0x44    /* EM Control */

/* EM_LOC bit definitions */
#define EM_LOC_MSG_TYPE_MASK   0xFF000000
#define EM_LOC_MSG_TYPE_SHIFT  24
#define EM_LOC_SMB             (1u << 23)  /* 1 = SMBIOS, 0 = memory buffer */
#define EM_LOC_ADDR_MASK       0x007FFFFF  /* buffer address / SMBIOS handle */

/* EM_CTL bit definitions */
#define EM_CTL_MR       (1u << 0)  /* Message Received */
#define EM_CTL_TM       (1u << 1)  /* Transmit Message */
#define EM_CTL_RST      (1u << 9)  /* Reset */

/* EM message types */
#define EM_MSG_TYPE_LED     1  /* LED message */

/* Static EM state */
static int              ahci_led_em_type = 0;   /* EM message type (0 = none) */
static volatile uint32_t *ahci_led_msg_buf = NULL;  /* EM message buffer VA */

/**
 * ahci_led_is_supported — check if the HBA supports LED management.
 * Returns 1 if LED management via EM interface is available, 0 otherwise.
 */
int ahci_led_is_supported(void)
{
    return (ahci_led_em_type == EM_MSG_TYPE_LED) ? 1 : 0;
}

/**
 * ahci_led_init — probe the EM (External Message Interface) for LED support.
 * Checks CAP.LED and configures the EM message buffer for LED control.
 * Returns 0 on success, -1 if LED management is not available.
 */
int ahci_led_init(void)
{
    uint32_t cap = hba_read(HBA_CAP_OFFSET);
    if (!(cap & CAP_LED)) {
        ahci_led_em_type = 0;
        return -1;
    }

    uint32_t em_loc = hba_read(HBA_EM_LOC_OFFSET);
    int msg_type = (int)((em_loc & EM_LOC_MSG_TYPE_MASK) >> EM_LOC_MSG_TYPE_SHIFT);

    if (msg_type != EM_MSG_TYPE_LED) {
        ahci_led_em_type = 0;
        return -1;
    }

    ahci_led_em_type = msg_type;

    /* If the EM buffer is in system memory (not SMBIOS), map it */
    if (!(em_loc & EM_LOC_SMB)) {
        uint64_t buf_paddr = (uint64_t)(em_loc & EM_LOC_ADDR_MASK);
        if (buf_paddr != 0) {
            ahci_led_msg_buf = (volatile uint32_t *)
                PHYS_TO_VIRT((void *)(uintptr_t)buf_paddr);
        }
    }

    /* Reset the EM interface to a known state */
    uint32_t em_ctl = hba_read(HBA_EM_CTL_OFFSET);
    hba_write(HBA_EM_CTL_OFFSET, em_ctl | EM_CTL_RST);
    __asm__ volatile("mfence" ::: "memory");
    hba_write(HBA_EM_CTL_OFFSET, em_ctl & ~EM_CTL_RST);
    __asm__ volatile("mfence" ::: "memory");

    /* Clear any pending message-received flag */
    hba_write(HBA_EM_CTL_OFFSET, hba_read(HBA_EM_CTL_OFFSET) & ~EM_CTL_MR);
    __asm__ volatile("mfence" ::: "memory");

    kprintf("  AHCI: LED management via EM interface (type %d) %s\n",
            msg_type,
            ahci_led_msg_buf ? "memory-buffer" : "SMBIOS");

    return 0;
}

/**
 * ahci_led_set — turn a port's activity LED on or off via EM message.
 * @port_num: physical port number (0-31)
 * @on:        1 = LED on, 0 = LED off
 * Returns 0 on success, -1 if LED management not available or port invalid.
 */
int ahci_led_set(int port_num, int on)
{
    if (ahci_led_em_type != EM_MSG_TYPE_LED)
        return -1;
    if (port_num < 0 || port_num > 31)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);

    if (ahci_led_msg_buf) {
        uint32_t led_state = *ahci_led_msg_buf;
        if (on)
            led_state |= (1u << port_num);
        else
            led_state &= ~(1u << port_num);
        *ahci_led_msg_buf = led_state;
        __asm__ volatile("mfence" ::: "memory");

        /* Trigger EM message transmission */
        hba_write(HBA_EM_CTL_OFFSET, EM_CTL_TM);
        __asm__ volatile("mfence" ::: "memory");
    }

    spinlock_irqsave_release(&ahci_lock, irq_flags);
    return 0;
}

/**
 * ahci_led_activity — pulse the activity LED for a port to indicate I/O.
 * Sets the port's LED bit in the EM message buffer and triggers
 * transmission. The LED will stay lit until the next EM message
 * clears it, providing visual activity indication.
 * @port_num: physical port number (0-31)
 * Returns 0 on success, -1 if LED management not available.
 */
int ahci_led_activity(int port_num)
{
    if (ahci_led_em_type != EM_MSG_TYPE_LED)
        return -1;
    if (port_num < 0 || port_num > 31)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ahci_lock, &irq_flags);

    if (ahci_led_msg_buf) {
        uint32_t led_state = *ahci_led_msg_buf;
        led_state |= (1u << port_num);    /* turn on */
        *ahci_led_msg_buf = led_state;
        __asm__ volatile("mfence" ::: "memory");
        hba_write(HBA_EM_CTL_OFFSET, EM_CTL_TM);
    }

    spinlock_irqsave_release(&ahci_lock, irq_flags);
    return 0;
}

/* ── PM-enhanced NCQ error handling ─────────────────────────────────── */

/**
 * ahci_ncq_pm_recover — NCQ error recovery for a specific PM port.
 * When an error occurs behind a PM, this function attempts to identify
 * which PM port(s) had errors and recover them individually.
 * @phys_port: physical port number
 * @pm_port_mask: bitmask of PM ports to recover (bits 0-14)
 * Returns 0 on success, -1 on complete failure.
 *
 * PM-specific error recovery flow:
 * 1. Read NCQ error log (same as physical port recovery)
 * 2. Determine which PM port caused the error from the error log
 * 3. Abort in-flight commands for that PM port
 * 4. Reset the specific PM port if needed
 * 5. Re-probe the PM port (send IDENTIFY)
 */
static int ahci_ncq_pm_recover(int phys_port, uint32_t pm_port_mask)
{
    kprintf("AHCI: PM NCQ error recovery on phys port %d, mask 0x%04x\n",
            phys_port, (unsigned int)pm_port_mask);

    /* Abort all in-flight requests for the affected PM ports */
    for (int i = 0; i < ahci_port_count; i++) {
        struct ahci_port *p = &ahci_ports[i];
        if (!p->present || p->port_num != phys_port)
            continue;
        if (!p->is_pm || !(pm_port_mask & (1u << p->pm_port)))
            continue;

        for (int s = 0; s < AHCI_SLOT_COUNT; s++) {
            struct blk_request *req = p->slots[s].req;
            if (req) {
                p->slots[s].req = NULL;
                tag_bitmap_free(&p->tag_bitmap, s);
                req->result = -EIO;
                blk_request_done(req);
            }
        }
        p->inflight_mask = 0;
        p->tag_bitmap = 0;
    }

    /* Reset each affected PM port */
    for (int pm = 0; pm < AHCI_MAX_PM_PORTS; pm++) {
        if (pm_port_mask & (1u << pm)) {
            kprintf("AHCI: resetting PM port %d on phys port %d\n",
                    pm, phys_port);
            ahci_pm_port_reset(phys_port, pm);
        }
    }

    kprintf("AHCI: PM NCQ error recovery complete on phys port %d\n",
            phys_port);
    return 0;
}

/* ── Physical port initialization (direct-attached or PM host) ──────── */

/**
 * Initialize a physical AHCI port: allocate command list, FIS area, slots.
 * Returns the index in ahci_ports array, or -1 on failure.
 */
static int ahci_init_phys_port(int p, int irq, int *is_pm) {
    *is_pm = 0;

    /* Check for Port Multiplier: PORT_CMD.PMP (bit 17) */
    uint32_t cmd_reg = port_read(p, PORT_CMD);
    if (cmd_reg & PORT_CMD_PMP) {
        *is_pm = 1;
        kprintf("  AHCI port %d: Port Multiplier detected\n", p);
    }

    /* Allocate port structure at the current end of the array */
    int idx = ahci_port_count;
    struct ahci_port *port = &ahci_ports[idx];
    memset(port, 0, sizeof(*port));
    port->present = 1;
    port->port_num = p;
    port->pm_port = -1;  /* direct attached by default */
    port->is_pm = 0;
    port->irq_line = (uint8_t)irq;
    port->inflight_mask = 0;

    /* Allocate command list (1 frame = 32 × 32-byte headers) */
    port->cmd_list_phys = pmm_alloc_frame();
    if (!port->cmd_list_phys) return -1;
    /* pmm_alloc_frame returns frame index, convert to phys address */
    uint64_t cmd_list_addr = port->cmd_list_phys * 4096;
    memset(PHYS_TO_VIRT((void*)(uintptr_t)cmd_list_addr), 0, 4096);
    port->cmd_list_phys = cmd_list_addr;

    /* Allocate FIS receive area (1 frame) */
    uint64_t fis_frame = pmm_alloc_frame();
    if (!fis_frame) return -1;
    uint64_t fis_addr = fis_frame * 4096;
    memset(PHYS_TO_VIRT((void*)(uintptr_t)fis_addr), 0, 4096);
    port->recv_fis_phys = fis_addr;

    /* Allocate per-slot command tables and data buffers */
    int alloc_ok = 1;
    for (int s = 0; s < AHCI_SLOT_COUNT; s++) {
        struct ahci_slot *slot = &port->slots[s];

        /* Command table: kmalloc (small, ~144 bytes each) */
        slot->cmd_tbl_virt = kmalloc(sizeof(struct ahci_cmd_table));
        if (!slot->cmd_tbl_virt) { alloc_ok = 0; break; }
        memset(slot->cmd_tbl_virt, 0, sizeof(struct ahci_cmd_table));
        slot->cmd_tbl_phys = VIRT_TO_PHYS(slot->cmd_tbl_virt);

        /* Data buffer: one 4KB frame per slot */
        uint64_t data_frame = pmm_alloc_frame();
        if (!data_frame) { alloc_ok = 0; break; }
        slot->data_buf_phys = data_frame * 4096;
        slot->data_buf_virt = PHYS_TO_VIRT((void*)(uintptr_t)slot->data_buf_phys);
        memset(slot->data_buf_virt, 0, 4096);
    }
    if (!alloc_ok) return -1;

    port_stop(p);
    port_write(p, PORT_CLB,  (uint32_t)(port->cmd_list_phys & 0xFFFFFFFF));
    port_write(p, PORT_CLBU, (uint32_t)(port->cmd_list_phys >> 32));
    port_write(p, PORT_FB,   (uint32_t)(port->recv_fis_phys & 0xFFFFFFFF));
    port_write(p, PORT_FBU,  (uint32_t)(port->recv_fis_phys >> 32));
    port_write(p, PORT_SERR, 0xFFFFFFFF);
    port_write(p, PORT_IS,   0xFFFFFFFF);

    /* Initialize ALPM — default to PERFORMANCE (no power saving)
     * to ensure reliable device detection during IDENTIFY. */
    ahci_alpm_init_port(p);

    /* Enable interrupts — mask SDBS and D2H */
    port_write(p, PORT_IE, PORT_IS_D2H | PORT_IS_SDBS | PORT_IS_PCS | PORT_IS_ERR);
    port_start(p);

    /* Don't increment ahci_port_count here — the caller (ahci_probe_device
     * or inline PM probe logic) will do that on successful registration. */
    return idx;
}

/**
 * Probe a device behind a physical port (direct or via PM) and register it.
 * Returns 0 on success, -1 on failure.
 */
static int ahci_probe_device(struct ahci_port *port, int pm_port __attribute__((unused)),
                              const char *devname_fmt, const char *blkdev_name) {
    /* IDENTIFY device */
    uint64_t identify_data = pmm_alloc_frame();
    if (!identify_data) return -1;
    memset(PHYS_TO_VIRT((void*)(uintptr_t)(identify_data * 4096)), 0, 4096);

    ahci_build_raw_cmd(port, 0, ATA_CMD_IDENTIFY, 0, 1,
                       port->slots[0].data_buf_phys, 0,
                       sizeof(struct fis_reg_h2d) / 4);
    /* IDENTIFY uses count=0 in FIS */
    {
        struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)
            port->slots[0].cmd_tbl_virt;
        struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
        fis->countl = 0;
        fis->counth = 0;
        /* PRDT byte count: 512 - 1 */
        tbl->prdt[0].dbc = 511;
    }

    if (ahci_issue_non_ncq(port, 0) == 0) {
        uint16_t *id = (uint16_t *)port->slots[0].data_buf_virt;
        uint32_t lo = ((uint32_t)id[101] << 16) | id[100];
        uint32_t sector_count = lo ? lo : ((uint32_t)id[57] | ((uint32_t)id[58] << 16));

        /* Bail out if zero size (no device on this PM port) */
        if (sector_count == 0) {
            pmm_free_frame(identify_data);
            return -1;
        }

        port->sector_count = sector_count;

        /* Check NCQ support: word 76 bits 8 = NCQ queue depth > 0 */
        if (id[76] & (1u << 8)) {
            port->ncq_capable = 1;
        }

        char capacity_str[32];
        uint32_t capacity_mb = sector_count / 2048;
        if (capacity_mb >= 1024) {
            snprintf(capacity_str, sizeof(capacity_str), "%lu GB",
                     (unsigned long)(capacity_mb / 1024));
        } else {
            snprintf(capacity_str, sizeof(capacity_str), "%lu MB",
                     (unsigned long)capacity_mb);
        }

        kprintf("  %s: %lu sectors (%s)%s\n",
                devname_fmt,
                (unsigned long)sector_count,
                capacity_str,
                port->ncq_capable ? " NCQ" : "");

        pmm_free_frame(identify_data);

        /* Register with block device layer */
        int bd_id = BLOCKDEV_AHCI + ahci_port_count;
        int ret = blockdev_register(bd_id, blkdev_name,
                                    ahci_submit_fn,
                                    ahci_idle_fn,
                                    sector_count,
                                    BLK_DRIVER_ASYNC);
        if (ret == 0) {
            port->blockdev_id = bd_id;
            ahci_port_count++;
            ahci_present = 1;
            return 0;
        }
        return -1;
    } else {
        pmm_free_frame(identify_data);
        return -1;
    }
}

/* ── Initialization ─────────────────────────────────────────────────── */
int __init ahci_init(void) {
    struct pci_device dev;
    if (pci_find_class(0x01, 0x06, &dev) < 0)
        return -1; /* no AHCI controller */

    uint64_t bar5 = dev.bar[5] & ~0xFULL;
    kprintf("  AHCI %04x:%04x (IRQ %d) HBA@0x%llx\n",
            dev.vendor_id, dev.device_id,
            dev.irq, (unsigned long long)bar5);

    hba_base = bar5;
    if (hba_base == 0) return -2;

    /* Map HBA MMIO in high-half VMA space */
    void *hba_virt = vmm_map_phys(hba_base, 0x2000,
                VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    if (IS_ERR(hba_virt)) return -2;
    hba_base = (uint64_t)hba_virt;

    /* Check CAP2.SNCQ for controller-wide NCQ support */
    uint32_t cap2 = hba_read(HBA_CAP2_OFFSET);
    if (cap2 & CAP2_SNCQ) {
        kprintf("  AHCI: Controller supports NCQ (CAP2.SNCQ=1)\n");
    } else {
        kprintf("  AHCI: Controller does NOT support NCQ\n");
    }

    /* Enable PCI bus mastering */
    pci_enable_bus_master(&dev);

    /* Enable AHCI mode and HBA interrupt */
    uint32_t ghc = hba_read(HBA_GHC_OFFSET);
    hba_write(HBA_GHC_OFFSET, ghc | GHC_AE | GHC_IE);

    spinlock_init(&ahci_lock);

    /* Initialize LED management via EM interface (if supported) */
    ahci_led_init();

    /* Scan implemented ports */
    uint32_t pi = hba_read(HBA_PI_OFFSET);
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t ssts = port_read(p, PORT_SSTS);
        uint8_t  det  = ssts & SSTS_DET_MASK;
        if (det != SSTS_DET_PRESENT) continue;

        uint32_t sig = port_read(p, PORT_SIG);
        /* For PM-attached ports, sig might be 0 or the first device's sig.
         * We probe via IDENTIFY regardless. */

        /* Initialize the physical port */
        int is_pm = 0;
        int phys_idx = ahci_init_phys_port(p, dev.irq, &is_pm);
        if (phys_idx < 0) continue;
        ahci_port_count = phys_idx + 1;  /* slot is now occupied */

        struct ahci_port *phys_port = &ahci_ports[phys_idx];

        if (is_pm) {
            /* Port Multiplier present — log detection */
            kprintf("  AHCI port %d: Port Multiplier detected, probing PM sub-ports\n", p);

            /* Dump PM diagnostics if supported */
            uint32_t cap_reg = hba_read(HBA_CAP_OFFSET);
            if (cap_reg & CAP_PSC) {
                uint32_t pm_rev = 0, pm_nports = 0;
                if (ahci_pm_get_info(p, &pm_rev, &pm_nports) == 0) {
                    kprintf("  AHCI PM info: revision=0x%x, %u ports\n",
                            (unsigned int)pm_rev,
                            (unsigned int)pm_nports);
                }
                ahci_pm_dump_info(p);
            }

            /* Probe each PM sub-port (0-14) */
            int pm_devices_found = 0;

            /*
             * Fast path: if the controller supports PM SCR access, check
             * each PM port's SStatus.DET before sending IDENTIFY. This
             * avoids unnecessary commands to unpopulated ports.
             *
             * Fallback: probe all 15 PM ports via IDENTIFY.
             */
            uint32_t pm_port_map = 0;
            int pm_connected = -1;
            if (cap_reg & CAP_PSC) {
                pm_connected = ahci_pm_get_port_map(p, &pm_port_map);
            }

            for (int pm = 0; pm < AHCI_MAX_PM_PORTS; pm++) {
                /* Skip ports not connected (if port map is available) */
                if (pm_connected >= 0 && !(pm_port_map & (1u << pm)))
                    continue;

                /* Check port signature — ATAPI devices need special handling */
                uint32_t pm_sig = port_read(p, PORT_SIG);

                /* IDENTIFY this PM port using the physical port's slot 0 */
                struct ahci_port probe_port;
                memcpy(&probe_port, phys_port, sizeof(probe_port));
                probe_port.pm_port = pm;
                probe_port.is_pm = 1;
                probe_port.sector_count = 0;
                probe_port.ncq_capable = 0;
                probe_port.blockdev_id = 0;
                /* Clear req ptrs from slots (borrowing phys_port's slots) */
                for (int s = 0; s < AHCI_SLOT_COUNT; s++) {
                    probe_port.slots[s].req = NULL;
                }

                int is_atapi = 0;
                uint8_t identify_cmd = ATA_CMD_IDENTIFY;

                /* For ATAPI devices, send IDENTIFY PACKET DEVICE (0xA1) */
                if (pm_sig == AHCI_SIG_ATAPI) {
                    identify_cmd = 0xA1;  /* IDENTIFY PACKET DEVICE */
                    is_atapi = 1;
                }

                ahci_build_raw_cmd(&probe_port, 0, identify_cmd, 0, 1,
                                   phys_port->slots[0].data_buf_phys, 0,
                                   sizeof(struct fis_reg_h2d) / 4);
                {
                    struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)
                        phys_port->slots[0].cmd_tbl_virt;
                    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
                    fis->countl = 0;
                    fis->counth = 0;
                    tbl->prdt[0].dbc = 511;
                }

                if (ahci_issue_non_ncq(&probe_port, 0) == 0) {
                    uint16_t *id = (uint16_t *)phys_port->slots[0].data_buf_virt;

                    /* For ATA devices: sector count from IDENTIFY words 100-101 or 57-58 */
                    /* For ATAPI devices: sector count from words 100-101 or 57-58 */
                    uint32_t lo = ((uint32_t)id[101] << 16) | id[100];
                    uint32_t sector_count = lo ? lo : ((uint32_t)id[57] | ((uint32_t)id[58] << 16));

                    if (sector_count == 0) continue;

                    /* Create a new port entry for this PM device */
                    struct ahci_port *sub_port = &ahci_ports[ahci_port_count];
                    memcpy(sub_port, phys_port, sizeof(struct ahci_port));
                    sub_port->pm_port = pm;
                    sub_port->is_pm = 1;
                    sub_port->sector_count = sector_count;
                    sub_port->ncq_capable = (id[76] & (1u << 8)) ? 1 : 0;
                    sub_port->inflight_mask = 0;
                    /* Clear req ptrs (borrowing phys_port's slot buffers) */
                    for (int s = 0; s < AHCI_SLOT_COUNT; s++) {
                        sub_port->slots[s].req = NULL;
                    }

                    char blkname[16];
                    if (is_atapi) {
                        snprintf(blkname, sizeof(blkname), "ahci%dp%d-cd", p, pm);
                    } else {
                        snprintf(blkname, sizeof(blkname), "ahci%dp%d", p, pm);
                    }

                    int bd_id = BLOCKDEV_AHCI_PM_BASE + ahci_port_count;
                    int ret = blockdev_register(bd_id, blkname,
                                                ahci_submit_fn,
                                                ahci_idle_fn,
                                                sector_count,
                                                BLK_DRIVER_ASYNC);
                    if (ret == 0) {
                        sub_port->blockdev_id = bd_id;
                        ahci_port_count++;
                        ahci_present = 1;
                        pm_devices_found++;
                        kprintf("  AHCI port %d PM %d: %lu sectors (%lu MB)%s%s\n",
                                p, pm,
                                (unsigned long)sector_count,
                                (unsigned long)(sector_count / 2048),
                                sub_port->ncq_capable ? " NCQ" : "",
                                is_atapi ? " ATAPI" : "");
                    }
                }
            }

            if (pm_devices_found == 0) {
                kprintf("  AHCI port %d: no PM devices found\n", p);
            } else {
                kprintf("  AHCI port %d: %d PM device(s) found\n", p, pm_devices_found);
            }
        } else {
            /* Direct-attached device */
            if (sig != AHCI_SIG_ATA) continue;

            char devname[32];
            snprintf(devname, sizeof(devname), "AHCI port %d", p);

            int found = ahci_probe_device(phys_port, -1, devname, "ahci");
            if (found < 0) {
                /* Roll back the port we allocated */
                ahci_port_count--;
            }
        }
    }

    if (!ahci_present) return -4;

    /* Register IRQ handler for the first AHCI port's IRQ line */
    int irq_line = dev.irq;
    idt_register_handler((uint8_t)(32 + irq_line), ahci_irq_handler);
    if (apic_is_init_complete())
        ioapic_unmask_irq((uint8_t)irq_line);
    pic_unmask((uint8_t)irq_line);

    kprintf("  AHCI: %d device(s) active, NCQ depth %ld\n",
            ahci_port_count, (long)AHCI_NCQ_SLOTS);
    return 0;
}
#include "initcall.h"
device_initcall(ahci_init);

/* ── ahci_exit — reverse initialization, for module unloading ── */
void ahci_exit(void) {
    if (!ahci_present) return;

    kprintf("[AHCI] Shutting down SATA controller...\n");

    /* Step 1: Stop all physical ports and disable their interrupts */
    uint32_t pi = hba_read(HBA_PI_OFFSET);
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        port_write(p, PORT_IE, 0);       /* disable port interrupts */
        port_stop(p);                     /* stop port DMA engines */
    }

    /* Step 2: Unregister all block devices */
    for (int i = 0; i < ahci_port_count; i++) {
        struct ahci_port *port = &ahci_ports[i];
        if (!port->present) continue;
        if (port->blockdev_id > 0)
            blockdev_unregister(port->blockdev_id);
    }

    /* Step 3: Free per-port resources */
    for (int i = 0; i < ahci_port_count; i++) {
        struct ahci_port *port = &ahci_ports[i];
        if (!port->present) continue;

        /* Free command list frame */
        if (port->cmd_list_phys) {
            pmm_free_frame(port->cmd_list_phys / 4096);
            port->cmd_list_phys = 0;
        }
        /* Free FIS receive area */
        if (port->recv_fis_phys) {
            pmm_free_frame(port->recv_fis_phys / 4096);
            port->recv_fis_phys = 0;
        }
        /* Free per-slot resources */
        for (int s = 0; s < AHCI_SLOT_COUNT; s++) {
            struct ahci_slot *slot = &port->slots[s];
            if (slot->cmd_tbl_virt) {
                kfree(slot->cmd_tbl_virt);
                slot->cmd_tbl_virt = NULL;
            }
            if (slot->data_buf_phys) {
                pmm_free_frame(slot->data_buf_phys / 4096);
                slot->data_buf_phys = 0;
                slot->data_buf_virt = NULL;
            }
        }
        port->present = 0;
    }

    /* Step 4: Mask IRQ in I/O APIC and PIC */
    if (ahci_port_count > 0) {
        uint8_t irq_line = ahci_ports[0].irq_line;
        if (apic_is_init_complete())
            ioapic_mask_irq(irq_line);
        pic_mask(irq_line);
    }

    /* Step 5: Disable HBA global interrupts and AHCI mode */
    hba_write(HBA_GHC_OFFSET, 0);
    hba_write(HBA_IS_OFFSET, 0xFFFFFFFF);  /* clear pending */

    /* Step 6: Unmap HBA MMIO region */
    if (hba_base) {
        vmm_unmap_phys((void *)hba_base, 0x2000);
        hba_base = 0;
    }

    ahci_present = 0;
    ahci_port_count = 0;
    kprintf("[AHCI] SATA controller shut down\n");
}

int ahci_is_present(void) { return ahci_present; }
uint32_t ahci_get_sectors(void) {
    if (ahci_port_count > 0) return ahci_ports[0].sector_count;
    return 0;
}

/* Legacy synchronous API — for backward compatibility with fat32 etc. */
int ahci_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (!ahci_present || ahci_port_count == 0) return -1;
    if (count == 0 || count > AHCI_DATA_FRAME_SECTORS) return -1;
    return blk_submit_sync(ahci_ports[0].blockdev_id, lba, count, buf, BLK_REQ_READ);
}

int ahci_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (!ahci_present || ahci_port_count == 0) return -1;
    if (count == 0 || count > AHCI_DATA_FRAME_SECTORS) return -1;
    return blk_submit_sync(ahci_ports[0].blockdev_id, lba, count, (void*)buf, BLK_REQ_WRITE);
}

#ifdef MODULE
/* Module entry/exit points — the ELF loader looks for these symbols */
int __init init_module(void) {
    return ahci_init();
}
void cleanup_module(void) {
    ahci_exit();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("AHCI SATA/NCQ driver with Port Multiplier support");
MODULE_ALIAS("pci:v00008086d00001E02sv*sd*bc*sc*i*");  /* Intel 7-series AHCI */
MODULE_ALIAS("pci:v00008086d00008C02sv*sd*bc*sc*i*");  /* Intel 8-series AHCI */
MODULE_ALIAS("pci:v00008086d00009C02sv*sd*bc*sc*i*");  /* Intel 9-series AHCI */
MODULE_ALIAS("pci:v00001022d00007801sv*sd*bc*sc*i*");  /* AMD Hudson AHCI */
MODULE_ALIAS("pci:v00001022d00007901sv*sd*bc*sc*i*");  /* AMD K15 AHCI */
MODULE_ALIAS("pci:v00001B21d00000612sv*sd*bc*sc*i*");  /* ASMedia 1062 AHCI */
MODULE_VERSION("1.0");
#endif /* MODULE */

/* Forward declaration for block-device layer API */
struct ahci_device;

int ahci_read(struct ahci_device *dev, uint64_t sector, void *buf, int count)
{
    (void)dev;
    if (sector > 0xFFFFFFFFULL) return -EOVERFLOW;
    return ahci_read_sectors((uint32_t)sector, (uint8_t)count, buf);
}

int ahci_write(struct ahci_device *dev, uint64_t sector, const void *buf, int count)
{
    (void)dev;
    if (sector > 0xFFFFFFFFULL) return -EOVERFLOW;
    return ahci_write_sectors((uint32_t)sector, (uint8_t)count, buf);
}

