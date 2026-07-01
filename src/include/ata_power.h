#ifndef ATA_POWER_H
#define ATA_POWER_H

/*
 * ata_power.h — ATA power management API.
 *
 * Implements the ATA/ATAPI power management command set for legacy
 * PIO/IDE drives: CHECK POWER MODE, IDLE, STANDBY, SLEEP, and wake.
 *
 * All functions operate on a bus+drive pair (primary/secondary bus,
 * master/slave drive) using PIO register access.
 *
 * Reference: ATA/ATAPI-8 (ACS-3), T13/2161-D Revision 3, Section 6.x.
 */

#include "types.h"

/* ====================================================================
 *  ATA Power Management Commands (28-bit LBA register interface)
 * ==================================================================== */

/*
 * Power management command codes — each has a primary (alt) encoding.
 * Both encodings work identically on all modern ATA devices.
 */
#define ATA_CMD_CHECK_POWER_MODE    0xE5
#define ATA_CMD_CHECK_POWER_MODE_ALT 0x98
#define ATA_CMD_IDLE_IMMEDIATE      0xE1
#define ATA_CMD_IDLE_IMMEDIATE_ALT  0x95
#define ATA_CMD_IDLE                0xE3
#define ATA_CMD_IDLE_ALT            0x97
#define ATA_CMD_STANDBY_IMMEDIATE   0xE0
#define ATA_CMD_STANDBY_IMMEDIATE_ALT 0x94
#define ATA_CMD_STANDBY             0xE2
#define ATA_CMD_STANDBY_ALT         0x96
#define ATA_CMD_SLEEP               0xE6
#define ATA_CMD_SLEEP_ALT           0x99

/* ====================================================================
 *  Power Mode State Definitions
 *
 *  These values are returned in the sector count register by the
 *  CHECK POWER MODE command.  Not all devices support every state.
 * ==================================================================== */

/* Device is in Active or Idle mode — fully operational, immediate
 * command acceptance.  0xFF is the standard "not standby/sleep" value. */
#define ATA_PM_ACTIVE      0xFF

/* Device is in Standby mode — heads parked, interface still active.
 * Recovery time (spin-up) is required before data access. */
#define ATA_PM_STANDBY     0x80

/* Device is in Sleep mode — lowest power, requires reset to exit.
 * CHECK POWER MODE cannot be issued after SLEEP without waking first. */
#define ATA_PM_SLEEP       0x00

/* Returned when a non-ATA device is present (ATAPI), or on error. */
#define ATA_PM_UNKNOWN     0x00

/* ====================================================================
 *  Timer Constants
 *
 *  The IDLE and STANDBY commands program a timer in the sector count
 *  register.  The timer value is multiplied by 5 seconds to determine
 *  the idle/standby delay.
 *
 *  Special values:
 *    0   = disable the timer (stay in current mode until commanded)
 *    1   = 5 seconds
 *    240 = 20 minutes
 *    255 = approximately 21 minutes
 *
 *  The actual timer resolution and range depend on the device.
 * ==================================================================== */

#define ATA_PM_TIMER_DISABLE    0   /* Disable idle/standby timer */
#define ATA_PM_TIMER_5SEC       1   /* 5 seconds */
#define ATA_PM_TIMER_10SEC      2   /* 10 seconds */
#define ATA_PM_TIMER_30SEC      6   /* 30 seconds */
#define ATA_PM_TIMER_1MIN       12  /* 1 minute */
#define ATA_PM_TIMER_5MIN       60  /* 5 minutes */
#define ATA_PM_TIMER_10MIN      120 /* 10 minutes */
#define ATA_PM_TIMER_20MIN      240 /* 20 minutes */

/* ====================================================================
 *  SET FEATURES subcommands related to power management
 *
 *  Issued via ATA_CMD_SET_FEATURES (0xEF) with the subcommand
 *  identifier in the feature register.
 * ==================================================================== */

/* Enable/disable the automatic Standby timer (subcommand for
 * SET FEATURES).  The timer value (in 5-second units) goes in the
 * sector count register.  This is an alternative to the STANDBY
 * command for some devices. */
#define ATA_SF_ENABLE_APM       0x05  /* Enable Advanced Power Management */
#define ATA_SF_DISABLE_APM      0x85  /* Disable Advanced Power Management */
#define ATA_SF_SET_STANDBY      0x02  /* Set standby timer (0=disable) */
#define ATA_SF_SET_IDLE_TIMER   0x03  /* Set idle timer (0=disable) */
#define ATA_SF_ENABLE_FREE_FALL 0x05  /* Enable free-fall control */
#define ATA_SF_DISABLE_FREE_FALL 0x85 /* Disable free-fall control */
#define ATA_SF_ENABLE_EPC       0x0A  /* Enable Extended Power Conditions */
#define ATA_SF_DISABLE_EPC      0x8A  /* Disable EPC */

/* ====================================================================
 *  Public API
 * ==================================================================== */

