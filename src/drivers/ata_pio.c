/*
 * ata_pio.c — ATA PIO data-in/data-out primitives for legacy IDE drives.
 *
 * Provides the low-level register-access functions for ATA PIO mode
 * transfers on the legacy IDE controller (primary and secondary buses,
 * master and slave drives).
 *
 * PIO (Programmed I/O) is the simplest ATA transfer mode — the CPU
 * reads/writes one 16-bit word at a time from/to the controller's data
 * register.  These functions implement the register-level protocol
 * (select drive, set LBA, issue command, wait for DRQ, transfer data,
 * check status).
 *
 * Both 28-bit LBA (standard) addressing is supported.
 *
 * The module also provides the higher-level wrappers that the block
 * device layer's legacy ATA driver (ata.c) uses for sector I/O.
 */

#include "ata_pio.h"
#include "io.h"
#include "printf.h"
#include "timer.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── Static Helpers ──────────────────────────────────────────────── */

/*
 * Return the device-control port (ALT_STATUS == DEV_CTRL).
 * SRST lives at the same port for each bus.
 */
static inline uint16_t dev_ctrl_port(int bus)
{
	return (uint16_t)(bus == 0 ? 0x3F6 : 0x376);
}

/* ── Wait Primitives ─────────────────────────────────────────────── */

int ata_pio_wait_bsy(int bus)
{
	int timeout = ATA_PIO_TIMEOUT_BSY;
	uint16_t sts_port = ATA_PIO_STATUS(bus);

	while ((inb(sts_port) & ATA_SR_BSY) && --timeout > 0)
		__asm__ volatile("pause");

	return timeout > 0 ? 0 : -ETIMEDOUT;
}

int ata_pio_wait_drq(int bus)
{
	int timeout = ATA_PIO_TIMEOUT_DRQ;
	uint16_t sts_port = ATA_PIO_STATUS(bus);

	while (timeout-- > 0) {
		uint8_t sts = inb(sts_port);

		if (sts & ATA_SR_ERR)
			return -EIO;
		if (sts & ATA_SR_DRQ)
			return 0;
		__asm__ volatile("pause");
	}
	return -ETIMEDOUT;
}

void ata_pio_400ns_delay(int bus)
{
	uint16_t alt = ATA_PIO_ALT_STATUS(bus);

	inb(alt);
	inb(alt);
	inb(alt);
	inb(alt);
}

uint8_t ata_pio_alt_status(int bus)
{
	return inb(ATA_PIO_ALT_STATUS(bus));
}

uint8_t ata_pio_read_error(int bus)
{
	return inb(ATA_PIO_ERROR(bus));
}

int ata_pio_flush(int bus)
{
	outb(ATA_PIO_COMMAND(bus), ATA_CMD_FLUSH);
	ata_pio_400ns_delay(bus);
	return ata_pio_wait_bsy(bus);
}

/* ── Raw Data Port Transfers ─────────────────────────────────────── */

int ata_pio_read_data(int bus, uint16_t *buf, int count)
{
	uint16_t data_port = ATA_PIO_DATA(bus);
	int i;

	if (!buf || count <= 0)
		return -EINVAL;

	for (i = 0; i < count; i++)
		buf[i] = inw(data_port);

	return 0;
}

int ata_pio_write_data(int bus, const uint16_t *buf, int count)
{
	uint16_t data_port = ATA_PIO_DATA(bus);
	int i;

	if (!buf || count <= 0)
		return -EINVAL;

	for (i = 0; i < count; i++)
		outw(data_port, buf[i]);

	return 0;
}

/* ── Drive Selection ─────────────────────────────────────────────── */

int ata_pio_select_drive(int bus, int master)
{
	uint16_t dh_port = ATA_PIO_DRIVE_HEAD(bus);
	uint8_t dh_val;

	/* LBA mode, master=0xA0, slave=0xB0 (just drive select, no LBA bits).
	 * The device will set BSY briefly; wait for it to clear. */
	dh_val = master ? 0xB0 : 0xA0;
	outb(dh_port, dh_val);
	ata_pio_400ns_delay(bus);

	if (ata_pio_wait_bsy(bus) < 0)
		return -EIO;

	/* Verify a drive is actually present — status should not be 0xFF. */
	if (inb(ATA_PIO_STATUS(bus)) == 0xFF)
		return -ENODEV;

	return 0;
}

/* ── IDENTIFY DEVICE ─────────────────────────────────────────────── */

