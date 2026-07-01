/*
 * ata_power.c — ATA power management (idle/standby/sleep) for legacy PIO drives.
 *
 * Implements the ATA/ATAPI power management command set:
 *   - CHECK POWER MODE         — query current power state
 *   - IDLE IMMEDIATE / IDLE    — enter/reduce power, set timer
 *   - STANDBY IMMEDIATE / STANDBY — heads park, spindle stop
 *   - SLEEP                    — lowest power, requires reset to wake
 *   - SET FEATURES             — APM, EPC, timer subcommands
 *
 * All operations use the legacy PIO register interface (primary/secondary
 * IDE bus) via the ata_pio_* primitives.
 *
 * Reference: ATA/ATAPI-8 (ACS-3), T13/2161-D Revision 3.
 */

#include "ata_power.h"
#include "ata_pio.h"
#include "io.h"
#include "printf.h"
#include "timer.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── Wait for command completion after issuing a PIO command ─────── */

/*
 * Issue a no-data PIO command (write command register, wait BSY=0,
 * check error).  Returns 0 on success, negative errno on error.
 *
 * @bus:       ATA bus number
 * @dev_head:  Drive/head register value (e.g. 0xA0 for master, 0xB0 slave)
 *             or 0xE0|0x10 for master with LBA bits.  Use 0 to skip
 *             drive selection (caller has already selected).
 * @cmd:       Command code (e.g. ATA_CMD_IDLE_IMMEDIATE)
 */
static int ata_power_send_cmd(int bus, uint8_t dev_head, uint8_t cmd)
{
    int ret;

    /* Select drive if requested */
    if (dev_head != 0) {
        outb(ATA_PIO_DRIVE_HEAD(bus), dev_head);
        ata_pio_400ns_delay(bus);
        ret = ata_pio_wait_bsy(bus);
        if (ret < 0)
            return ret;
        /* Check for device presence */
        if (inb(ATA_PIO_STATUS(bus)) == 0xFF)
            return -ENODEV;
    }

    /* Write the command register */
    outb(ATA_PIO_COMMAND(bus), cmd);
    ata_pio_400ns_delay(bus);

    /* Wait for command completion (BSY clear) */
    ret = ata_pio_wait_bsy(bus);
    if (ret < 0) {
        kprintf("[ATA PM] Command 0x%02x timeout on bus %d\n", cmd, bus);
        return ret;
    }

    /* Check error bit */
    {
        uint8_t sts = inb(ATA_PIO_STATUS(bus));

        if (sts & ATA_SR_ERR) {
            uint8_t err = ata_pio_read_error(bus);

            kprintf("[ATA PM] Command 0x%02x error: status=0x%02x "
                    "error=0x%02x\n", cmd, sts, err);
            return -EIO;
        }
    }

    return 0;
}

/* ── CHECK POWER MODE ────────────────────────────────────────────── */

int ata_power_check_mode(int bus, int master)
{
    int ret;
    uint8_t dev_head;
    uint8_t result;

    if (bus != 0 && bus != 1)
        return -EINVAL;
    if (master != 0 && master != 1)
        return -EINVAL;

    /* Set device/head register: LBA mode, correct drive */
    dev_head = 0xA0 | (master ? 0x10 : 0x00);
    outb(ATA_PIO_DRIVE_HEAD(bus), dev_head);
    ata_pio_400ns_delay(bus);

    ret = ata_pio_wait_bsy(bus);
    if (ret < 0)
        return ret;

    /* Check presence */
    if (inb(ATA_PIO_STATUS(bus)) == 0xFF)
        return -ENODEV;

    /* Clear sector count register before issuing command.
     * Some devices return the previous value if we don't. */
    outb(ATA_PIO_SECT_CNT(bus), 0);
    ata_pio_400ns_delay(bus);

    /* Issue CHECK POWER MODE */
    outb(ATA_PIO_COMMAND(bus), ATA_CMD_CHECK_POWER_MODE);
    ata_pio_400ns_delay(bus);

    /* Wait for BSY clear */
    ret = ata_pio_wait_bsy(bus);
    if (ret < 0) {
        /*
         * If the device is in Sleep mode, the status register may
         * read 0x00 (device not responding) or 0xFF (no device).
         * Try to distinguish this from a true timeout by reading
         * the status register.
         */
        uint8_t sts = inb(ATA_PIO_STATUS(bus));

        if (sts == 0x00 || sts == 0xFF) {
            kprintf("[ATA PM] Device on bus %d %s appears to be in "
                    "SLEEP mode (status=0x%02x)\n",
                    bus, master ? "slave" : "master", sts);
            return ATA_PM_SLEEP;
        }
        return ret;
    }

    /* Read the sector count register — this is where the device
     * returns the current power mode. */
    result = inb(ATA_PIO_SECT_CNT(bus));

    return (int)result;
}

