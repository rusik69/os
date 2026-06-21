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

    /* Read block length from DATA0 */
    int count = inb(SMBUS_DATA0);
    if (count > len) count = len;

    /* Read block data from BLOCK port */
    for (int i = 0; i < count; i++)
        buf[i] = inb(SMBUS_BLOCK);

    return count;
}
#include "module.h"
module_init(smbus_init);

/* ── Stub: smbus_read_block ─────────────────────────────── */
int smbus_read_block(int addr, int reg, void *buf, size_t len)
{
    (void)addr;
    (void)reg;
    (void)buf;
    (void)len;
    kprintf("[smbus] smbus_read_block: not yet implemented\n");
    return -ENOSYS;
}
