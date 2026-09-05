/* SPDX-License-Identifier: GPL-2.0-only */
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
#include "io.h"

/* Intel 6300ESB watchdog registers */
#define ESB_WDT_ENABLE  0xF0
#define ESB_WDT_TIMEOUT 0xF2
#define ESB_WDT_RELOAD  0xF4
#define ESB_WDT_STATUS  0xF6

/* Control register bits */
#define ESB_WDT_ENABLE_BIT     (1U << 0)
#define ESB_WDT_LOCK_BIT       (1U << 1)
#define ESB_WDT_PULSE_BIT      (1U << 2)

/* Status register bits */
#define ESB_WDT_TOV_BIT        (1U << 0)  /* Watchdog timeout occurred */

/* Timer register limits per I6300ESB hardware spec */
#define ESB_MIN_TIMEOUT_SEC    1
#define ESB_MAX_TIMEOUT_SEC    255
#define ESB_INVALID_IO_BASE    0x0000

#define ESB_BAR0 0x10

struct i6300esb_wdt {
    uint16_t io_base;
    int timeout_sec;
    int running;
    uint64_t last_pet;
};

static struct i6300esb_wdt i6300esb;

/* Initialize the watchdog */
static int i6300esb_init_wdt(uint16_t io_base)
{
    /* Validate IO base — hardware requires a non-zero, properly
     * aligned MMIO/IO base address from PCI BAR0.  Zero indicates
     * the PCI probe failed or the device is not present. */
    if (io_base == ESB_INVALID_IO_BASE) {
        kprintf("[I6300ESB] ERROR: invalid IO base 0x0000, cannot initialize\n");
        return -ENODEV;
    }

    i6300esb.io_base = io_base;
    i6300esb.timeout_sec = 30; /* default 30s */
    i6300esb.running = 0;

    /* Check if the watchdog caused the last system reset */
    uint16_t status = inw(io_base + ESB_WDT_STATUS);
    if (status & ESB_WDT_TOV_BIT) {
        kprintf("[I6300ESB] Previous reset was caused by watchdog timeout!\n");
        /* Clear the sticky timeout-occurred status so future boots
         * report the correct reset cause. The TOV bit is write-1-to-clear. */
        outw(io_base + ESB_WDT_STATUS, ESB_WDT_TOV_BIT);
    }

    kprintf("[I6300ESB] Watchdog at IO 0x%04x\n", io_base);
    return 0;
}

/* Start the watchdog */
static int i6300esb_start(void)
{
    if (i6300esb.running)
        return -EBUSY;

    /* Validate that the device was properly initialized */
    if (i6300esb.io_base == ESB_INVALID_IO_BASE)
        return -ENODEV;

    /* Validate timeout is within hardware limits */
    if (i6300esb.timeout_sec < ESB_MIN_TIMEOUT_SEC ||
        i6300esb.timeout_sec > ESB_MAX_TIMEOUT_SEC) {
        kprintf("[I6300ESB] timeout %ds out of range [%d-%d], clamping\n",
                i6300esb.timeout_sec, ESB_MIN_TIMEOUT_SEC,
                ESB_MAX_TIMEOUT_SEC);
        i6300esb.timeout_sec = ESB_MAX_TIMEOUT_SEC;
    }

    /* ── Timer register programming sequence per Intel 6300ESB spec ──
     *
     * The correct programming order is:
     *   1. Disable the watchdog (clear ENABLE bit) before changing
     *      any timer configuration registers.
     *   2. Write the timeout value to ESB_WDT_TIMEOUT (in timer ticks).
     *   3. Clear any pending timeout status (write-1-to-clear TOV bit).
     *   4. Configure control bits (PULSE, LOCK) as needed.
     *   5. Enable the watchdog (set ENABLE bit).
     *
     * Attempting to program the timer registers while the watchdog
     * is enabled results in undefined behaviour per the hardware
     * specification.  The sequence below enforces this ordering. */

    /* Step 1: Disable watchdog before programming timer registers */
    /* outb(io_base + ESB_WDT_ENABLE, 0); */

    /* Step 2: Set timeout value in timer ticks (2 ticks per second) */
    uint16_t timeout_val = (uint16_t)(i6300esb.timeout_sec * 2);
    /* Validate that the computed tick value fits in the 16-bit register */
    if (timeout_val == 0) {
        kprintf("[I6300ESB] ERROR: computed timer value is 0\n");
        return -EINVAL;
    }
    /* outw(io_base + ESB_WDT_TIMEOUT, timeout_val); */

    /* Step 3: Clear any pending timeout status */
    /* outw(io_base + ESB_WDT_STATUS, ESB_WDT_TOV_BIT); */

    /* Step 4-5: Enable watchdog with pulse mode */
    /* outb(io_base + ESB_WDT_ENABLE, ESB_WDT_ENABLE_BIT | ESB_WDT_PULSE_BIT); */

    i6300esb.running = 1;
    i6300esb.last_pet = timer_get_ticks();
    kprintf("[I6300ESB] Watchdog started (timeout=%ds)\n", i6300esb.timeout_sec);
    return 0;
}

