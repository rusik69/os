#define KERNEL_INTERNAL
#include "types.h"
#include "nmi_watchdog.h"
#include "printf.h"
#include "timer.h"
#include "process.h"
#include "scheduler.h"
#include "vga.h"
#include "acpi.h"
#include "panic.h"
#include "apic.h"

static volatile uint64_t last_pet_tick = 0;
static int watchdog_running = 0;

/* Called from idle loop or timer interrupt to pet the watchdog */
void nmi_watchdog_pet(void) {
    last_pet_tick = timer_get_ticks();
}

void nmi_watchdog_start(void) {
    watchdog_running = 1;
    last_pet_tick = timer_get_ticks();
}

void nmi_watchdog_stop(void) {
    watchdog_running = 0;
}

int nmi_watchdog_available(void) {
    return 1; /* Always available — uses local APIC timer if possible */
}

/* NMI handler — called when watchdog timeout fires */
void nmi_watchdog_handler(void) {
    uint64_t now = timer_get_ticks();
    uint64_t elapsed_ms = (now - last_pet_tick) * 1000ULL / TIMER_FREQ;

    if (!watchdog_running) return;

    if (elapsed_ms >= WATCHDOG_TIMEOUT_MS) {
        vga_set_color(VGA_WHITE, VGA_RED);
        kprintf("\n=== WATCHDOG TIMEOUT ===\n");
        kprintf("Last pet %llu ms ago\n", (unsigned long long)elapsed_ms);

        /* Dump register state */
        dump_regs();
        dump_stack();

        /* Dump current process info */
        struct process *cur = process_get_current();
        if (cur) {
            kprintf("Current process: %s (pid=%u)\n",
                    cur->name ? cur->name : "?", cur->pid);
        }

        /* Dump task list */
        struct process *table = process_get_table();
        kprintf("PID  STATE  NAME\n");
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state == PROCESS_UNUSED) continue;
            const char *state_str = "?";
            switch (table[i].state) {
                case PROCESS_READY:   state_str = "RDY"; break;
                case PROCESS_RUNNING: state_str = "RUN"; break;
                case PROCESS_BLOCKED: state_str = "BLK"; break;
                case PROCESS_ZOMBIE:  state_str = "ZMB"; break;
                default: break;
            }
            kprintf(" %3u  %s  %s\n", table[i].pid, state_str,
                    table[i].name ? table[i].name : "?");
        }

        /* Pet to prevent re-trigger immediately */
        last_pet_tick = now;
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

void nmi_watchdog_init(void) {
    kprintf("[OK] NMI watchdog initialized (%d ms timeout)\n", WATCHDOG_TIMEOUT_MS);
}
