/*
 * AHCI (Advanced Host Controller Interface) driver with NCQ
 * Supports SATA devices with multi-slot interrupt-driven I/O.
 *
 * Uses Native Command Queuing (NCQ) for up to 31 concurrent commands.
 * Slot 0 is reserved for non-NCQ commands (IDENTIFY, etc.).
 * Slots 1-31 are used for NCQ READ/WRITE FPDMA QUEUED.
 */
#include "ahci.h"
#include "blockdev.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "idt.h"
#include "apic.h"
#include "pic.h"

/* ── HBA register offsets ───────────────────────────────────────────── */
#define HBA_GHC_OFFSET   0x04   /* Global Host Control */
#define HBA_IS_OFFSET    0x08   /* Interrupt Status */
#define HBA_PI_OFFSET    0x0C   /* Ports Implemented */
#define HBA_VS_OFFSET    0x10   /* Version */
#define HBA_PORT_BASE    0x100  /* First port register block */
#define HBA_PORT_SIZE    0x80   /* Per-port register stride */

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
#define ATA_CMD_READ_DMA_EX      0x25
#define ATA_CMD_WRITE_DMA_EX     0x35
#define ATA_CMD_READ_FPDMA_QUEUED  0x60
#define ATA_CMD_WRITE_FPDMA_QUEUED 0x61
#define ATA_CMD_IDENTIFY         0xEC

/* ── Constants ──────────────────────────────────────────────────────── */
#define AHCI_SLOT_COUNT    32
#define AHCI_NCQ_SLOTS     (AHCI_SLOT_COUNT - 1)  /* 31 NCQ slots */
#define AHCI_PRDT_ENTRIES  1
#define AHCI_DATA_FRAME_SECTORS 8  /* 4KB data buffer = 8 sectors */

/* ── Data structures ────────────────────────────────────────────────── */

/* Register FIS – Host to Device (20 bytes) */
struct fis_reg_h2d {
    uint8_t  fis_type;
    uint8_t  pmport_c;
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
    struct blk_request *req;      /* current request, NULL if free */
    uint64_t            cmd_tbl_phys;    /* physical address of command table */
    void               *cmd_tbl_virt;    /* kernel virtual address of command table */
    uint64_t            data_buf_phys;   /* physical address of data buffer (4KB) */
    void               *data_buf_virt;   /* kernel virtual address of data buffer */
};

/* Per-port state */
struct ahci_port {
    int                 present;
    int                 port_num;
    uint32_t            sector_count;
    int                 ncq_capable;     /* word 76 bit 8 */
    int                 blockdev_id;
    uint8_t             irq_line;
    struct ahci_slot    slots[AHCI_SLOT_COUNT];
    uint64_t            cmd_list_phys;
    uint64_t            recv_fis_phys;
    uint32_t            inflight_mask;   /* bits set for issued slots */
};

/* ── Driver state ───────────────────────────────────────────────────── */
static int              ahci_present   = 0;
static uint64_t         hba_base       = 0;
static struct ahci_port ahci_ports[32]; /* max 32 ports */
static int              ahci_port_count = 0;
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

/* ── Slot management ────────────────────────────────────────────────── */
static int ahci_find_free_slot(struct ahci_port *port) {
    /* Check CI (commands in progress) and SACT (NCQ active).
     * A free slot has bits clear in both. For NCQ we use slots 1..31. */
    uint32_t ci   = port_read(port->port_num, PORT_CI);
    uint32_t sact = port_read(port->port_num, PORT_SACT);
    uint32_t busy = ci | sact;

    /* Try NCQ slots (1-31) first */
    for (int i = 1; i < AHCI_SLOT_COUNT; i++) {
        if (!(busy & (1u << i))) return i;
    }
    /* Fall back to slot 0 for non-NCQ commands */
    if (!(busy & 1u)) return 0;
    return -1;
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

    /* Command FIS */
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;  /* C=1 */
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
    fis->pmport_c = 0x80;
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
        } else {
            /* NCQ: build and issue asynchronously */
            ahci_build_ncq_cmd(port, slot, req);
            port->inflight_mask |= (1u << slot);
            spinlock_irqsave_release(&ahci_lock, irq_flags);

            ahci_issue_ncq(port, slot);
        }
    }
}

