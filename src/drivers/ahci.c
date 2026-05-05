/*
 * AHCI (Advanced Host Controller Interface) driver
 * Supports SATA devices on real hardware (e.g. ThinkPad X220)
 *
 * Implements PIO-based command submission using the AHCI HBA MMIO registers.
 * DMA targets use physical frames allocated from the PMM.
 */
#include "ahci.h"
#include "blockdev.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"

/* ── HBA register offsets ──────────────────────────────────────────────────── */
#define HBA_GHC_OFFSET  0x04   /* Global Host Control */
#define HBA_IS_OFFSET   0x08   /* Interrupt Status */
#define HBA_PI_OFFSET   0x0C   /* Ports Implemented */
#define HBA_VS_OFFSET   0x10   /* Version */
#define HBA_PORT_BASE   0x100  /* First port register block */
#define HBA_PORT_SIZE   0x80   /* Per-port register stride */

/* Port register offsets (relative to port base) */
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
#define PORT_SACT   0x34   /* SATA Active */
#define PORT_CI     0x38   /* Command Issue */

/* GHC bits */
#define GHC_AE      (1u << 31)  /* AHCI Enable */
#define GHC_HR      (1u << 0)   /* HBA Reset */

/* PORT_CMD bits */
#define PORT_CMD_ST     (1u << 0)   /* Start */
#define PORT_CMD_FRE    (1u << 4)   /* FIS Receive Enable */
#define PORT_CMD_FR     (1u << 14)  /* FIS Receive Running */
#define PORT_CMD_CR     (1u << 15)  /* Command List Running */

/* PORT_TFD bits */
#define PORT_TFD_BSY    (1u << 7)
#define PORT_TFD_DRQ    (1u << 3)
#define PORT_TFD_ERR    (1u << 0)

/* SATA Status (SSTS) detection */
#define SSTS_DET_MASK   0x0F
#define SSTS_DET_PRESENT 0x03  /* device present and phy established */

/* FIS types */
#define FIS_TYPE_REG_H2D  0x27  /* Register FIS – Host to Device */

/* ATA commands */
#define ATA_CMD_READ_DMA_EX   0x25
#define ATA_CMD_WRITE_DMA_EX  0x35
#define ATA_CMD_IDENTIFY      0xEC

/* ── Data structures ────────────────────────────────────────────────────────── */

/* Register FIS – Host to Device (20 bytes) */
struct fis_reg_h2d {
    uint8_t  fis_type;   /* FIS_TYPE_REG_H2D */
    uint8_t  pmport_c;   /* bit7=C (update command register) */
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
    uint16_t flags;      /* bit[4:0]=CFL (FIS length/4), bit5=ATAPI, bit6=W, bit7=P */
    uint16_t prdtl;      /* PRDT length (number of entries) */
    uint32_t prdbc;      /* PRD Byte Count (filled by HBA after transfer) */
    uint32_t ctba;       /* Command Table Base Address (low) */
    uint32_t ctbau;      /* Command Table Base Address (high) */
    uint32_t rsv[4];
} __attribute__((packed));

/* Physical Region Descriptor Table Entry (16 bytes) */
struct ahci_prdt_entry {
    uint32_t dba;        /* Data Base Address (low) */
    uint32_t dbau;       /* Data Base Address (high) */
    uint32_t rsv;
    uint32_t dbc;        /* Byte Count (0-based, bit31=interrupt on completion) */
} __attribute__((packed));

/* Command Table: FIS area + ACMD area + PRDT entries */
#define PRDT_MAX_ENTRIES  1
struct ahci_cmd_table {
    uint8_t  cfis[64];                          /* Command FIS */
    uint8_t  acmd[16];                          /* ATAPI command */
    uint8_t  rsv[48];
    struct ahci_prdt_entry prdt[PRDT_MAX_ENTRIES];
} __attribute__((packed));

/* Received FIS area (256 bytes per port) */
struct ahci_recv_fis {
    uint8_t dsfis[28];   /* DMA Setup FIS */
    uint8_t pad0[4];
    uint8_t psfis[20];   /* PIO Setup FIS */
    uint8_t pad1[12];
    uint8_t rfis[20];    /* D2H Register FIS */
    uint8_t pad2[4];
    uint8_t sdbfis[8];   /* Set Device Bits FIS */
    uint8_t ufis[64];    /* Unknown FIS */
    uint8_t rsv[96];
} __attribute__((packed));

/* ── Driver state ───────────────────────────────────────────────────────────── */
static int           ahci_present  = 0;
static uint64_t      hba_base      = 0;   /* MMIO base (virt == phys for identity map) */
static int           ahci_port     = -1;  /* first SATA port with a device */
static uint32_t      ahci_sectors  = 0;

