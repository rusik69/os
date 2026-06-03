/*
 * watchdog.c — Software watchdog timer and system reset
 *
 * Uses the dynamic timer subsystem to implement a software watchdog.
 * If watchdog_pet() is not called within the timeout period, the system
 * is rebooted via the keyboard controller reset port.
 * Supports a pretimeout callback that fires before the full timeout.
 *
 * Also provides watchdog_system_reset() — a multi-method machine reset
 * usable from interrupt-disabled contexts (e.g., panic).
 */

#include "watchdog.h"
#include "timers.h"
#include "timer.h"
#include "io.h"
#include "string.h"
#include "printf.h"
#include "smp.h"
#include "nmi_watchdog.h"

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

/* Pretimeout callback — fires g_pretimeout_secs before the full timeout.
 *
 * When a pretimeout fires:
 *   1. Send NMI IPI backtrace requests to all other CPUs so we capture
 *      their register state and stacks before the system resets.
 *   2. Log the timeout warning with the remaining margin.
 *   3. If a user-registered pretimeout function exists, call it as well
 *      (e.g. for panic+crashdump before watchdog reset).
 */
static void watchdog_pretimeout_tick(void *arg) {
    (void)arg;
    if (!g_watchdog_active) return;

    g_pretimeout_fired = 1;

    /* ── NMI backtrace: send IPI to all other CPUs ────────────── */
    kprintf("\n*** WATCHDOG PRETIMEOUT — system will reset in ~%d seconds ***\n",
            g_pretimeout_secs);
    kprintf("*** Triggering CPU backtrace before timeout... ***\n");

    /* Request backtrace IPI from all other CPUs to capture their state
     * before the watchdog fires.  This mirrors the lockup-detection
     * path in nmi_watchdog_handler / nmi_watchdog_check_soft. */
    if (smp_get_cpu_count() > 1)
        nmi_watchdog_request_backtrace();

    /* ── User-registered pretimeout handler ────────────────────── */
    if (g_pretimeout_fn) {
        g_pretimeout_fn();
    }
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

/*
 * watchdog_system_reset — Multi-method machine reset usable from panic
 *
 * Attempts the following reset methods in order:
 *   1. ACPI reset register (via acpi_reboot)
 *   2. Keyboard controller (0x64, 0xFE)
 *   3. Legacy chipset reset ports (0x604 BX_RST, 0xB004)
 *   4. Triple-fault by loading an IDT with zero limit
 *   5. Infinite halt as last resort
 *
 * This function never returns.
 */
__attribute__((noreturn))
void watchdog_system_reset(void)
{
    /* Attempt ACPI reset register (FADT RESET_REG) if available */
    extern int acpi_find_reset_register(void);
    if (acpi_find_reset_register()) {
        extern void acpi_reboot(void);
        acpi_reboot();
        /* If we're still alive, acpi_reboot failed — continue */
    }

    kprintf("watchdog: Trying keyboard controller reset...\n");

    /* Method 2: Keyboard controller (standard PC/AT reset) */
    cli();
    for (int i = 0; i < 3; i++) {
        outb(0x64, 0xFE);
        io_wait();
    }

    kprintf("watchdog: Trying legacy chipset reset ports...\n");

    /* Method 3: Legacy chipset reset ports (Bochs/QEMU, real hw) */
    outw(0x604, 0x2000); /* BX_RST — Bochs/QEMU reset port */
    io_wait();
    outw(0xB004, 0x2000); /* QEMU/KVM alternative */
    io_wait();
    outw(0xCF9, 0x06);    /* Intel ICH/PCH reset control — hard reset */
    io_wait();

    kprintf("watchdog: Trying triple-fault reset...\n");

    /* Method 4: Triple fault — load zero-length IDT and trigger an interrupt.
     * The CPU will attempt to read the IDT, get a limit=0 fault, which in
     * protected mode triggers a triple-fault and resets the machine. */
    __asm__ volatile(
        "movw $0, %%ax\n\t"
        "lidt (%%rax)\n\t"
        "int3\n\t"
        : : : "memory"
    );

    kprintf("watchdog: All reset methods failed — system halted\n");

    /* Last resort */
    for (;;) hlt();
}