/* ── Interrupt handler ──────────────────────────────────────────────── */
static void ahci_irq_handler(struct interrupt_frame *frame) {
    (void)frame;

    uint32_t is = hba_read(HBA_IS_OFFSET);
    if (!is) return;

    for (int p = 0; p < 32; p++) {
        if (!(is & (1u << p))) continue;
        struct ahci_port *port = &ahci_ports[p];
        if (!port->present) continue;

        uint32_t port_is = port_read(p, PORT_IS);
        uint32_t tfd = port_read(p, PORT_TFD);

        /* Clear interrupt status */
        port_write(p, PORT_IS, port_is);
        hba_write(HBA_IS_OFFSET, (1u << p));

        if (port_is & PORT_IS_ERR) {
            /* Error interrupt — read SERR to see what happened */
            uint32_t serr = port_read(p, PORT_SERR);
            port_write(p, PORT_SERR, serr);
            kprintf("AHCI port %d error: IS=0x%x TFD=0x%x SERR=0x%x\n",
                    (uint64_t)p, (uint64_t)port_is, (uint64_t)tfd, (uint64_t)serr);
        }

        if (port_is & PORT_IS_SDBS) {
            /* NCQ completion: check which slots completed */
            uint32_t ci   = port_read(p, PORT_CI);
            uint32_t sact = port_read(p, PORT_SACT);
            uint32_t done = port->inflight_mask & ~(ci | sact);

            for (int slot = 1; slot < AHCI_SLOT_COUNT; slot++) {
                if (done & (1u << slot)) {
                    struct ahci_slot *s = &port->slots[slot];
                    struct blk_request *req = s->req;
                    if (req) {
                        s->req = NULL;
                        port->inflight_mask &= ~(1u << slot);

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

        if (port_is & PORT_IS_D2H) {
            /* Non-NCQ D2H completion — handled inline in ahci_issue_non_ncq */
        }

        /* Drain request queue to submit more commands */
        ahci_drain_queue(port);
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

/* ── Initialization ─────────────────────────────────────────────────── */
int ahci_init(void) {
    struct pci_device dev;
    if (pci_find_class(0x01, 0x06, &dev) < 0)
        return -1; /* no AHCI controller */

    uint64_t bar5 = dev.bar[5] & ~0xFULL;
    kprintf("  AHCI %04x:%04x (IRQ %u) HBA@0x%x\n",
            (uint64_t)dev.vendor_id, (uint64_t)dev.device_id,
            (uint64_t)dev.irq, bar5);

    hba_base = bar5;
    if (hba_base == 0) return -2;

    /* Map HBA MMIO in high-half VMA space */
    hba_base = (uint64_t)vmm_map_phys(hba_base, 0x2000,
                VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    if (!hba_base) return -2;

    /* Enable PCI bus mastering */
    pci_enable_bus_master(&dev);

    /* Enable AHCI mode and HBA interrupt */
    uint32_t ghc = hba_read(HBA_GHC_OFFSET);
    hba_write(HBA_GHC_OFFSET, ghc | GHC_AE | GHC_IE);

    spinlock_init(&ahci_lock);

    /* Scan implemented ports */
    uint32_t pi = hba_read(HBA_PI_OFFSET);
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t ssts = port_read(p, PORT_SSTS);
        uint8_t  det  = ssts & SSTS_DET_MASK;
        if (det != SSTS_DET_PRESENT) continue;

        uint32_t sig = port_read(p, PORT_SIG);
        if (sig != AHCI_SIG_ATA) continue;

        /* Initialize port structure */
        struct ahci_port *port = &ahci_ports[ahci_port_count];
        memset(port, 0, sizeof(*port));
        port->present = 1;
        port->port_num = p;
        port->irq_line = dev.irq;
        port->inflight_mask = 0;

        /* Allocate command list (1 frame = 32 × 32-byte headers) */
        port->cmd_list_phys = pmm_alloc_frame();
        if (!port->cmd_list_phys) continue;
        memset(PHYS_TO_VIRT(port->cmd_list_phys), 0, 4096);

        /* Allocate FIS receive area (1 frame) */
        port->recv_fis_phys = pmm_alloc_frame();
        if (!port->recv_fis_phys) continue;
        memset(PHYS_TO_VIRT(port->recv_fis_phys), 0, 4096);

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
            slot->data_buf_phys = pmm_alloc_frame();
            if (!slot->data_buf_phys) { alloc_ok = 0; break; }
            slot->data_buf_virt = PHYS_TO_VIRT(slot->data_buf_phys);
            memset(slot->data_buf_virt, 0, 4096);
        }
        if (!alloc_ok) continue;

        port_stop(p);
        port_write(p, PORT_CLB,  (uint32_t)(port->cmd_list_phys & 0xFFFFFFFF));
        port_write(p, PORT_CLBU, (uint32_t)(port->cmd_list_phys >> 32));
        port_write(p, PORT_FB,   (uint32_t)(port->recv_fis_phys & 0xFFFFFFFF));
        port_write(p, PORT_FBU,  (uint32_t)(port->recv_fis_phys >> 32));
        port_write(p, PORT_SERR, 0xFFFFFFFF);
        port_write(p, PORT_IS,   0xFFFFFFFF);

        /* Enable interrupts — mask SDBS and D2H */
        port_write(p, PORT_IE, PORT_IS_D2H | PORT_IS_SDBS | PORT_IS_PCS | PORT_IS_ERR);
        port_start(p);

        /* IDENTIFY device */
        {
            uint64_t identify_data = pmm_alloc_frame();
            if (!identify_data) continue;
            memset(PHYS_TO_VIRT(identify_data), 0, 4096);

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
                port->sector_count = lo ? lo : ((uint32_t)id[57] | ((uint32_t)id[58] << 16));

                /* Check NCQ support: word 76 bits 8 = NCQ queue depth > 0 */
                if (id[76] & (1u << 8)) {
                    port->ncq_capable = 1;
                }

                kprintf("  AHCI port %d: %u sectors (%u MB)%s\n",
                        (uint64_t)p, (uint64_t)port->sector_count,
                        (uint64_t)(port->sector_count / 2048),
                        port->ncq_capable ? " NCQ" : "");

                pmm_free_frame(identify_data);

                /* Register with block device layer */
                int bd_id = BLOCKDEV_AHCI + ahci_port_count;
                int ret = blockdev_register(bd_id, "ahci",
                                            ahci_submit_fn,
                                            ahci_idle_fn,
                                            port->sector_count,
                                            BLK_DRIVER_ASYNC);
                if (ret == 0) {
                    port->blockdev_id = bd_id;
                    ahci_port_count++;
                    ahci_present = 1;
                }
            } else {
                pmm_free_frame(identify_data);
                continue;
            }
        }
    }

    if (!ahci_present) return -4;

    /* Register IRQ handler for the first AHCI port's IRQ line */
    idt_register_handler(32 + ahci_ports[0].irq_line, ahci_irq_handler);
    if (apic_is_init_complete())
        ioapic_unmask_irq(ahci_ports[0].irq_line);
    pic_unmask(ahci_ports[0].irq_line);

    kprintf("  AHCI: %d port(s) active, NCQ depth %d\n",
            (uint64_t)ahci_port_count, (uint64_t)AHCI_NCQ_SLOTS);
    return 0;
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