/* Per-port DMA memory (allocated once, identity-mapped) */
static uint64_t      cmd_list_phys = 0;   /* 1 KB for 32 command headers */
static uint64_t      recv_fis_phys = 0;   /* 256 B for received FIS */
static uint64_t      cmd_tbl_phys  = 0;   /* command table (512 B enough for 1 PRDT) */
static uint64_t      data_phys     = 0;   /* 4 KB scratch data buffer */

/* ── MMIO helpers ───────────────────────────────────────────────────────────── */
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

/* ── I/O helpers ────────────────────────────────────────────────────────────── */
static void busy_wait(volatile int n) { while (n-- > 0) __asm__("pause"); }

/* ── Port stop/start ────────────────────────────────────────────────────────── */
static void port_stop(int p) {
    uint32_t cmd = port_read(p, PORT_CMD);
    cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    port_write(p, PORT_CMD, cmd);
    /* Wait for CR and FR to clear */
    int timeout = 50000;
    while ((port_read(p, PORT_CMD) & (PORT_CMD_CR | PORT_CMD_FR)) && --timeout)
        busy_wait(10);
}

static void port_start(int p) {
    /* Wait until CR is clear */
    int timeout = 50000;
    while ((port_read(p, PORT_CMD) & PORT_CMD_CR) && --timeout)
        busy_wait(10);
    uint32_t cmd = port_read(p, PORT_CMD);
    cmd |= PORT_CMD_FRE | PORT_CMD_ST;
    port_write(p, PORT_CMD, cmd);
}

/* ── Issue a command and wait for completion ────────────────────────────────── */
static int port_issue_cmd(int p, int slot) {
    /* Clear errors */
    port_write(p, PORT_SERR, port_read(p, PORT_SERR));
    port_write(p, PORT_IS,   port_read(p, PORT_IS));

    /* Issue command */
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
    if (timeout == 0) return -2;  /* timeout */
    return 0;
}

/* ── Build a command in slot 0 ──────────────────────────────────────────────── */
static void build_cmd(uint8_t ata_cmd, uint32_t lba, uint8_t count,
                      uint64_t data_phys_addr, int is_write, int fis_len_dw) {
    /* Command Header (slot 0) */
    struct ahci_cmd_hdr *hdr = (struct ahci_cmd_hdr *)cmd_list_phys;
    memset(hdr, 0, sizeof(*hdr));
    hdr->flags  = (uint16_t)fis_len_dw;           /* CFL = FIS length in DW */
    if (is_write) hdr->flags |= (1u << 6);          /* W bit */
    hdr->prdtl  = 1;
    hdr->ctba   = (uint32_t)(cmd_tbl_phys & 0xFFFFFFFF);
    hdr->ctbau  = (uint32_t)(cmd_tbl_phys >> 32);

    /* Command Table */
    struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)cmd_tbl_phys;
    memset(tbl, 0, sizeof(*tbl));

    /* Command FIS */
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;               /* C=1: update command register */
    fis->command  = ata_cmd;
    fis->device   = (1u << 6);          /* LBA mode */
    fis->lba0 = (uint8_t)(lba >>  0);
    fis->lba1 = (uint8_t)(lba >>  8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = 0;
    fis->lba5 = 0;
    fis->countl = count;
    fis->counth = 0;

    /* PRDT entry */
    tbl->prdt[0].dba  = (uint32_t)(data_phys_addr & 0xFFFFFFFF);
    tbl->prdt[0].dbau = (uint32_t)(data_phys_addr >> 32);
    tbl->prdt[0].rsv  = 0;
    /* byte count is 0-based; bit31 = interrupt on completion */
    tbl->prdt[0].dbc  = (uint32_t)count * AHCI_SECTOR_SIZE - 1;
}

/* ── Public API ─────────────────────────────────────────────────────────────── */
int ahci_is_present(void) { return ahci_present; }
uint32_t ahci_get_sectors(void) { return ahci_sectors; }

int ahci_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (!ahci_present || ahci_port < 0) return -1;
    if (count == 0) return 0;

    port_stop(ahci_port);
    build_cmd(ATA_CMD_READ_DMA_EX, lba, count, data_phys, 0,
              sizeof(struct fis_reg_h2d) / 4);
    port_start(ahci_port);

    int rc = port_issue_cmd(ahci_port, 0);
    if (rc == 0)
        memcpy(buf, (void *)data_phys, (size_t)count * AHCI_SECTOR_SIZE);
    return rc;
}

int ahci_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (!ahci_present || ahci_port < 0) return -1;
    if (count == 0) return 0;

    memcpy((void *)data_phys, buf, (size_t)count * AHCI_SECTOR_SIZE);
    port_stop(ahci_port);
    build_cmd(ATA_CMD_WRITE_DMA_EX, lba, count, data_phys, 1,
              sizeof(struct fis_reg_h2d) / 4);
    port_start(ahci_port);

    return port_issue_cmd(ahci_port, 0);
}