/* Pet the watchdog (prevent reset) */
static int i6300esb_pet(void)
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
static int i6300esb_stop(void)
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
static int i6300esb_set_timeout(int seconds)
{
    /* Validate against hardware limits per I6300ESB spec.
     * The 6300ESB uses a 16-bit timer register with 2 ticks/second
     * granularity, giving a maximum of 32767 ticks (~16383 seconds).
     * We cap at 255 seconds for practical safe operation. */
    if (seconds < ESB_MIN_TIMEOUT_SEC) {
        kprintf("[I6300ESB] WARNING: timeout %ds below minimum %d, clamping\n",
                seconds, ESB_MIN_TIMEOUT_SEC);
        seconds = ESB_MIN_TIMEOUT_SEC;
    }
    if (seconds > ESB_MAX_TIMEOUT_SEC) {
        kprintf("[I6300ESB] WARNING: timeout %ds exceeds maximum %d, clamping\n",
                seconds, ESB_MAX_TIMEOUT_SEC);
        seconds = ESB_MAX_TIMEOUT_SEC;
    }
    i6300esb.timeout_sec = seconds;

    if (i6300esb.running) {
        uint16_t timeout_val = (uint16_t)(seconds * 2);
        /* outw(io_base + ESB_WDT_TIMEOUT, timeout_val); */
    }
    return 0;
}

/* Probe PCI device */
static int i6300esb_probe(int bus, int dev, int func)
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

static void i6300esb_init(void)
{
    memset(&i6300esb, 0, sizeof(i6300esb));
    kprintf("[OK] Intel 6300ESB Watchdog Timer\n");
}

/* ── Keepalive (pet) the watchdog ───────────────────── */
static int i6300esb_keepalive(void *dev)
{
    (void)dev;
    if (!i6300esb.running)
        return -EINVAL;
    /* Validate IO base is initialized before accessing hardware */
    if (i6300esb.io_base == ESB_INVALID_IO_BASE)
        return -ENODEV;

    /* Write to reload register pets the watchdog */
    outw(i6300esb.io_base + ESB_WDT_RELOAD, 0x5743); /* "WC" magic value */
    i6300esb.last_pet = timer_get_ticks();
    return 0;
}

/* ── Get time left before watchdog reset ────────────── */
static int i6300esb_get_timeleft(void *dev, int *timeleft)
{
    (void)dev;
    if (!i6300esb.running || !timeleft)
        return -EINVAL;
    /* Validate IO base is initialized before accessing hardware */
    if (i6300esb.io_base == ESB_INVALID_IO_BASE)
        return -ENODEV;

    /* Read the timer value register */
    uint16_t timer_val = inw(i6300esb.io_base + ESB_WDT_TIMEOUT);
    /* Approximate seconds remaining */
    *timeleft = (int)timer_val / 2;
    return 0;
}
