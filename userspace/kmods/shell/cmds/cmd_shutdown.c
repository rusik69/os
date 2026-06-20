/* cmd_shutdown.c — Shutdown and reboot with service termination
 *
 * U5: Enhanced shutdown with service cleanup.
 *
 * Usage:
 *   shutdown -h now   — halt the system (power off via ACPI)
 *   shutdown -r now   — reboot the system
 *   reboot            — reboot the system
 *
 * Before halting/rebooting, all running services are stopped and
 * filesystem buffers are flushed to disk.
 */

#include "shell_cmds.h"
#include "shell.h"
#include "printf.h"
#include "string.h"
#include "service.h"
#include "fat32.h"
#include "timer.h"

/* ── Forward declarations for ACPI functions ────────────────────────── */
extern void acpi_shutdown(void);
extern void acpi_reboot(void);

/* ── Helper: synchronize filesystem ────────────────────────────────── */
static void do_sync(void)
{
    if (fat32_is_mounted()) {
        int ret = fat32_sync();
        if (ret == 0)
            kprintf("shutdown: filesystem synced\n");
        else
            kprintf("shutdown: sync returned %d\n", ret);
    } else {
        /* Also try VFS-level sync if available */
        /* For now, just log that there's nothing to sync */;
    }
}

/* ── Helper: stop all running services ──────────────────────────────── */
static void stop_all_services(void)
{
    int n = service_count();
    if (n == 0) {
        kprintf("shutdown: no services to stop\n");
        return;
    }

    kprintf("shutdown: stopping %d service(s)...\n", n);

    /* Stop services in reverse registration order (last registered first) */
    for (int i = n - 1; i >= 0; i--) {
        struct service *svc = service_get(i);
        if (svc && svc->state == SERVICE_RUNNING) {
            kprintf("shutdown: stopping '%s'...\n", svc->name);
            service_stop(svc->name);
        }
    }
}

/* Parse +N minutes from a time spec string. Returns delay in ticks, 0 if not valid. */
static uint64_t parse_delay_minutes(const char *p)
{
    if (!p || p[0] != '+') return 0;
    p++; /* skip '+' */
    int minutes = 0;
    while (*p >= '0' && *p <= '9') {
        minutes = minutes * 10 + (*p - '0');
        p++;
    }
    if (minutes <= 0) return 0;
    /* Convert minutes to ticks */
    return (uint64_t)minutes * 60 * TIMER_FREQ;
}

/* ── Main shutdown command ──────────────────────────────────────────── */
void cmd_shutdown(const char *args)
{
    int do_reboot = 0;

    /* Parse arguments */
    if (args && args[0]) {
        const char *p = args;
        while (*p == ' ') p++;

        if (*p == '-') {
            p++;
            if (*p == 'h' || *p == 'H') {
                do_reboot = 0;  /* halt */
            } else if (*p == 'r' || *p == 'R') {
                do_reboot = 1;  /* reboot */
            } else {
                kprintf("shutdown: unknown option '-%c' (use -h to halt, -r to reboot)\n", *p);
                return;
            }
            p++;
            while (*p == ' ') p++;

            /* Check for "now" keyword (we ignore other time specs for now) */
            if (strcmp(p, "now") != 0 && p[0] != '\0') {
                /* Accept it as informational but do it now anyway */
                kprintf("shutdown: ignoring time specification, shutting down now\n");
            }
        } else if (strcmp(p, "now") == 0) {
            /* Plain "shutdown now" — halt by default */;
        } else {
            /* Check for +N format (delayed) — implement timer-based delay */
            if (p[0] == '+') {
                uint64_t delay_ticks = parse_delay_minutes(p);
                if (delay_ticks > 0) {
                    char *end = NULL;
                    int minutes = (int)strtol(p + 1, &end, 10);
                    kprintf("shutdown: delaying %d minute(s)...\n", minutes > 0 ? minutes : 0);
                    /* Wait using timer ticks (yield to other processes) */
                    uint64_t deadline = timer_get_ticks() + delay_ticks;
                    while (timer_get_ticks() < deadline) {
                        /* Yield CPU to other processes while waiting */
                        __asm__ volatile("pause");
                    }
                } else {
                    kprintf("shutdown: invalid delay '%s', shutting down now\n", p);
                }
            }
            /* Anything else: treat as message or unknown */
        }
    }

    kprintf("shutdown: commencing %s...\n",
            do_reboot ? "reboot" : "system halt");

    /* Step 1: Stop all services */
    stop_all_services();

    /* Step 2: Sync filesystem */
    do_sync();

    /* Step 3: Halt or reboot */
    if (do_reboot) {
        kprintf("shutdown: rebooting...\n");
        kprintf_flush();
        /* Small delay to let serial output complete */
        for (volatile int d = 0; d < 1000000; d++);
        acpi_reboot();
    } else {
        kprintf("shutdown: powering off...\n");
        kprintf_flush();
        /* Small delay to let serial output complete */
        for (volatile int d = 0; d < 1000000; d++);
        acpi_shutdown();
    }

    /* Should not reach here */
    for (;;) __asm__ volatile("hlt");
}
