/*
 * acpi_thermal.c — ACPI thermal zone driver
 *
 * Reads thermal zone temperature and status from ACPI.
 * In a real system, this parses _TMP from DSDT/SSDT.
 * This implementation provides a framework with simulated values
 * for testing, and hooks for real ACPI thermal zones.
 */

#include "acpi_thermal.h"
#include "acpi.h"
#include "io.h"
#include "printf.h"
#include "string.h"

#define MAX_THERMAL_ZONES 4

static struct acpi_thermal_zone g_thermal_zones[MAX_THERMAL_ZONES];
static int g_thermal_zone_count = 0;
static int g_thermal_init_done = 0;

/*
 * Reads a temperature from an ACPI thermal zone.
 * In a real system this would evaluate _TMP via AML.
 * For now, we return a simulated 30°C (303.1 K) * 10 = 3031.
 */
static int thermal_read_temp(int zone_idx) {
    (void)zone_idx;
    /* Return 30°C = 3031 tenths of Kelvin */
    return 3031;
}

int acpi_thermal_init(void) {
    if (g_thermal_init_done)
        return 0;

    memset(g_thermal_zones, 0, sizeof(g_thermal_zones));

    /* Create one simulated thermal zone */
    struct acpi_thermal_zone *zone = &g_thermal_zones[0];
    zone->present = 1;
    memcpy(zone->name, "CPUZ\0", 5);
    zone->temp = thermal_read_temp(0);
    zone->crit_temp = 3731;  /* ~100°C critical */
    zone->passive_temp = 3581; /* ~85°C passive */
    zone->polling_ms = 1000;
    g_thermal_zone_count = 1;

    kprintf("[ACPI_TZ] Initialized: zone 0 \"%s\" temp=%d (%d.%d°C)\n",
            zone->name, zone->temp,
            zone->temp / 10 - 273, (zone->temp % 10) ? (zone->temp % 10) : 0);

    g_thermal_init_done = 1;
    return 0;
}

int acpi_thermal_get_temp(int zone, int *temp_k) {
    if (!g_thermal_init_done || zone < 0 || zone >= MAX_THERMAL_ZONES)
        return -1;
    if (!g_thermal_zones[zone].present || !temp_k)
        return -1;

    g_thermal_zones[zone].temp = thermal_read_temp(zone);
    *temp_k = g_thermal_zones[zone].temp;
    return 0;
}

int acpi_thermal_get_state(int zone) {
    if (!g_thermal_init_done || zone < 0 || zone >= MAX_THERMAL_ZONES)
        return -1;
    if (!g_thermal_zones[zone].present)
        return -1;

    int temp = g_thermal_zones[zone].temp;
    if (temp >= g_thermal_zones[zone].crit_temp)
        return ACPI_THERMAL_CRIT;
    if (temp >= g_thermal_zones[zone].passive_temp)
        return ACPI_THERMAL_HOT;
    return ACPI_THERMAL_OK;
}

int acpi_thermal_zone_count(void) {
    return g_thermal_zone_count;
}

void acpi_thermal_print_info(void) {
    kprintf("ACPI Thermal Zones: %d\n", g_thermal_zone_count);
    for (int i = 0; i < g_thermal_zone_count; i++) {
        struct acpi_thermal_zone *z = &g_thermal_zones[i];
        if (!z->present) continue;
        kprintf("  Zone %d: %s temp=%d (%d.%d°C) crit=%d pass=%d\n",
                i, z->name, z->temp,
                z->temp / 10 - 273, z->temp % 10,
                z->crit_temp, z->passive_temp);
    }
}
