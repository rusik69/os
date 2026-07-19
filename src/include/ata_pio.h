#ifndef ATA_PIO_H
#define ATA_PIO_H

#include "types.h"

/* ── ATA PIO Bus I/O Ports (Primary / Secondary) ─────────────────── */

#define ATA_PIO_DATA(bus)         ((bus) == 0 ? 0x1F0 : 0x170)
#define ATA_PIO_ERROR(bus)        ((bus) == 0 ? 0x1F1 : 0x171)
#define ATA_PIO_SECT_CNT(bus)     ((bus) == 0 ? 0x1F2 : 0x172)
#define ATA_PIO_LBA_LO(bus)       ((bus) == 0 ? 0x1F3 : 0x173)
#define ATA_PIO_LBA_MID(bus)      ((bus) == 0 ? 0x1F4 : 0x174)
#define ATA_PIO_LBA_HI(bus)       ((bus) == 0 ? 0x1F5 : 0x175)
#define ATA_PIO_DRIVE_HEAD(bus)   ((bus) == 0 ? 0x1F6 : 0x176)
#define ATA_PIO_COMMAND(bus)      ((bus) == 0 ? 0x1F7 : 0x177)
#define ATA_PIO_STATUS(bus)       ((bus) == 0 ? 0x1F7 : 0x177)
#define ATA_PIO_ALT_STATUS(bus)   ((bus) == 0 ? 0x3F6 : 0x376)
#define ATA_PIO_DEV_CTRL(bus)     ((bus) == 0 ? 0x3F6 : 0x376)

/* ── Status Register Bits ─────────────────────────────────────────── */

#define ATA_SR_BSY   0x80  /* Busy — device is processing a command */
#define ATA_SR_DRDY  0x40  /* Drive Ready — device is ready to accept commands */
#define ATA_SR_DF    0x20  /* Device Fault — unrecovered error */
#define ATA_SR_DRQ   0x08  /* Data Request — ready to transfer a word */
#define ATA_SR_ERR   0x01  /* Error — previous command ended with error */

/* Error register bits */
#define ATA_ER_AMNF  0x01  /* Address mark not found */
#define ATA_ER_TKZNF 0x02  /* Track zero not found */
#define ATA_ER_ABRT  0x04  /* Aborted command */
#define ATA_ER_MCR   0x08  /* Media change request */
#define ATA_ER_IDNF  0x10  /* ID not found */
#define ATA_ER_MC    0x20  /* Media changed */
#define ATA_ER_UNC   0x40  /* Uncorrectable data error */
#define ATA_ER_BBK   0x80  /* Bad block detected */

/* Device Control register bits */
#define ATA_DC_SRST  0x04  /* Software Reset */
#define ATA_DC_NIEN  0x02  /* Disable interrupts */

/* ── Standard ATA Commands (28-bit LBA) ──────────────────────────── */

#define ATA_CMD_READ_PIO       0x20
#define ATA_CMD_READ_PIO_EXT   0x24
#define ATA_CMD_READ_DMA       0xC8
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_PIO      0x30
#define ATA_CMD_WRITE_PIO_EXT  0x34
#define ATA_CMD_WRITE_DMA      0xCA
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_FLUSH          0xE7
#define ATA_CMD_FLUSH_EXT      0xEA
#define ATA_CMD_SET_FEATURES   0xEF
#define ATA_CMD_STANDBY        0xE2
#define ATA_CMD_IDLE           0xE3
#define ATA_CMD_STANDBY_IMMED  0xE0
#define ATA_CMD_IDLE_IMMED     0xE1
#define ATA_CMD_SLEEP          0xE6
#define ATA_CMD_READ_NATIVE_MAX 0xF8
#define ATA_CMD_SET_MAX        0xF9
#define ATA_CMD_SECURITY_FREEZE 0xF5

/* ── Drive / Bus Selection ───────────────────────────────────────── */

#define ATA_BUS_PRIMARY    0
#define ATA_BUS_SECONDARY  1
#define ATA_DRIVE_MASTER   0
#define ATA_DRIVE_SLAVE    1

/* ── Timeout Constants ───────────────────────────────────────────── */

#define ATA_PIO_TIMEOUT_BSY  10000000  /* Wait loops for BSY clear */
#define ATA_PIO_TIMEOUT_DRQ  10000000  /* Wait loops for DRQ set */
#define ATA_PIO_TMO_RESET    5000000   /* Wait loops for reset recovery */

