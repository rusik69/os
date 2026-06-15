#ifndef AHCI_H
#define AHCI_H

#include "types.h"

#define AHCI_SECTOR_SIZE 512

/* AHCI signature constants */
#define AHCI_SIG_ATA    0x00000101  /* SATA drive */
#define AHCI_SIG_ATAPI  0xEB140101  /* SATAPI drive */

/* Maximum number of PM (Port Multiplier) ports per physical port */
#define AHCI_MAX_PM_PORTS  15

/* Block device ID base for PM sub-ports */
#define BLOCKDEV_AHCI_PM_BASE  100

int  ahci_init(void);
void ahci_exit(void);
int  ahci_is_present(void);
uint32_t ahci_get_sectors(void);
int  ahci_read_sectors(uint32_t lba, uint8_t count, void *buf);
int  ahci_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* ── AHCI NCQ API ──────────────────────────────────────────────────── */

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

#endif
