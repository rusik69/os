#include "ata.h"

#include "blockdev.h"
#include "io.h"
#include "printf.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── ATA PIO Transfer Model ───────────────────────────────────────────
 *
 * This driver implements Programmed I/O (PIO) on the primary IDE bus
 * (master drive).  PIO is the simplest ATA transfer mode: the CPU
 * directly reads/writes data registers on the device using in/out
 * instructions.  No DMA (Direct Memory Access) is used — the CPU
 * manages every word transfer.
 *
 * ── Register Map (Primary Bus, 0x1F0–0x1F7) ───────────────────────
 *
 *    Offset  Register        Access  Description
 *    ──────  ─────────────── ──────  ───────────────────────────────
 *    0x1F0   DATA            R/W     Data port (16-bit, 256 words/sector)
 *    0x1F1   ERROR / FEAT    R/W     Error (read) / Features (write)
 *    0x1F2   SECTOR COUNT    R/W     Number of sectors to transfer (1–255)
 *    0x1F3   LBA LOW         R/W     LBA bits 7:0
 *    0x1F4   LBA MID         R/W     LBA bits 15:8
 *    0x1F5   LBA HIGH        R/W     LBA bits 23:16
 *    0x1F6   DRIVE/HEAD      R/W     Drive sel | LBA bits 27:24 | 0xE0
 *    0x1F7   STATUS / CMD    R/W     Status (read) / Command (write)
 *
 * ── Status Register Bits ────────────────────────────────────────────
 *
 *    Bit 7 (BSY)   :  Device is busy — no other bits valid
 *    Bit 6 (DRDY)  :  Drive ready for commands
 *    Bit 5 (DF)    :  Device fault
 *    Bit 3 (DRQ)   :  Data request — ready to transfer a word
 *    Bit 0 (ERR)   :  Error — check error register
 *
 * ── PIO Read Protocol (ata_read_sectors) ────────────────────────────
 *
 *    1.  Wait for BSY to clear (ata_wait_bsy)
 *    2.  Program the drive/head, sector count, and LBA registers
 *    3.  Write the READ PIO command (0x20)
 *    4.  Wait 400ns (four reads of status register)
 *    5.  For each sector:
 *        a.  Wait for BSY to clear
 *        b.  Check for ERR
 *        c.  Wait for DRQ to assert (data ready)
 *        d.  Read 256 words from the DATA port (512 bytes)
 *    6.  Return 0 on success, -1 on any error
 *
 * ── PIO Write Protocol (ata_write_sectors) ──────────────────────────
 *
 *    1.  Wait for BSY to clear
 *    2.  Program drive/head, sector count, and LBA registers
 *    3.  Write the WRITE PIO command (0x30)
 *    4.  Wait 400ns
 *    5.  For each sector:
 *        a.  Wait for BSY to clear
 *        b.  Wait for DRQ to assert (device ready to receive)
 *        c.  Write 256 words to the DATA port (512 bytes)
 *    6.  Issue FLUSH CACHE command (0xE7) and wait for BSY
 *    7.  Return 0 on success, -1 on any error
 *
 * ── DMA Notes ───────────────────────────────────────────────────────
 *
 *    This driver is PIO-only.  DMA commands (0xC8 READ DMA, 0xCA WRITE
 *    DMA) are defined in ata_pio.h but not used here.  DMA would require
 *    bus-mastering IDE support via the PCI bus master BAR (typically at
 *    PCI function 0x20) and a scatter-gather descriptor table, enabling
 *    the controller to transfer data directly to/from memory without CPU
 *    intervention.  The PIO approach is simpler and sufficient for the
 *    legacy primary master drive.
 *
 * ── Redirect Mechanism ──────────────────────────────────────────────
 *
 *    When ata_set_redirect() is called (e.g. by the ramdisk driver),
 *    ata_read_sectors and ata_write_sectors delegate to the provided
 *    callbacks instead of touching real ATA hardware.  This allows
 *    the filesystem layer to work identically on a ramdisk or real
 *    disk without conditional code.
 *
 * ───────────────────────────────────────────────────────────────────*/

/* Optional redirect: if set, ata_read_sectors/write_sectors delegate
 * to these callbacks instead of real ATA PIO.  Used by the ramdisk
 * driver so that fs.c works without ATA hardware. */
static int (*redirect_read)(uint32_t lba, uint8_t count, void *buf) = NULL;
static int (*redirect_write)(uint32_t lba, uint8_t count, const void *buf) = NULL;

void ata_set_redirect(int (*read_fn)(uint32_t, uint8_t, void *),
                      int (*write_fn)(uint32_t, uint8_t, const void *)) {
    redirect_read = read_fn;
    redirect_write = write_fn;
}