/* ── Initialization ─────────────────────────────────────────────────────────── */
int ahci_init(void) {
    /* Find AHCI controller: PCI class 0x01 subclass 0x06 (SATA) */
    uint16_t found_vid = 0, found_did = 0;
    uint8_t  found_bus = 0, found_slot = 0;
    uint32_t found_bar5 = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t r0 = pci_read(bus, slot, 0, 0);
            if ((r0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t r2 = pci_read(bus, slot, 0, 0x08);
            uint8_t cls = (r2 >> 24) & 0xFF;
            uint8_t sub = (r2 >> 16) & 0xFF;
            if (cls == 0x01 && sub == 0x06) {
                found_vid  = r0 & 0xFFFF;
                found_did  = (r0 >> 16) & 0xFFFF;
                found_bus  = (uint8_t)bus;
                found_slot = (uint8_t)slot;
                found_bar5 = pci_read(bus, slot, 0, 0x24); /* BAR5 = ABAR */
                goto found;
            }
        }
    }
    return -1; /* no AHCI controller */

found:
    hba_base = (uint64_t)(found_bar5 & ~0xFU);
    if (hba_base == 0) return -2;

    kprintf("  AHCI %04x:%04x HBA@0x%x\n",
            (uint64_t)found_vid, (uint64_t)found_did, hba_base);

    /* Enable bus master on the AHCI controller */
    {
        struct pci_device dev;
        dev.bus = found_bus; dev.slot = found_slot; dev.func = 0;
        pci_enable_bus_master(&dev);
    }

    /* Enable AHCI mode */
    uint32_t ghc = hba_read(HBA_GHC_OFFSET);
    hba_write(HBA_GHC_OFFSET, ghc | GHC_AE);

    /* Allocate DMA memory (identity-mapped in first 1 GB) */
    cmd_list_phys = pmm_alloc_frame();   /* 4 KB frame for command list (1KB used) */
    recv_fis_phys = pmm_alloc_frame();   /* 4 KB frame for received FIS (256B used) */
    cmd_tbl_phys  = pmm_alloc_frame();   /* 4 KB frame for command table */
    data_phys     = pmm_alloc_frame();   /* 4 KB data scratch */

    if (!cmd_list_phys || !recv_fis_phys || !cmd_tbl_phys || !data_phys)
        return -3;

    memset((void *)cmd_list_phys, 0, 4096);
    memset((void *)recv_fis_phys, 0, 4096);
    memset((void *)cmd_tbl_phys,  0, 4096);
    memset((void *)data_phys,     0, 4096);

    /* Scan implemented ports */
    uint32_t pi = hba_read(HBA_PI_OFFSET);
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t ssts = port_read(p, PORT_SSTS);
        uint8_t  det  = ssts & SSTS_DET_MASK;
        if (det != SSTS_DET_PRESENT) continue;

        uint32_t sig = port_read(p, PORT_SIG);
        if (sig != AHCI_SIG_ATA) continue;   /* only plain SATA drives */

        /* Set up command list and FIS base for this port */
        port_stop(p);
        port_write(p, PORT_CLB,  (uint32_t)(cmd_list_phys & 0xFFFFFFFF));
        port_write(p, PORT_CLBU, (uint32_t)(cmd_list_phys >> 32));
        port_write(p, PORT_FB,   (uint32_t)(recv_fis_phys & 0xFFFFFFFF));
        port_write(p, PORT_FBU,  (uint32_t)(recv_fis_phys >> 32));
        /* Clear errors */
        port_write(p, PORT_SERR, 0xFFFFFFFF);
        port_write(p, PORT_IS,   0xFFFFFFFF);
        port_start(p);

        /* IDENTIFY to get sector count */
        memset((void *)data_phys, 0, 512);
        build_cmd(ATA_CMD_IDENTIFY, 0, 1, data_phys, 0,
                  sizeof(struct fis_reg_h2d) / 4);
        /* IDENTIFY uses count=0 in FIS */
        {
            struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)cmd_tbl_phys;
            struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
            fis->countl = 0;
            /* PRDT byte count: 512 - 1 */
            tbl->prdt[0].dbc = 511;
        }
        if (port_issue_cmd(p, 0) == 0) {
            uint16_t *id = (uint16_t *)data_phys;
            /* Words 100-103: 48-bit total LBA count */
            uint32_t lo = ((uint32_t)id[101] << 16) | id[100];
            ahci_sectors = lo ? lo : ((uint32_t)id[57] | ((uint32_t)id[58] << 16));
            kprintf("  AHCI port %d: %u sectors (%u MB)\n",
                    (uint64_t)p, (uint64_t)ahci_sectors,
                    (uint64_t)(ahci_sectors / 2048));
        }

        ahci_port    = p;
        ahci_present = 1;
            blockdev_register(BLOCKDEV_AHCI, "ahci", ahci_read_sectors, ahci_write_sectors, ahci_get_sectors);
        break;
    }

    if (!ahci_present) return -4;
    return 0;
}
