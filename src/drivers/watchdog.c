/*
 * watchdog.c — Software watchdog timer
 *
 * Uses the dynamic timer subsystem to implement a software watchdog.
 * If watchdog_pet() is not called within the timeout period, the system
 * is rebooted via the keyboard controller reset port.
 * Supports a pretimeout callback that fires before the full timeout.
 */

#include "watchdog.h"
#include "timers.h"
#include "timer.h"
#include "io.h"
#include "string.h"
#include "printf.h"

static int g_watchdog_timer_id = -1;
static int g_pretimeout_timer_id = -1;
static int g_watchdog_timeout_ticks = 0;
static int g_watchdog_active = 0;

/* Pretimeout state */
static int g_pretimeout_secs = 0;
static watchdog_pretimeout_fn_t g_pretimeout_fn = NULL;
static volatile int g_pretimeout_fired = 0;

/* Internal callback: if this fires, the watchdog was not petted in time */
static void watchdog_reboot(void *arg) {
    (void)arg;
    kprintf("\n*** WATCHDOG TIMEOUT — System reset ***\n");

    for (int i = 0; i < 3; i++) {
        outb(0x64, 0xFE);
        io_wait();
    }

    __asm__ volatile("div %0" : : "r"(0) : "eax", "edx");

    cli();
    for (;;) hlt();
}

/* Internal callback to re-arm the watchdog periodically */
static void watchdog_tick(void *arg) {
    (void)arg;
    if (!g_watchdog_active) return;

    watchdog_reboot(NULL);
}

/* Pretimeout callback — fires g_pretimeout_secs before the full timeout */
static void watchdog_pretimeout_tick(void *arg) {
    (void)arg;
    if (!g_watchdog_active || !g_pretimeout_fn) return;

    g_pretimeout_fired = 1;
    g_pretimeout_fn();
}

void watchdog_init(int timeout_seconds) {
    if (g_watchdog_active) {
        watchdog_stop();
    }

    if (timeout_seconds <= 0) timeout_seconds = 10;

    g_watchdog_timeout_ticks = (uint64_t)timeout_seconds * TIMER_FREQ;
    g_watchdog_active = 1;
    g_pretimeout_fired = 0;

    /* Schedule the initial watchdog timer */
    g_watchdog_timer_id = timer_schedule(watchdog_tick, NULL, g_watchdog_timeout_ticks);

    /* Schedule pretimeout if configured */
    if (g_pretimeout_secs > 0 && g_pretimeout_secs < timeout_seconds) {
        int pretimeout_ticks = (uint64_t)(timeout_seconds - g_pretimeout_secs) * TIMER_FREQ;
        g_pretimeout_timer_id = timer_schedule(watchdog_pretimeout_tick, NULL, pretimeout_ticks);
    }

    kprintf("[OK] Watchdog initialized (%d seconds timeout)\n", timeout_seconds);
}

void watchdog_pet(void) {
    if (!g_watchdog_active) return;

    /* Cancel the old timers and schedule new ones */
    if (g_watchdog_timer_id >= 0) {
        timer_cancel(g_watchdog_timer_id);
    }
    if (g_pretimeout_timer_id >= 0) {
        timer_cancel(g_pretimeout_timer_id);
        g_pretimeout_timer_id = -1;
    }

    g_watchdog_timer_id = timer_schedule(watchdog_tick, NULL, g_watchdog_timeout_ticks);

    /* Reset pretimeout fired flag */
    g_pretimeout_fired = 0;

    /* Re-schedule pretimeout if configured */
    if (g_pretimeout_secs > 0 && g_pretimeout_fn) {
        int timeout_secs = g_watchdog_timeout_ticks / TIMER_FREQ;
        if (g_pretimeout_secs < timeout_secs) {
            int pretimeout_ticks = (uint64_t)(timeout_secs - g_pretimeout_secs) * TIMER_FREQ;
            g_pretimeout_timer_id = timer_schedule(watchdog_pretimeout_tick, NULL, pretimeout_ticks);
        }
    }
}

void watchdog_stop(void) {
    if (!g_watchdog_active) return;

    g_watchdog_active = 0;
    if (g_watchdog_timer_id >= 0) {
        timer_cancel(g_watchdog_timer_id);
        g_watchdog_timer_id = -1;
    }
    if (g_pretimeout_timer_id >= 0) {
        timer_cancel(g_pretimeout_timer_id);
        g_pretimeout_timer_id = -1;
    }

    kprintf("[OK] Watchdog stopped\n");
}

void watchdog_set_pretimeout(int secs) {
    g_pretimeout_secs = secs;
}

void watchdog_set_pretimeout_fn(watchdog_pretimeout_fn_t fn) {
    g_pretimeout_fn = fn;
}
