/*
 * smbus.c — SMBus (System Management Bus) driver
 *
 * Intel PCH SMBus host controller interface using I/O ports.
 * Supports byte/word/block read/write operations.
 */

#include "smbus.h"
#include "io.h"
#include "printf.h"
#include "string.h"

static int g_smbus_init_done = 0;
static int g_smbus_present = 0;

/* Wait for SMBus to be ready (not busy) */
static int smbus_wait_idle(void) {
    int timeout = 100000;
    while (timeout--) {
        uint8_t hctl = inb(SMBUS_HCTL);
        if (!(hctl & SMBUS_HCTL_BUSY))
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

int smbus_init(void) {
    if (g_smbus_init_done)
        return 0;

    /* Probe: try to read SMBus host config */
    outb(SMBUS_HCTL, 0);
    uint8_t hctl = inb(SMBUS_HCTL);
    if (hctl == 0xFF) {
        kprintf("[SMBus] No SMBus controller detected at ports 0x%X-0x%X\n",
                SMBUS_BASE, SMBUS_BASE + 10);
        g_smbus_init_done = 1;
        return -1;
    }

    g_smbus_present = 1;
    g_smbus_init_done = 1;
    kprintf("[SMBus] Initialized (I/O base 0x%X)\n", SMBUS_BASE);
    return 0;
}

int smbus_is_present(void) {
    return g_smbus_present;
}

int smbus_read_byte(uint8_t addr, uint8_t reg, uint8_t *val) {
    if (!g_smbus_present || !val)
        return -1;
    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    /* Set command = register offset */
    outb(SMBUS_CMD, reg);
    /* Set slave address (7-bit << 1 | R/W#) */
    outb(SMBUS_ADDR, (addr << 1) | SMBUS_READ);
    /* Start byte-data read */
    outb(SMBUS_HCTL, SMBUS_HCTL_START | SMBUS_HCTL_BYTE_DATA);

    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    *val = inb(SMBUS_DATA0);
    return 0;
}

int smbus_write_byte(uint8_t addr, uint8_t reg, uint8_t val) {
    if (!g_smbus_present)
        return -1;
    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    outb(SMBUS_CMD, reg);
    outb(SMBUS_DATA0, val);
    outb(SMBUS_ADDR, (addr << 1) | SMBUS_WRITE);
    outb(SMBUS_HCTL, SMBUS_HCTL_START | SMBUS_HCTL_BYTE_DATA);

    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    return 0;
}

int smbus_read_word(uint8_t addr, uint8_t reg, uint16_t *val) {
    if (!g_smbus_present || !val)
        return -1;
    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    outb(SMBUS_CMD, reg);
    outb(SMBUS_ADDR, (addr << 1) | SMBUS_READ);
    outb(SMBUS_HCTL, SMBUS_HCTL_START | SMBUS_HCTL_WORD_DATA);

    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    *val = inb(SMBUS_DATA0) | ((uint16_t)inb(SMBUS_DATA1) << 8);
    return 0;
}

int smbus_write_word(uint8_t addr, uint8_t reg, uint16_t val) {
    if (!g_smbus_present)
        return -1;
    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    outb(SMBUS_CMD, reg);
    outb(SMBUS_DATA0, (uint8_t)val);
    outb(SMBUS_DATA1, (uint8_t)(val >> 8));
    outb(SMBUS_ADDR, (addr << 1) | SMBUS_WRITE);
    outb(SMBUS_HCTL, SMBUS_HCTL_START | SMBUS_HCTL_WORD_DATA);

    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    return 0;
}

/* ── Block count validation ───────────────────────────────────────── */

/* SMBus specification limits: block transfers carry a count byte that
 * must be 1-32 (inclusive).  A count of 0 or >32 is a protocol violation
 * from the device or a bus error.  Counts <= 0 are impossible from the
 * hardware (uint8_t) but we check defensively.
 *
 * Returns 0 if valid, -1 on violation.  Logs a warning on violation. */
static int smbus_validate_block_count(int count)
{
    if (count <= 0 || count > 32) {
        kprintf("[SMBus] WARNING: invalid block count %d "
                "(must be 1-32 per SMBus spec)\n", count);
        return -1;
    }
    return 0;
}

int smbus_block_read(uint8_t addr, uint8_t cmd, uint8_t *buf, int len) {
    if (!g_smbus_present || !buf || len <= 0 || len > 32)
        return -1;
    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    outb(SMBUS_CMD, cmd);
    outb(SMBUS_ADDR, (addr << 1) | SMBUS_READ);
    outb(SMBUS_HCTL, SMBUS_HCTL_START | SMBUS_HCTL_BLOCK);

    if (smbus_wait_idle() < 0)
        return SMBUS_ERR_TIMEOUT;

    /* Check for transaction error (NACK, CRC, host-device error) */
    if (inb(SMBUS_HCTL) & SMBUS_HCTL_ERROR)
        return SMBUS_ERR_NODEV;

    /* Read block length from DATA0 */
    int count = inb(SMBUS_DATA0);

    /* Validate block count using the shared helper; drain the FIFO
     * on failure so stale data does not corrupt subsequent operations. */
    if (smbus_validate_block_count(count) < 0) {
        /* Drain block data FIFO when count > 32 (the FIFO depth is
         * exactly 32 bytes per Intel PCH, so we drain that many).
         * For count == 0 there is nothing to drain. */
        if (count > 32) {
            for (int i = 0; i < 32; i++)
                (void)inb(SMBUS_BLOCK);
        }
        return SMBUS_ERR_NODEV;
    }

    /* Read up to len bytes into caller's buffer */
    int to_read = (count < len) ? count : len;
    for (int i = 0; i < to_read; i++)
        buf[i] = inb(SMBUS_BLOCK);

    /*
     * Drain remaining FIFO bytes if caller's buffer was smaller than
     * the device's count.  The BLOCK port (0xEF4) is shared with
     * SMBUS_DATA1; leaving unconsumed bytes would corrupt subsequent
     * word reads or block reads.
     */
    for (int i = to_read; i < count; i++)
        (void)inb(SMBUS_BLOCK);

    return to_read;
}
#include "module.h"
module_init(smbus_init);

/* ── Stub: smbus_read_block (reserved for future use) ─────────── */
static int smbus_read_block(int addr, int reg, void *buf, size_t len)
{
    /* Validate parameters */
    if (addr < 0 || addr > 127)
        return -1;
    if (!buf || len == 0 || len > 32)
        return -1;
    if (smbus_validate_block_count((int)len) < 0)
        return -1;

    kprintf("[SMBUS] smbus_read_block: not yet implemented\n");
    return -1;
}