/* ── Sector Size ─────────────────────────────────────────────────── */

#define ATA_SECTOR_SIZE      512
#define ATA_SECTOR_WORDS     256

/* ── Multi-Sector Transfer Limits ─────────────────────────────────── */

/* Maximum number of sectors per PIO multi-sector command per ATA spec.
 * The 8-bit sector count register can hold 1-255 explicitly, and 0x00
 * means 256 sectors.  We expose the count as uint8_t so callers pass
 * 1-255; the hardware value 0x00 (== 256) is handled internally. */
#define ATA_PIO_MAX_SECTORS  256

/* ── Public API ──────────────────────────────────────────────────── */

/* Wait for BSY bit to clear (device ready).  Returns 0 on success,
 * -ETIMEDOUT if the device does not respond within the timeout. */
int ata_pio_wait_bsy(int bus);

/* Wait for DRQ bit to be set (data ready to transfer).
 * Returns 0 on success, -ETIMEDOUT on timeout.
 * If ERR is set, returns -EIO immediately. */
int ata_pio_wait_drq(int bus);

/* Perform the ATA-specified 400ns recovery delay by reading the
 * alternate status port four times.  Some controllers require this
 * between PIO commands. */
void ata_pio_400ns_delay(int bus);

/* ── Raw Data Port Transfers ─────────────────────────────────────── */

/* PIO data-in: read `count` 16-bit words from the ATA data port into
 * `buf`.  This is the lowest-level primitive — the caller must ensure
 * DRQ is asserted before calling, and check ERR after completion.
 * Returns 0 on success, -EIO on error. */
int ata_pio_read_data(int bus, uint16_t *buf, int count);

/* PIO data-out: write `count` 16-bit words from `buf` to the ATA data
 * port.  The caller must ensure DRQ is asserted before calling.
 * Returns 0 on success, -EIO on error. */
int ata_pio_write_data(int bus, const uint16_t *buf, int count);

/* ── High-Level Primitives ───────────────────────────────────────── */

/* Select a drive on a bus and wait for it to become ready.
 * Returns 0 on success, -ENODEV if no drive responds, -EIO on error. */
int ata_pio_select_drive(int bus, int master);

/* Execute IDENTIFY DEVICE command.  Reads 256 words of identify data
 * into `ident`.  Returns 0 on success, negative errno on error. */
int ata_pio_identify(int bus, int master, uint16_t *ident);

/* Read one sector (512 bytes) using PIO data-in protocol.
 * @bus:    ATA_BUS_PRIMARY or ATA_BUS_SECONDARY
 * @master: ATA_DRIVE_MASTER or ATA_DRIVE_SLAVE
 * @lba:    28-bit LBA address
 * @buf:    destination buffer (must be at least 512 bytes)
 * Returns 0 on success, negative errno on error. */
int ata_pio_read_sector(int bus, int master, uint32_t lba, uint16_t *buf);

/* Read multiple sectors using PIO data-in protocol.
 * @bus:    ATA bus number
 * @master: drive select
 * @lba:    28-bit starting LBA
 * @count:  number of sectors to read (max 256)
 * @buf:    destination buffer (must be count * 512 bytes)
 * Returns 0 on success, negative errno on error. */
int ata_pio_read_sectors(int bus, int master, uint32_t lba,
                         uint8_t count, uint16_t *buf);

/* Write one sector (512 bytes) using PIO data-out protocol.
 * Same parameters as ata_pio_read_sector, but data flows to the device.
 * Returns 0 on success, negative errno on error. */
int ata_pio_write_sector(int bus, int master, uint32_t lba,
                         const uint16_t *buf);

/* Write multiple sectors using PIO data-out protocol.
 * Returns 0 on success, negative errno on error.
 * Automatically issues FLUSH CACHE after the final sector. */
int ata_pio_write_sectors(int bus, int master, uint32_t lba,
                          uint8_t count, const uint16_t *buf);

/* Flush the device write cache.
 * Returns 0 on success, negative errno on error. */
int ata_pio_flush(int bus);

/* Read the alternate status register (does not affect IRQ logic). */
uint8_t ata_pio_alt_status(int bus);

/* Read the error register. */
uint8_t ata_pio_read_error(int bus);

/* Soft-reset a bus (set SRST, wait, clear).  Returns 0 on success,
 * -EIO if the bus doesn't recover. */
int ata_pio_soft_reset(int bus);

#endif /* ATA_PIO_H */