int ata_pio_identify(int bus, int master, uint16_t *ident)
{
	int ret;

	ret = ata_pio_select_drive(bus, master);
	if (ret < 0)
		return ret;

	/* Prepare IDENTIFY command (all registers zero except command) */
	outb(ATA_PIO_SECT_CNT(bus), 0);
	outb(ATA_PIO_LBA_LO(bus), 0);
	outb(ATA_PIO_LBA_MID(bus), 0);
	outb(ATA_PIO_LBA_HI(bus), 0);

	outb(ATA_PIO_COMMAND(bus), ATA_CMD_IDENTIFY);
	ata_pio_400ns_delay(bus);

	/* Check for device presence — a non-existent device returns 0 */
	{
		uint8_t sts = inb(ATA_PIO_STATUS(bus));

		if (sts == 0)
			return -ENODEV;
	}

	/* Wait for BSY=0 */
	ret = ata_pio_wait_bsy(bus);
	if (ret < 0)
		return ret;

	/* Non-ATA devices have non-zero LBAmid and LBAhi */
	if (inb(ATA_PIO_LBA_MID(bus)) != 0 || inb(ATA_PIO_LBA_HI(bus)) != 0)
		return -ENODEV;

	/* Wait for DRQ or check ERR — poll both */
	{
		int timeout = 100000;

		while (timeout-- > 0) {
			uint8_t sts = inb(ATA_PIO_STATUS(bus));

			if (sts & ATA_SR_ERR)
				return -EIO;
			if (sts & ATA_SR_DRQ)
				break;
			__asm__ volatile("pause");
		}
		if (timeout <= 0)
			return -ETIMEDOUT;
	}

	/* Read 256 words of IDENTIFY data */
	return ata_pio_read_data(bus, ident, ATA_SECTOR_WORDS);
}

/* ── Sector Read / Write ─────────────────────────────────────────── */

/*
 * Retry a timing-sensitive ATA PIO operation on timeout.
 * The loop resets the bus and retries up to ATA_PIO_READ_RETRIES times
 * when the device does not respond in time (ETIMEDOUT).  Hard errors
 * (EIO, EINVAL) are returned immediately.
 */
#define ATA_PIO_READ_RETRIES  3

int ata_pio_read_sector(int bus, int master, uint32_t lba, uint16_t *buf)
{
	int ret;
	int attempt;

	if (!buf)
		return -EINVAL;
	if (lba > 0x0FFFFFFF)
		return -EINVAL;

	for (attempt = 0; attempt < ATA_PIO_READ_RETRIES; attempt++) {
		uint16_t dh_port = ATA_PIO_DRIVE_HEAD(bus);

		/* Select drive with LBA bits */
		outb(dh_port, 0xE0 | (master ? 0x10 : 0x00) |
		     ((lba >> 24) & 0x0F));
		ata_pio_400ns_delay(bus);

		ret = ata_pio_wait_bsy(bus);
		if (ret < 0) {
			if (ret == -ETIMEDOUT && attempt < ATA_PIO_READ_RETRIES - 1) {
				kprintf("[ATA PIO] READ LBA=%u: timeout on "
					"BSY wait, retrying (%d)\n",
					(unsigned)lba, attempt + 1);
				ata_pio_soft_reset(bus);
				continue;
			}
			return ret;
		}

		/* Program sector count and LBA registers */
		outb(ATA_PIO_SECT_CNT(bus), 1);
		outb(ATA_PIO_LBA_LO(bus), lba & 0xFF);
		outb(ATA_PIO_LBA_MID(bus), (lba >> 8) & 0xFF);
		outb(ATA_PIO_LBA_HI(bus), (lba >> 16) & 0xFF);

		/* Issue READ SECTOR(S) command */
		outb(ATA_PIO_COMMAND(bus), ATA_CMD_READ_PIO);
		ata_pio_400ns_delay(bus);

		/* Wait for BSY=0 */
		ret = ata_pio_wait_bsy(bus);
		if (ret < 0) {
			if (ret == -ETIMEDOUT && attempt < ATA_PIO_READ_RETRIES - 1) {
				kprintf("[ATA PIO] READ LBA=%u: timeout after "
					"command, retrying (%d)\n",
					(unsigned)lba, attempt + 1);
				ata_pio_soft_reset(bus);
				continue;
			}
			return ret;
		}

		/* Check error */
		{
			uint8_t sts = inb(ATA_PIO_STATUS(bus));

			if (sts & ATA_SR_ERR) {
				uint8_t err = ata_pio_read_error(bus);

				kprintf("[ATA PIO] READ error: status=0x%02x "
					"error=0x%02x LBA=%u\n",
					sts, err, (unsigned)lba);
				return -EIO;
			}
		}

		/* Wait for DRQ and read data */
		ret = ata_pio_wait_drq(bus);
		if (ret < 0) {
			if (ret == -ETIMEDOUT && attempt < ATA_PIO_READ_RETRIES - 1) {
				kprintf("[ATA PIO] READ LBA=%u: timeout on "
					"DRQ wait, retrying (%d)\n",
					(unsigned)lba, attempt + 1);
				ata_pio_soft_reset(bus);
				continue;
			}
			return ret;
		}

		return ata_pio_read_data(bus, buf, ATA_SECTOR_WORDS);
	}

	return -ETIMEDOUT;
}