/* Primary ATA bus ports */
#define ATA_DATA 0x1F0
#define ATA_ERROR 0x1F1
#define ATA_SECT_CNT 0x1F2
#define ATA_LBA_LO 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI 0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_COMMAND 0x1F7

/* Status bits */
#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

/* Commands */
#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH 0xE7

static int ata_present = 0;
static uint32_t ata_total_sectors = 0;

/* Wait for BSY bit to clear (device ready for next command).
 * Polls the status register in a tight loop with a PAUSE hint,
 * timing out after ~10 million iterations.  Returns 0 on success,
 * -1 if the device hangs. */
static int ata_wait_bsy(void) {
    int timeout = 10000000;
    while ((inb(ATA_STATUS) & ATA_SR_BSY) && --timeout > 0)
        __asm__ volatile("pause");
    return timeout > 0 ? 0 : -1;
}

/* Wait for DRQ bit to be set (device has data ready to transfer, or
 * is ready to accept data).  Same polling strategy as ata_wait_bsy.
 * Returns 0 on success, -1 on timeout. */
static int ata_wait_drq(void) {
    int timeout = 10000000;
    while (!(inb(ATA_STATUS) & ATA_SR_DRQ) && --timeout > 0)
        __asm__ volatile("pause");
    return timeout > 0 ? 0 : -1;
}

/* Perform the ATA-specified 400ns recovery delay by reading the
 * alternate status register four times.  Required between PIO
 * commands to give the device time to update its status bits. */
static void ata_400ns_delay(void) {
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
}

/* Initialize the primary ATA master drive.
 *
 * Sequence:
 *   1. Select master drive (0xA0 for master on primary bus)
 *   2. Wait 400ns
 *   3. Send IDENTIFY DEVICE command (0xEC)
 *   4. Wait for BSY to clear, check for non-ATA signature
 *   5. Wait for DRQ, then read 256 words of IDENTIFY data
 *   6. Extract total sector count from words 60-61
 *   7. Register with the block device layer
 *
 * If no ATA hardware is present (status == 0) or the device is
 * non-ATA (ATAPI), ata_present remains 0 and the driver is inert. */
void __init ata_init(void) {
    static int ata_inited = 0;
    if (ata_inited)
        return; /* already probed (device_initcall + direct call from kernel.c) */
    ata_inited = 1;

    /* Select master drive */
    outb(ATA_DRIVE_HEAD, 0xA0);
    ata_400ns_delay();

    /* Send IDENTIFY */
    outb(ATA_SECT_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) {
        ata_present = 0;
        return;
    }

    if (ata_wait_bsy() < 0) {
        ata_present = 0;
        return;
    }

    /* Check for non-ATA */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) {
        ata_present = 0;
        return;
    }

    /* Wait for DRQ or ERR */
    for (int timeout = 0; timeout < 100000; timeout++) {
        status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) {
            ata_present = 0;
            return;
        }
        if (status & ATA_SR_DRQ)
            break;
        __asm__ volatile("pause");
    }
    if (!(status & ATA_SR_DRQ)) {
        ata_present = 0;
        return;
    }

    /* Read identify data (256 words) */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = inw(ATA_DATA);

    ata_present = 1;

    uint32_t sectors = identify[60] | ((uint32_t)identify[61] << 16);
    ata_total_sectors = sectors;
    blockdev_register_legacy(BLOCKDEV_ATA, "ata", ata_read_sectors, ata_write_sectors,
                             ata_get_sectors);
    kprintf("  ATA disk: %llu sectors (%llu MB)\n", (unsigned long long)sectors,
            (unsigned long long)(sectors / 2048));
}
#include "initcall.h"
device_initcall(ata_init);

/* Return 1 if ATA hardware (or a redirect) is available.
 * Used by the block device layer and fs.c to decide whether
 * to probe the disk. */
int ata_is_present(void) {
    return ata_present || redirect_read;
}

/* Return the total number of 512-byte sectors on the disk.
 * Populated during ata_init() from IDENTIFY words 60-61.
 * Returns 0 if no ATA hardware was found. */