/* ── IDLE IMMEDIATE ──────────────────────────────────────────────── */

int ata_power_idle_immediate(int bus, int master)
{
    uint8_t dev_head = 0xA0 | (master ? 0x10 : 0x00);

    return ata_power_send_cmd(bus, dev_head, ATA_CMD_IDLE_IMMEDIATE);
}

/* ── IDLE (with timer) ───────────────────────────────────────────── */

int ata_power_idle(int bus, int master, uint8_t timer)
{
    int ret;
    uint8_t dev_head = 0xA0 | (master ? 0x10 : 0x00);

    /* Select drive */
    outb(ATA_PIO_DRIVE_HEAD(bus), dev_head);
    ata_pio_400ns_delay(bus);

    ret = ata_pio_wait_bsy(bus);
    if (ret < 0)
        return ret;

    if (inb(ATA_PIO_STATUS(bus)) == 0xFF)
        return -ENODEV;

    /* Write timer value to sector count register */
    outb(ATA_PIO_SECT_CNT(bus), timer);
    ata_pio_400ns_delay(bus);

    /* Issue IDLE command */
    outb(ATA_PIO_COMMAND(bus), ATA_CMD_IDLE);
    ata_pio_400ns_delay(bus);

    ret = ata_pio_wait_bsy(bus);
    if (ret < 0)
        return ret;

    {
        uint8_t sts = inb(ATA_PIO_STATUS(bus));

        if (sts & ATA_SR_ERR) {
            uint8_t err = ata_pio_read_error(bus);

            kprintf("[ATA PM] IDLE(timer=%u) error: status=0x%02x "
                    "error=0x%02x\n", timer, sts, err);
            return -EIO;
        }
    }

    return 0;
}

/* ── STANDBY IMMEDIATE ───────────────────────────────────────────── */

int ata_power_standby_immediate(int bus, int master)
{
    uint8_t dev_head = 0xA0 | (master ? 0x10 : 0x00);

    return ata_power_send_cmd(bus, dev_head, ATA_CMD_STANDBY_IMMEDIATE);
}

/* ── STANDBY (with timer) ────────────────────────────────────────── */

int ata_power_standby(int bus, int master, uint8_t timer)
{
    int ret;
    uint8_t dev_head = 0xA0 | (master ? 0x10 : 0x00);

    /* Select drive */
    outb(ATA_PIO_DRIVE_HEAD(bus), dev_head);
    ata_pio_400ns_delay(bus);

    ret = ata_pio_wait_bsy(bus);
    if (ret < 0)
        return ret;

    if (inb(ATA_PIO_STATUS(bus)) == 0xFF)
        return -ENODEV;

    /* Write timer value to sector count register */
    outb(ATA_PIO_SECT_CNT(bus), timer);
    ata_pio_400ns_delay(bus);

    /* Issue STANDBY command */
    outb(ATA_PIO_COMMAND(bus), ATA_CMD_STANDBY);
    ata_pio_400ns_delay(bus);

    ret = ata_pio_wait_bsy(bus);
    if (ret < 0)
        return ret;

    {
        uint8_t sts = inb(ATA_PIO_STATUS(bus));

        if (sts & ATA_SR_ERR) {
            uint8_t err = ata_pio_read_error(bus);

            kprintf("[ATA PM] STANDBY(timer=%u) error: status=0x%02x "
                    "error=0x%02x\n", timer, sts, err);
            return -EIO;
        }
    }

    return 0;
}

/* ── SLEEP ───────────────────────────────────────────────────────── */

int ata_power_sleep(int bus, int master)
{
    uint8_t dev_head = 0xA0 | (master ? 0x10 : 0x00);

    /*
     * After SLEEP, the device will not respond to commands except
     * reset.  The command register write itself may complete before
     * the device actually enters sleep on some controllers.
     * We still wait briefly for BSY to ensure the command was
     * accepted, but don't treat a timeout as a hard failure since
     * the device may already be asleep.
     */
    int ret = ata_power_send_cmd(bus, dev_head, ATA_CMD_SLEEP);

    if (ret < 0) {
        /*
         * The device may have entered Sleep before we could read
         * status.  If the status register reads 0x00 (bus floating)
         * or we got a timeout, assume the command succeeded.
         */
        uint8_t sts = inb(ATA_PIO_STATUS(bus));

        if (sts == 0x00 || ret == -ETIMEDOUT) {
            kprintf("[ATA PM] SLEEP command accepted on bus %d %s "
                    "(device likely asleep)\n",
                    bus, master ? "slave" : "master");
            return 0;
        }
    }

    return ret;
}

/* ── WAKE (soft reset) ───────────────────────────────────────────── */