int ata_pio_read_sectors(int bus, int master, uint32_t lba,
			 uint8_t count, uint16_t *buf)
{
	uint16_t dh_port = ATA_PIO_DRIVE_HEAD(bus);
	int ret;
	unsigned int s;

	if (!buf || count == 0)
		return -EINVAL;
	if (lba > 0x0FFFFFFF)
		return -EINVAL;
	/* Validate the full multi-sector transfer stays within the 28-bit
	 * addressable range (268,435,456 sectors, numbered 0..0x0FFFFFFF). */
	if ((uint32_t)(lba + count) > 0x10000000UL)
		return -EINVAL;

	/* Select drive with LBA bits */
	outb(dh_port, 0xE0 | (master ? 0x10 : 0x00) | ((lba >> 24) & 0x0F));
	ata_pio_400ns_delay(bus);

	ret = ata_pio_wait_bsy(bus);
	if (ret < 0)
		return ret;

	/* Program sector count and LBA */
	outb(ATA_PIO_SECT_CNT(bus), count);
	outb(ATA_PIO_LBA_LO(bus), lba & 0xFF);
	outb(ATA_PIO_LBA_MID(bus), (lba >> 8) & 0xFF);
	outb(ATA_PIO_LBA_HI(bus), (lba >> 16) & 0xFF);

	/* Issue READ SECTOR(S) */
	outb(ATA_PIO_COMMAND(bus), ATA_CMD_READ_PIO);
	ata_pio_400ns_delay(bus);

	for (s = 0; s < count; s++) {
		ata_pio_400ns_delay(bus);

		ret = ata_pio_wait_bsy(bus);
		if (ret < 0)
			return ret;

		{
			uint8_t sts = inb(ATA_PIO_STATUS(bus));

			if (sts & ATA_SR_ERR) {
				uint8_t err = ata_pio_read_error(bus);

				kprintf("[ATA PIO] READ sectors error at LBA=%u+%u: "
					"status=0x%02x error=0x%02x\n",
					(unsigned)lba, s, sts, err);
				return -EIO;
			}
		}

		ret = ata_pio_wait_drq(bus);
		if (ret < 0)
			return ret;

		ret = ata_pio_read_data(bus, buf + s * ATA_SECTOR_WORDS,
					ATA_SECTOR_WORDS);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int ata_pio_write_sector(int bus, int master, uint32_t lba,
			 const uint16_t *buf)
{
	uint16_t dh_port = ATA_PIO_DRIVE_HEAD(bus);
	int ret;

	if (!buf)
		return -EINVAL;
	if (lba > 0x0FFFFFFF)
		return -EINVAL;

	/* Select drive with LBA bits */
	outb(dh_port, 0xE0 | (master ? 0x10 : 0x00) | ((lba >> 24) & 0x0F));
	ata_pio_400ns_delay(bus);

	ret = ata_pio_wait_bsy(bus);
	if (ret < 0)
		return ret;

	/* Program sector count and LBA */
	outb(ATA_PIO_SECT_CNT(bus), 1);
	outb(ATA_PIO_LBA_LO(bus), lba & 0xFF);
	outb(ATA_PIO_LBA_MID(bus), (lba >> 8) & 0xFF);
	outb(ATA_PIO_LBA_HI(bus), (lba >> 16) & 0xFF);

	/* Issue WRITE SECTOR(S) */
	outb(ATA_PIO_COMMAND(bus), ATA_CMD_WRITE_PIO);
	ata_pio_400ns_delay(bus);

	/* Wait for DRQ before writing data */
	ret = ata_pio_wait_bsy(bus);
	if (ret < 0)
		return ret;

	ret = ata_pio_wait_drq(bus);
	if (ret < 0)
		return ret;

	/* Write data */
	ret = ata_pio_write_data(bus, buf, ATA_SECTOR_WORDS);
	if (ret < 0)
		return ret;

	/* Wait for command completion */
	ret = ata_pio_wait_bsy(bus);
	if (ret < 0)
		return ret;

	/* Check error */
	{
		uint8_t sts = inb(ATA_PIO_STATUS(bus));

		if (sts & ATA_SR_ERR) {
			uint8_t err = ata_pio_read_error(bus);

			kprintf("[ATA PIO] WRITE error: status=0x%02x error=0x%02x "
				"LBA=%u\n", sts, err, (unsigned)lba);
			return -EIO;
		}
	}

	/* Flush write cache */
	return ata_pio_flush(bus);
}

int ata_pio_write_sectors(int bus, int master, uint32_t lba,
			  uint8_t count, const uint16_t *buf)
{
	uint16_t dh_port = ATA_PIO_DRIVE_HEAD(bus);
	int ret;
	unsigned int s;

	if (!buf || count == 0)
		return -EINVAL;
	if (lba > 0x0FFFFFFF)
		return -EINVAL;
	/* Validate the full multi-sector transfer stays within the 28-bit
	 * addressable range (268,435,456 sectors, numbered 0..0x0FFFFFFF). */
	if ((uint32_t)(lba + count) > 0x10000000UL)
		return -EINVAL;

	/* Select drive with LBA bits */
	outb(dh_port, 0xE0 | (master ? 0x10 : 0x00) | ((lba >> 24) & 0x0F));
	ata_pio_400ns_delay(bus);

	ret = ata_pio_wait_bsy(bus);
	if (ret < 0)
		return ret;

	/* Program sector count and LBA */
	outb(ATA_PIO_SECT_CNT(bus), count);
	outb(ATA_PIO_LBA_LO(bus), lba & 0xFF);
	outb(ATA_PIO_LBA_MID(bus), (lba >> 8) & 0xFF);
	outb(ATA_PIO_LBA_HI(bus), (lba >> 16) & 0xFF);

	/* Issue WRITE SECTOR(S) */
	outb(ATA_PIO_COMMAND(bus), ATA_CMD_WRITE_PIO);
	ata_pio_400ns_delay(bus);

	for (s = 0; s < count; s++) {
		ata_pio_400ns_delay(bus);

		ret = ata_pio_wait_bsy(bus);
		if (ret < 0)
			return ret;

		ret = ata_pio_wait_drq(bus);
		if (ret < 0)
			return ret;

		ret = ata_pio_write_data(bus, buf + s * ATA_SECTOR_WORDS,
					 ATA_SECTOR_WORDS);
		if (ret < 0)
			return ret;
	}

	/* Wait for final completion */
	ret = ata_pio_wait_bsy(bus);
	if (ret < 0)
		return ret;

	{
		uint8_t sts = inb(ATA_PIO_STATUS(bus));

		if (sts & ATA_SR_ERR) {
			uint8_t err = ata_pio_read_error(bus);

			kprintf("[ATA PIO] WRITE sectors error at LBA=%u+%u: "
				"status=0x%02x error=0x%02x\n",
				(unsigned)lba, count - 1, sts, err);
			return -EIO;
		}
	}

	/* Flush write cache */
	return ata_pio_flush(bus);
}

/* ── Soft Reset ──────────────────────────────────────────────────── */

int ata_pio_soft_reset(int bus)
{
	uint16_t ctrl = dev_ctrl_port(bus);
	int ret;

	/* Set SRST */
	outb(ctrl, ATA_DC_SRST);
	ata_pio_400ns_delay(bus);

	/* Wait at least 5us for reset to take effect */
	{
		volatile int d;

		for (d = 0; d < 5000; d++)
			io_wait();
	}

	/* Clear SRST */
	outb(ctrl, 0);
	ata_pio_400ns_delay(bus);

	/* Wait for BSY to clear (device should now be in default state) */
	ret = ata_pio_wait_bsy(bus);
	if (ret < 0) {
		kprintf("[ATA PIO] Soft reset failed on bus %d\n", bus);
		return -EIO;
	}

	kprintf("[ATA PIO] Soft reset completed on bus %d\n", bus);
	return 0;
}

#ifdef MODULE
int init_module(void)
{
	kprintf("[ATA PIO] Module loaded\n");
	return 0;
}

void cleanup_module(void)
{
	kprintf("[ATA PIO] Module unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ATA PIO data-in/data-out primitives — legacy IDE transfers");
MODULE_ALIAS("ata_pio");
#endif /* MODULE */