uint32_t ata_get_sectors(void) {
    return ata_total_sectors;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    /* If a redirect is active (ramdisk mode), use it instead of real ATA */
    if (redirect_read)
        return redirect_read(lba, count, buf);
    if (!ata_present)
        return -1;

    /* ── Debug: trace module-read region (stall root-cause) ── */
    if ((lba >= 8600 && lba < 9000) || lba < 100)
        kprintf("[ata] read lba=%u count=%u\n", (unsigned int)lba, (unsigned int)count);

    /* Validate LBA range before issuing ATA command */
    if (count == 0)
        return -1; /* must read at least 1 sector */
    if (lba >= ata_total_sectors)
        return -1; /* start LBA out of range */
    if (lba + count < lba)
        return -1; /* uint32_t overflow check */
    if (lba + count > ata_total_sectors)
        return -1; /* end exceeds disk size */

    if (ata_wait_bsy() < 0)
        return -1;
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECT_CNT, count);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);

    uint8_t *ptr = (uint8_t *)buf;
    for (int s = 0; s < count; s++) {
        ata_400ns_delay();
        if (ata_wait_bsy() < 0)
            return -1;
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR)
            return -1;
        if (ata_wait_drq() < 0)
            return -1;
        for (int i = 0; i < 256; i++) {
            uint16_t word = inw(ATA_DATA);
            __builtin_memcpy(ptr + i * 2, &word, 2);
        }
        ptr += ATA_SECTOR_SIZE;
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    /* If a redirect is active (ramdisk mode), use it instead of real ATA */
    if (redirect_write)
        return redirect_write(lba, count, buf);
    if (!ata_present)
        return -1;

    /* Validate LBA range before issuing ATA command */
    if (count == 0)
        return -1; /* must write at least 1 sector */
    if (lba >= ata_total_sectors)
        return -1; /* start LBA out of range */
    if (lba + count < lba)
        return -1; /* uint32_t overflow check */
    if (lba + count > ata_total_sectors)
        return -1; /* end exceeds disk size */

    if (ata_wait_bsy() < 0)
        return -1;
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECT_CNT, count);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);

    const uint8_t *ptr = (const uint8_t *)buf;
    for (int s = 0; s < count; s++) {
        ata_400ns_delay();
        if (ata_wait_bsy() < 0)
            return -1;
        if (ata_wait_drq() < 0)
            return -1;
        for (int i = 0; i < 256; i++) {
            uint16_t word;
            __builtin_memcpy(&word, ptr + i * 2, 2);
            outw(ATA_DATA, word);
        }
        ptr += ATA_SECTOR_SIZE;
    }

    /* Flush cache */
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    if (ata_wait_bsy() < 0)
        return -1;
    return 0;
}

#ifdef MODULE
/*
 * Module entry/exit points — the ELF module loader looks for these symbols.
 * When compiled as a loadable module (.ko), init_module calls ata_init();
 * when built into the kernel, the boot code calls ata_init() directly.
 */
int init_module(void) {
    ata_init();
    return 0; /* ata_init never fails — it just sets ata_present=0 if no HW */
}

void cleanup_module(void) {
    /* Unregister from the block device layer */
    blockdev_unregister(BLOCKDEV_ATA);
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Legacy ATA PIO driver — primary IDE controller (master)");
MODULE_ALIAS("ata");
#endif /* MODULE */

static int ata_identify(void *ident_data) {
    if (!ident_data)
        return -EINVAL;
    if (!ata_present)
        return -ENODEV;
    if (ata_wait_bsy() < 0)
        return -EIO;
    outb(ATA_DRIVE_HEAD, 0xA0);
    ata_400ns_delay();
    outb(ATA_SECT_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();
    uint8_t status = inb(ATA_STATUS);
    if (status == 0)
        return -ENODEV;
    if (ata_wait_bsy() < 0)
        return -EIO;
    for (int timeout = 0; timeout < 100000; timeout++) {
        status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR)
            return -EIO;
        if (status & ATA_SR_DRQ)
            break;
        __asm__ volatile("pause");
    }
    if (!(status & ATA_SR_DRQ))
        return -EIO;
    uint16_t *ident = (uint16_t *)ident_data;
    for (int i = 0; i < 256; i++)
        ident[i] = inw(ATA_DATA);
    kprintf("[ATA] IDENTIFY successful\n");
    return 0;
}

static int ata_reset(int bus) {
    (void)bus;
    kprintf("[ATA] Soft resetting ATA bus\n");
    if (bus == 0) {
        outb(0x3F6, 0x04);
        ata_400ns_delay();
        /* small delay */
        for (volatile int _d = 0; _d < 5000; _d++)
            io_wait();
        outb(0x3F6, 0x00);
        ata_400ns_delay();
        if (ata_wait_bsy() < 0) {
            kprintf("[ATA] Reset failed\n");
            return -EIO;
        }
        ata_present = 0;
        return 0;
    } else if (bus == 1) {
        outb(0x376, 0x04);
        ata_400ns_delay();
        for (volatile int _dl = 0; _dl < 5000; _dl++)
            io_wait();
        outb(0x376, 0x00);
        ata_400ns_delay();
        return 0;
    }
    return -EINVAL;
}
