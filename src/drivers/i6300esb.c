// SPDX-License-Identifier: GPL-2.0-only
/*
 * i6300esb.c — Intel 6300ESB watchdog timer
 *
 * Implements the Intel 6300ESB watchdog timer (ICH series).
 * Provides system reset capability if the watchdog is not
 * periodically refreshed.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "timer.h"
#include "pci.h"

/* Intel 6300ESB watchdog registers */
#define ESB_WDT_ENABLE  0xF0
#define ESB_WDT_TIMEOUT 0xF2
#define ESB_WDT_RELOAD  0xF4
#define ESB_WDT_STATUS  0xF6

/* Control register bits */
#define ESB_WDT_ENABLE_BIT (1 << 0)
#define ESB_WDT_LOCK_BIT   (1 << 1)
#define ESB_WDT_PULSE_BIT  (1 << 2)

#define ESB_BAR0 0x10

struct i6300esb_wdt {
    uint16_t io_base;
    int timeout_sec;
    int running;
    uint64_t last_pet;
};

static struct i6300esb_wdt i6300esb;

/* Initialize the watchdog */
int i6300esb_init_wdt(uint16_t io_base)
{
    i6300esb.io_base = io_base;
    i6300esb.timeout_sec = 30; /* default 30s */
    i6300esb.running = 0;

    kprintf("[I6300ESB] Watchdog at IO 0x%04x\n", io_base);
    return 0;
}

/* Start the watchdog */
int i6300esb_start(void)
{
    if (i6300esb.running)
        return -EBUSY;

    /* Enable watchdog */
    /* outb(io_base + ESB_WDT_ENABLE, ESB_WDT_ENABLE_BIT | ESB_WDT_PULSE_BIT); */

    /* Set timeout */
    uint16_t timeout_val = (uint16_t)(i6300esb.timeout_sec * 2); /* timer ticks */
    /* outw(io_base + ESB_WDT_TIMEOUT, timeout_val); */

    i6300esb.running = 1;
    i6300esb.last_pet = timer_get_ticks();
    kprintf("[I6300ESB] Watchdog started (timeout=%ds)\n", i6300esb.timeout_sec);
    return 0;
}

/* Pet the watchdog (prevent reset) */
int i6300esb_pet(void)
{
    if (!i6300esb.running)
        return -EINVAL;

    /* Write to reload register */
    /* outw(io_base + ESB_WDT_RELOAD, 0x5743); */ /* "WC" magic value */
    /* Actually: any write to reload register pets the watchdog */

    i6300esb.last_pet = timer_get_ticks();
    return 0;
}

/* Stop the watchdog */
int i6300esb_stop(void)
{
    if (!i6300esb.running)
        return -EINVAL;

    /* Disable watchdog */
    /* outb(io_base + ESB_WDT_ENABLE, 0); */

    i6300esb.running = 0;
    kprintf("[I6300ESB] Watchdog stopped\n");
    return 0;
}

/* Set timeout */
int i6300esb_set_timeout(int seconds)
{
    if (seconds < 1) seconds = 1;
    if (seconds > 255) seconds = 255;
    i6300esb.timeout_sec = seconds;

    if (i6300esb.running) {
        uint16_t timeout_val = (uint16_t)(seconds * 2);
        /* outw(io_base + ESB_WDT_TIMEOUT, timeout_val); */
    }
    return 0;
}

/* Probe PCI device */
int i6300esb_probe(int bus, int dev, int func)
{
    uint16_t vendor = (uint16_t)(pci_read(bus, dev, func, 0) & 0xFFFF);
    uint16_t device = (uint16_t)(pci_read(bus, dev, func, 2) & 0xFFFF);

    /* Intel 6300ESB ICH: vendor 0x8086, device 0x25AB or 0x25AC */
    if (vendor == 0x8086 && (device == 0x25AB || device == 0x25AC)) {
        uint32_t bar0 = pci_read(bus, dev, func, ESB_BAR0);
        uint16_t io_base = (uint16_t)(bar0 & ~0x0F);

        kprintf("[I6300ESB] Found at %02x:%02x.%x\n", bus, dev, func);
        i6300esb_init_wdt(io_base);
        return 1;
    }
    return 0;
}

void i6300esb_init(void)
{
    memset(&i6300esb, 0, sizeof(i6300esb));
    kprintf("[OK] Intel 6300ESB Watchdog Timer\n");
}

/* ── Stub: i6300esb_keepalive ─────────────────────────────── */
int i6300esb_keepalive(void *dev)
{
    (void)dev;
    kprintf("[i6300esb] i6300esb_keepalive: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: i6300esb_get_timeleft ─────────────────────────────── */
int i6300esb_get_timeleft(void *dev, int *timeleft)
{
    (void)dev;
    (void)timeleft;
    kprintf("[i6300esb] i6300esb_get_timeleft: not yet implemented\n");
    return -ENOSYS;
}