int ata_power_wake(int bus, int master)
{
    int ret;

    kprintf("[ATA PM] Waking device on bus %d %s via soft reset\n",
            bus, master ? "slave" : "master");

    /*
     * Issue a software reset (SRST) via the device control register.
     * This is the standard method to wake a device from Sleep.
     */
    ret = ata_pio_soft_reset(bus);
    if (ret < 0) {
        kprintf("[ATA PM] Soft reset failed on bus %d\n", bus);
        return ret;
    }

    /* After reset, re-select the drive */
    {
        uint8_t dev_head = 0xA0 | (master ? 0x10 : 0x00);

        outb(ATA_PIO_DRIVE_HEAD(bus), dev_head);
        ata_pio_400ns_delay(bus);

        ret = ata_pio_wait_bsy(bus);
        if (ret < 0) {
            kprintf("[ATA PM] Drive select after wake failed on "
                    "bus %d %s\n", bus, master ? "slave" : "master");
            return ret;
        }
    }

    kprintf("[ATA PM] Device on bus %d %s woken successfully\n",
            bus, master ? "slave" : "master");
    return 0;
}

/* ── SET FEATURES ────────────────────────────────────────────────── */

int ata_power_set_feature(int bus, int master, uint8_t subcmd,
                          uint8_t count)
{
    int ret;
    uint8_t dev_head = 0xA0 | (master ? 0x10 : 0x00);

    /* Select drive */
    outb(ATA_PIO_DRIVE_HEAD(bus), dev_head);
    ata_pio_400ns_delay(bus);

    ret = ata_pio_wait_bsy(bus);
    if (ret < 0)
        return ret;

    if (inb(ATA_PIO_STATUS(bus)) == 0xFF)
        return -ENODEV;

    /*
     * SET FEATURES register setup:
     *   - feature register = subcommand identifier
     *   - sector count     = parameter
     *   - LBA registers    = 0
     *   - device/head      = already set
     */
    outb(ATA_PIO_ERROR(bus), subcmd);   /* feature register on legacy PIO */
    outb(ATA_PIO_SECT_CNT(bus), count);
    outb(ATA_PIO_LBA_LO(bus), 0);
    outb(ATA_PIO_LBA_MID(bus), 0);
    outb(ATA_PIO_LBA_HI(bus), 0);
    ata_pio_400ns_delay(bus);

    /* Issue SET FEATURES */
    outb(ATA_PIO_COMMAND(bus), 0xEF);
    ata_pio_400ns_delay(bus);

    ret = ata_pio_wait_bsy(bus);
    if (ret < 0)
        return ret;

    {
        uint8_t sts = inb(ATA_PIO_STATUS(bus));

        if (sts & ATA_SR_ERR) {
            uint8_t err = ata_pio_read_error(bus);

            kprintf("[ATA PM] SET FEATURES subcmd=0x%02x error: "
                    "status=0x%02x error=0x%02x\n",
                    subcmd, sts, err);
            return -EIO;
        }
    }

    return 0;
}

/* ── APM Level ───────────────────────────────────────────────────── */

int ata_power_apm_set(int bus, int master, uint8_t level)
{
    if (level >= 255)
        return ata_power_set_feature(bus, master,
                                     ATA_SF_DISABLE_APM, 0);

    return ata_power_set_feature(bus, master,
                                 ATA_SF_ENABLE_APM, level);
}

/* ── Print State ─────────────────────────────────────────────────── */

int ata_power_print_state(int bus, int master)
{
    int mode = ata_power_check_mode(bus, master);

    if (mode < 0) {
        kprintf("[ATA PM] bus %d %s: cannot query mode (err=%d)\n",
                bus, master ? "slave" : "master", mode);
        return mode;
    }

    switch (mode) {
    case ATA_PM_ACTIVE:
        kprintf("[ATA PM] bus %d %s: ACTIVE / IDLE (0x%02x)\n",
                bus, master ? "slave" : "master", (unsigned)mode);
        break;
    case ATA_PM_STANDBY:
        kprintf("[ATA PM] bus %d %s: STANDBY (0x%02x)\n",
                bus, master ? "slave" : "master", (unsigned)mode);
        break;
    case ATA_PM_SLEEP:
        kprintf("[ATA PM] bus %d %s: SLEEP (0x%02x)\n",
                bus, master ? "slave" : "master", (unsigned)mode);
        break;
    default:
        kprintf("[ATA PM] bus %d %s: UNKNOWN (0x%02x)\n",
                bus, master ? "slave" : "master", (unsigned)mode);
        break;
    }

    return mode;
}

#ifdef MODULE
/*
 * Module entry/exit — when compiled as a loadable module.
 * Power management is typically called from the ATA driver; this
 * module entry is minimal for self-loading scenarios.
 */
int init_module(void)
{
    kprintf("[ATA PM] ATA power management module loaded\n");
    return 0;
}

void cleanup_module(void)
{
    kprintf("[ATA PM] ATA power management module unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ATA power management — idle, standby, sleep, APM control");
MODULE_ALIAS("ata_power");
#endif /* MODULE */
