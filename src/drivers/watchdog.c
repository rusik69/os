/*
 * watchdog.c — Software watchdog timer
 *
 * Uses the dynamic timer subsystem to implement a software watchdog.
 * If watchdog_pet() is not called within the timeout period, the system
 * is rebooted via the keyboard controller reset port.
 */

#include "watchdog.h"
#include "timers.h"
#include "timer.h"
#include "io.h"
#include "string.h"
#include "printf.h"

static int g_watchdog_timer_id = -1;
static int g_watchdog_timeout_ticks = 0;
static int g_watchdog_active = 0;

/* Internal callback: if this fires, the watchdog was not petted in time */
static void watchdog_reboot(void *arg) {
    (void)arg;
    kprintf("\n*** WATCHDOG TIMEOUT — System reset ***\n");

    /* Attempt to reboot via keyboard controller */
    for (int i = 0; i < 3; i++) {
        outb(0x64, 0xFE);  /* pulse reset line */
        io_wait();
    }

    /* Fallback: triple fault via zero-divide */
    __asm__ volatile("div %0" : : "r"(0) : "eax", "edx");

    /* If still alive, halt */
    cli();
    for (;;) hlt();
}

/* Internal callback to re-arm the watchdog periodically */
static void watchdog_tick(void *arg) {
    (void)arg;
    if (!g_watchdog_active) return;

    /* The watchdog wasn't petted — fire the reboot */
    watchdog_reboot(NULL);
}

void watchdog_init(int timeout_seconds) {
    if (g_watchdog_active) {
        watchdog_stop();
    }

    if (timeout_seconds <= 0) timeout_seconds = 10;

    g_watchdog_timeout_ticks = (uint64_t)timeout_seconds * TIMER_FREQ;
    g_watchdog_active = 1;

    /* Schedule the initial watchdog timer */
    g_watchdog_timer_id = timer_schedule(watchdog_tick, NULL, g_watchdog_timeout_ticks);

    kprintf("[OK] Watchdog initialized (%d seconds timeout)\n", timeout_seconds);
}

void watchdog_pet(void) {
    if (!g_watchdog_active) return;

    /* Cancel the old timer and schedule a new one */
    if (g_watchdog_timer_id >= 0) {
        timer_cancel(g_watchdog_timer_id);
    }

    g_watchdog_timer_id = timer_schedule(watchdog_tick, NULL, g_watchdog_timeout_ticks);
}

void watchdog_stop(void) {
    if (!g_watchdog_active) return;

    g_watchdog_active = 0;
    if (g_watchdog_timer_id >= 0) {
        timer_cancel(g_watchdog_timer_id);
        g_watchdog_timer_id = -1;
    }

    kprintf("[OK] Watchdog stopped\n");
}
