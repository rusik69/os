#ifndef AHCI_H
#define AHCI_H

#include "types.h"

#define AHCI_SECTOR_SIZE 512

/* ── Signature constants ────────────────────────────────────────────── */

/* AHCI signature constants */
#define AHCI_SIG_ATA    0x00000101  /* SATA drive */
#define AHCI_SIG_ATAPI  0xEB140101  /* SATAPI drive */
#define AHCI_SIG_PM     0x96690101  /* Port Multiplier */
#define AHCI_SIG_SEMB   0xC33C0101  /* SEMB device */
#define AHCI_SIG_PM_ATA 0x00000101  /* ATA device behind PM (same as direct) */

/* ── PM (Port Multiplier) constants ─────────────────────────────────── */

/* Maximum number of PM (Port Multiplier) ports per physical port.
 * Ports 0-14 are device ports, port 15 is the PM itself. */
#define AHCI_MAX_PM_PORTS      15
#define AHCI_PM_PORT_SELF      15   /* PM's own configuration port */

/* PM GSCR (General Status Configuration Register) indices.
 * These are read via SCR access at PM port 15 (the PM itself). */
#define AHCI_PM_GSCR_REVISION    0   /* PM hardware revision */
#define AHCI_PM_GSCR_PORT_COUNT  1   /* number of PM ports minus 1 */
#define AHCI_PM_GSCR_PORT_INFO   2   /* bitmask of connected ports */
#define AHCI_PM_GSCR_FEATURES    4   /* feature flags */

/* PM SCR register addresses (for per-port access) */
#define AHCI_SCR_STATUS     0   /* SStatus */
#define AHCI_SCR_ERROR      1   /* SError */
#define AHCI_SCR_CONTROL    2   /* SControl */
#define AHCI_SCR_ACTIVE     3   /* SActive */

/* SStatus.DET field values */
#define SSTS_DET_MASK       0x0F
#define SSTS_DET_NONE       0x00   /* no device detected */
#define SSTS_DET_COMRESET   0x01   /* device presence detected but COMRESET */
#define SSTS_DET_PRESENT    0x03   /* Phy communication established */
#define SSTS_DET_OFFLINE    0x04   /* Phy offline / disabled */

/* SControl.DET field values */
#define SCTL_DET_NONE       0x00   /* no action */
#define SCTL_DET_INIT       0x01   /* perform COMRESET */

/* Block device ID base for PM sub-ports */
#define BLOCKDEV_AHCI_PM_BASE  100

/* ── Core AHCI API ──────────────────────────────────────────────────── */

int  ahci_init(void);
void ahci_exit(void);
int  ahci_is_present(void);
uint32_t ahci_get_sectors(void);
int  ahci_read_sectors(uint32_t lba, uint8_t count, void *buf);
int  ahci_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* ── NCQ API ────────────────────────────────────────────────────────── */

/* Check if the AHCI controller supports NCQ (CAP2.SNCQ) */
int  ahci_has_ncq_cap(void);

/* Submit a queued NCQ READ FPDMA QUEUED command.
 * @port_num: physical port number (0-31)
 * @pm_port:  Port Multiplier port (-1 for direct-attached, 0-14 for PM)
 * @lba:      starting LBA (48-bit)
 * @count:    sector count (1-8 typically)
 * @buf:      data buffer (size = count * 512)
 * Returns 0 on success, -1 on error. */
int  ahci_ncq_read(int port_num, int pm_port, uint32_t lba,
                    uint8_t count, void *buf);

/* Submit a queued NCQ WRITE FPDMA QUEUED command.
 * Same parameters as ahci_ncq_read. */
int  ahci_ncq_write(int port_num, int pm_port, uint32_t lba,
                     uint8_t count, const void *buf);

/* Poll for NCQ completions on a physical port.
 * Checks the SDB FIS (Set Device Bits) and completes finished commands.
 * @port_num: physical port number
 * Returns the number of commands completed, or 0 if none. */
int  ahci_ncq_completion_poll(int port_num);

/* NCQ error recovery — reads error log, aborts pending commands, resets port.
 * @port_num: physical port number
 * @pm_port:  Port Multiplier port (-1 for direct, 0-14 for PM sub-port).
 *            Pass -1 to recover the physical port; pass specific PM port
 *            for per-PM-port error recovery.
 * Returns 0 on recovery completion, -1 on failure. */
int  ahci_ncq_recover_port(int port_num);

/* NCQ priority management — set/get the priority level for a port.
 * @port_num: physical port number
 * @priority: 0=simple, 1=deterministic, 2=high */
int  ahci_ncq_set_priority(int port_num, int priority);
int  ahci_ncq_get_priority(int port_num);

/* NCQ queue status — get active/free slot bitmasks.
 * @port_num:   physical port number
 * @out_active: output: bitmask of active (in-flight) slots
 * @out_free:   output: bitmask of free NCQ slots
 * Returns 0 on success, -1 if port not found. */
int  ahci_ncq_queue_status(int port_num, uint32_t *out_active,
                            uint32_t *out_free);

/* ── PM (Port Multiplier) API ───────────────────────────────────────── */

/* Read a PM port's SCR register (SStatus, SError, SControl, SActive).
 * @phys_port: physical port number (0-31)
 * @pm_port:   PM port number (0-14 for device ports, 15 for PM itself)
 * @scr_addr:  SCR register address (AHCI_SCR_STATUS/ERROR/CONTROL/ACTIVE)
 * @val:       output: register value
 * Returns 0 on success, -1 if the controller does not support PM SCR access. */
int  ahci_pm_scr_read(int phys_port, int pm_port, int scr_addr,
                       uint32_t *val);

/* Write a PM port's SCR register (typically SControl for reset, SError for clear).
 * @phys_port: physical port number (0-31)
 * @pm_port:   PM port number (0-14 for device ports, 15 for PM itself)
 * @scr_addr:  SCR register address (AHCI_SCR_ERROR/CONTROL)
 * @val:       value to write
 * Returns 0 on success, -1 if the controller does not support PM SCR access. */
int  ahci_pm_scr_write(int phys_port, int pm_port, int scr_addr,
                        uint32_t val);

/* Check if a PM port has a device connected (Phy communication established).
 * @phys_port: physical port number (0-31)
 * @pm_port:   PM port number to check (0-14)
 * Returns 1 if device present, 0 if no device, -1 on error. */
int  ahci_pm_port_detect(int phys_port, int pm_port);

/* Reset a PM port by cycling DET via its SControl register.
 * @phys_port: physical port number
 * @pm_port:   PM port to reset (0-14)
 * Returns 0 on success, -1 on failure. */
int  ahci_pm_port_reset(int phys_port, int pm_port);

/* Get a bitmask of PM ports that have devices connected.
 * Bit N is set if PM port N has a device.
 * @phys_port: physical port number
 * @port_map:  output: bitmask (bits 0-14)
 * Returns the number of connected ports, or -1 on error. */
int  ahci_pm_get_port_map(int phys_port, uint32_t *port_map);

/* Get PM capabilities (revision and port count).
 * @phys_port:  physical port number
 * @revision:   output: PM hardware revision (GSCR 0)
 * @port_count: output: number of PM ports (GSCR 1 + 1)
 * Returns 0 on success, -1 if not supported. */
int  ahci_pm_get_info(int phys_port, uint32_t *revision,
                       uint32_t *port_count);

/* Print PM diagnostic information to the kernel log. */
void ahci_pm_dump_info(int phys_port);

#endif /* AHCI_H */
