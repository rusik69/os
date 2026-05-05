#include "ata.h"
#include "blockdev.h"
#include "io.h"
#include "printf.h"

/* Primary ATA bus ports */
#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECT_CNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7

/* Status bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Commands */
#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_FLUSH     0xE7

static int ata_present = 0;
static uint32_t ata_total_sectors = 0;

static void ata_wait_bsy(void) {
    while (inb(ATA_STATUS) & ATA_SR_BSY);
}

static void ata_wait_drq(void) {
    while (!(inb(ATA_STATUS) & ATA_SR_DRQ));
}

static void ata_400ns_delay(void) {
    inb(ATA_STATUS); inb(ATA_STATUS);
    inb(ATA_STATUS); inb(ATA_STATUS);
}

void ata_init(void) {
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

    ata_wait_bsy();

    /* Check for non-ATA */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) {
        ata_present = 0;
        return;
    }

    /* Wait for DRQ or ERR */
    while (1) {
        status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) { ata_present = 0; return; }
        if (status & ATA_SR_DRQ) break;
    }

    /* Read identify data (256 words) */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = inw(ATA_DATA);

    ata_present = 1;

    uint32_t sectors = identify[60] | ((uint32_t)identify[61] << 16);
    ata_total_sectors = sectors;
    blockdev_register(BLOCKDEV_ATA, "ata", ata_read_sectors, ata_write_sectors, ata_get_sectors);
    kprintf("  ATA disk: %u sectors (%u MB)\n", (uint64_t)sectors,
            (uint64_t)(sectors / 2048));
}

int ata_is_present(void) {
    return ata_present;
}

uint32_t ata_get_sectors(void) {
    return ata_total_sectors;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (!ata_present) return -1;

    ata_wait_bsy();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECT_CNT, count);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        ata_400ns_delay();
        ata_wait_bsy();
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) return -1;
        ata_wait_drq();
        for (int i = 0; i < 256; i++)
            *ptr++ = inw(ATA_DATA);
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (!ata_present) return -1;

    ata_wait_bsy();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECT_CNT, count);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        ata_400ns_delay();
        ata_wait_bsy();
        ata_wait_drq();
        for (int i = 0; i < 256; i++)
            outw(ATA_DATA, *ptr++);
    }

    /* Flush cache */
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    ata_wait_bsy();
    return 0;
}
