#include "battery.h"
#include "printf.h"

/* ACPI battery stub — no ACPI control method evaluation support yet.
   Always reports "no battery". */

static int g_battery_init = 0;

int battery_init(void) {
    g_battery_init = 1;
    kprintf("[--] Battery: ACPI battery monitoring not available (stub)\n");
    return -1;
}

int battery_get_status(struct battery_status *status) {
    if (!status) return -1;

    status->present = 0;
    status->charging = 0;
    status->percentage = 0;
    status->voltage = 0;
    status->rate = 0;

    if (!g_battery_init) return -1;
    return 0;
}