/**
 * ata_power_check_mode — Query the current power mode of an ATA device.
 * @bus:    ATA_BUS_PRIMARY (0) or ATA_BUS_SECONDARY (1)
 * @master: ATA_DRIVE_MASTER (0) or ATA_DRIVE_SLAVE (1)
 *
 * Returns one of ATA_PM_ACTIVE (0xFF), ATA_PM_STANDBY (0x80),
 * or ATA_PM_SLEEP (0x00) on success, or a negative errno on error:
 *   -ENODEV:  No device present on the bus/drive
 *   -EIO:     Device error or command failure
 *   -ETIMEDOUT: Device did not respond within timeout
 *
 * Note: A device in SLEEP state cannot respond to CHECK POWER MODE.
 * In that case the status register will read 0x00 or 0xFF depending on
 * the controller.  The return value ATA_PM_SLEEP is inferred.
 */
int ata_power_check_mode(int bus, int master);

/**
 * ata_power_idle_immediate — Enter Idle mode immediately.
 *
 * The device enters Idle mode (reduced power, heads possibly unloaded).
 * The interface stays active and the device can accept all commands
 * without a recovery delay.
 *
 * @bus:    ATA bus number (0=primary, 1=secondary)
 * @master: Drive select (0=master, 1=slave)
 * Returns 0 on success, negative errno on error.
 */
int ata_power_idle_immediate(int bus, int master);

/**
 * ata_power_idle — Set the idle timer and optionally enter Idle mode.
 * @bus:    ATA bus number
 * @master: Drive select
 * @timer:  Timer value in 5-second units.
 *          0 = disable idle timer (stay in current mode).
 *          1..255 = enter Idle mode after timer ticks.
 *
 * If timer > 0, the device waits the specified duration in Idle mode
 * (with heads possibly unloaded) before transitioning to Standby.
 * If timer = 0, the idle timer is disabled.
 *
 * Returns 0 on success, negative errno on error.
 */
int ata_power_idle(int bus, int master, uint8_t timer);

/**
 * ata_power_standby_immediate — Enter Standby mode immediately.
 *
 * The device enters Standby mode (heads parked, spindle stopped on HDD).
 * A recovery delay (spin-up) is required before data access.
 *
 * Returns 0 on success, negative errno on error.
 */
int ata_power_standby_immediate(int bus, int master);

/**
 * ata_power_standby — Set the standby timer and optionally enter Standby.
 * @bus:    ATA bus number
 * @master: Drive select
 * @timer:  Timer value in 5-second units.
 *          0 = disable standby timer (stay in current mode).
 *          1..255 = enter Standby after timer ticks.
 *
 * For HDDs, the spindle is stopped in Standby mode.  A spin-up
 * (typically 3-10 seconds) is needed before subsequent data access.
 *
 * Returns 0 on success, negative errno on error.
 */
int ata_power_standby(int bus, int master, uint8_t timer);

/**
 * ata_power_sleep — Enter Sleep mode (lowest power).
 *
 * The device enters the lowest power state.  The interface may appear
 * inactive.  To exit Sleep, the caller must either:
 *   - Use ata_power_wake() to perform a soft reset, or
 *   - Cycle device power via a controller-level action.
 *
 * After wake, the device needs time to spin up and re-initialise.
 *
 * Returns 0 on success, negative errno on error.
 * Note: On some controllers, the SLEEP command completes before the
 * device actually enters Sleep, so the return does not guarantee
 * the device is asleep — only that the command was accepted.
 */
int ata_power_sleep(int bus, int master);

/**
 * ata_power_wake — Wake a device from Sleep or Standby.
 * @bus:    ATA bus number
 * @master: Drive select
 *
 * Issues a soft reset (SRST) to wake the device from Sleep mode,
 * or to recover a device in Standby that is not accepting commands.
 *
 * After wake, the device needs time to spin up.  The caller should
 * verify with ata_power_check_mode() or ata_pio_identify().
 *
 * Returns 0 on success, negative errno on error.
 */
int ata_power_wake(int bus, int master);

/**
 * ata_power_set_feature — Issue a SET FEATURES power-management
 *                          subcommand.
 * @bus:      ATA bus number
 * @master:   Drive select
 * @subcmd:   Subcommand identifier (e.g. ATA_SF_ENABLE_APM)
 * @count:    Sector count register value (parameter for the subcommand)
 *
 * This is a low-level wrapper that issues the SET FEATURES command
 * (0xEF) with the given subcommand in the feature register and the
 * given value in the sector count register.
 *
 * Returns 0 on success, negative errno on error.
 */
int ata_power_set_feature(int bus, int master, uint8_t subcmd,
                          uint8_t count);

/**
 * ata_power_apm_set — Set Advanced Power Management level.
 * @bus:    ATA bus number
 * @master: Drive select
 * @level:  APM level (1..254, where 1=minimum power consumption,
 *          254=maximum performance, 128=balanced, 255=disable APM)
 *
 * Uses SET FEATURES with subcommand ATA_SF_ENABLE_APM (0x05).
 * If level >= 255, calls ATA_SF_DISABLE_APM.
 *
 * Returns 0 on success, negative errno on error.
 */
int ata_power_apm_set(int bus, int master, uint8_t level);

/**
 * ata_power_print_state — Log the current ATA power state via kprintf.
 * @bus:    ATA bus number
 * @master: Drive select
 *
 * Queries the power mode and logs a human-readable description.
 * Returns the power mode on success, negative errno on error.
 */
int ata_power_print_state(int bus, int master);

#endif /* ATA_POWER_H */
