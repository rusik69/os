/*
 * acpi_thermal.c — ACPI thermal zone driver with adaptive polling governor
 *
 * Reads thermal zone temperature and status from ACPI.
 * The polling rate governor dynamically adjusts the sampling interval
 * based on temperature trend:
 *   - Rising temperature → shorter poll interval (respond faster)
 *   - Falling temperature → gradually lengthen poll interval
 *   - Stable/low temperature → max poll interval (save CPU)
 *   - Approaching critical → minimum poll interval
 *
 * Temperature trend is computed from consecutive samples: if temp moves
 * by more than the hysteresis threshold in the same direction, the trend
 * is considered rising or falling.
 */

#include "acpi_thermal.h"
#include "acpi.h"
#include "io.h"
#include "printf.h"
#include "string.h"

#define MAX_THERMAL_ZONES       4
#define TEMP_TREND_HYSTERESIS   5   /* tenths K (0.5°C) to consider a trend change */
#define TREND_RISING            1
#define TREND_FALLING          (-1)
#define TREND_STABLE            0
#define DEFAULT_POLL_MS         1000
#define MIN_POLL_MS             100   /* every 100ms when near critical */
#define MAX_POLL_MS             10000 /* every 10s when cool and stable */

static struct acpi_thermal_zone g_thermal_zones[MAX_THERMAL_ZONES];
static int g_thermal_zone_count = 0;
static int g_thermal_init_done = 0;

/*
 * Compute the thermal polling governor decision for a zone.
 * Adjusts polling_ms based on temperature, trend, and proximity to limits.
 */
static void thermal_governor_tick(struct acpi_thermal_zone *zone, int new_temp)
{
    int prev = zone->temp;
    int diff = new_temp - prev;

    /* Update trend detection with hysteresis */
    if (diff > TEMP_TREND_HYSTERESIS) {
        if (zone->trend == TREND_RISING) {
            zone->consecutive_samples++;
        } else {
            zone->trend = TREND_RISING;
            zone->consecutive_samples = 1;
        }
    } else if (diff < -TEMP_TREND_HYSTERESIS) {
        if (zone->trend == TREND_FALLING) {
            zone->consecutive_samples++;
        } else {
            zone->trend = TREND_FALLING;
            zone->consecutive_samples = 1;
        }
    } else {
        /* Within hysteresis band — consider stable */
        if (zone->trend != TREND_STABLE) {
            zone->trend = TREND_STABLE;
            zone->consecutive_samples = 0;
        }
    }

    /* Adjust polling interval based on conditions */
    int proximity = 0; /* 0 = cool, 1 = warm, 2 = hot */

    if (zone->crit_temp > 0 && new_temp >= zone->crit_temp) {
        proximity = 2; /* at or above critical — poll as fast as possible */
    } else if (zone->passive_temp > 0 && new_temp >= zone->passive_temp) {
        proximity = 1; /* entering passive cooling range */
    }

    if (proximity == 2) {
        zone->polling_ms = zone->polling_ms_min;
    } else if (proximity == 1) {
        /* Hot: poll faster based on trend */
        if (zone->trend == TREND_RISING) {
            /* Rising toward critical — speed up */
            zone->polling_ms = zone->polling_ms > 500
                ? zone->polling_ms - 200
                : zone->polling_ms_min;
        } else if (zone->trend == TREND_FALLING) {
            /* Cooling down — gradually relax */
            zone->polling_ms = zone->polling_ms < 2000
                ? zone->polling_ms + 500
                : zone->polling_ms;
        } else {
            /* Stable in hot zone — moderate rate */
            zone->polling_ms = (zone->polling_ms < 1000) ? 1000
                : (zone->polling_ms > 3000) ? zone->polling_ms - 200
                : zone->polling_ms;
        }
    } else {
        /* Normal temperature range */
        if (zone->trend == TREND_RISING) {
            /* Temperature rising but still cool — increase vigilance */
            zone->polling_ms = zone->polling_ms > 2000
                ? zone->polling_ms - 500
                : zone->polling_ms;
        } else if (zone->trend == TREND_FALLING) {
            /* Cooling — can relax, slow down polling */
            zone->polling_ms = zone->polling_ms < zone->polling_ms_max
                ? zone->polling_ms + 1000
                : zone->polling_ms_max;
        } else if (zone->consecutive_samples >= 5) {
            /* Consistently stable at a safe temperature — go to max interval */
            zone->polling_ms = zone->polling_ms < zone->polling_ms_max
                ? zone->polling_ms + 500
                : zone->polling_ms_max;
        }
    }

    /* Clamp to configured min/max */
    if (zone->polling_ms < zone->polling_ms_min)
        zone->polling_ms = zone->polling_ms_min;
    if (zone->polling_ms > zone->polling_ms_max)
        zone->polling_ms = zone->polling_ms_max;

    /* Store previous temperature and update current */
    zone->prev_temp = zone->temp;
    zone->temp = new_temp;
}

/*
 * Reads a temperature from an ACPI thermal zone.
 * In a real system this would evaluate _TMP via AML.
 * For now, we return a simulated 30°C (303.1 K) * 10 = 3031
 * with slight random variation for testing the governor.
 */
static int thermal_read_temp(int zone_idx)
{
    (void)zone_idx;
    /* Simulate around 30°C = 3031 dK with a ±2 dK variation */
    static int base = 3031;
    base += (base % 7) - 3; /* pseudo-random walk */
    if (base < 3020) base = 3020;
    if (base > 3050) base = 3050;
    return base;
}

int acpi_thermal_init(void)
{
    if (g_thermal_init_done)
        return 0;

    memset(g_thermal_zones, 0, sizeof(g_thermal_zones));

    /* Create one simulated thermal zone with governor parameters */
    struct acpi_thermal_zone *zone = &g_thermal_zones[0];
    zone->present = 1;
    memcpy(zone->name, "CPUZ\0", 5);
    zone->temp = thermal_read_temp(0);
    zone->prev_temp = zone->temp;
    zone->crit_temp = 3731;     /* ~100°C critical */
    zone->passive_temp = 3581;  /* ~85°C passive */
    zone->polling_ms = DEFAULT_POLL_MS;
    zone->polling_ms_min = MIN_POLL_MS;
    zone->polling_ms_max = MAX_POLL_MS;
    zone->trend = TREND_STABLE;
    zone->consecutive_samples = 0;
    g_thermal_zone_count = 1;

    kprintf("[ACPI_TZ] Initialized: zone 0 \"%s\" temp=%d (%d.%d°C) "
            "poll=[%d..%d]ms\n",
            zone->name, zone->temp,
            zone->temp / 10 - 273, (zone->temp % 10) ? (zone->temp % 10) : 0,
            zone->polling_ms_min, zone->polling_ms_max);

    g_thermal_init_done = 1;
    return 0;
}

int acpi_thermal_get_temp(int zone_idx, int *temp_k)
{
    if (!g_thermal_init_done || zone_idx < 0 || zone_idx >= MAX_THERMAL_ZONES)
        return -1;
    if (!g_thermal_zones[zone_idx].present || !temp_k)
        return -1;

    struct acpi_thermal_zone *zone = &g_thermal_zones[zone_idx];
    int new_temp = thermal_read_temp(zone_idx);

    /* Run the polling governor before returning */
    thermal_governor_tick(zone, new_temp);

    *temp_k = zone->temp;
    return 0;
}

int acpi_thermal_get_state(int zone_idx)
{
    if (!g_thermal_init_done || zone_idx < 0 || zone_idx >= MAX_THERMAL_ZONES)
        return -1;
    if (!g_thermal_zones[zone_idx].present)
        return -1;

    int temp = g_thermal_zones[zone_idx].temp;
    if (temp >= g_thermal_zones[zone_idx].crit_temp)
        return ACPI_THERMAL_CRIT;
    if (temp >= g_thermal_zones[zone_idx].passive_temp)
        return ACPI_THERMAL_HOT;
    return ACPI_THERMAL_OK;
}

int acpi_thermal_zone_count(void)
{
    return g_thermal_zone_count;
}

/* Get the current polling interval for a zone (in ms) */
int acpi_thermal_get_polling_ms(int zone_idx)
{
    if (!g_thermal_init_done || zone_idx < 0 || zone_idx >= MAX_THERMAL_ZONES)
        return -1;
    if (!g_thermal_zones[zone_idx].present)
        return -1;
    return g_thermal_zones[zone_idx].polling_ms;
}

void acpi_thermal_print_info(void)
{
    kprintf("ACPI Thermal Zones: %d\n", g_thermal_zone_count);
    for (int i = 0; i < g_thermal_zone_count; i++) {
        struct acpi_thermal_zone *z = &g_thermal_zones[i];
        if (!z->present) continue;
        const char *trend_str = "stable";
        if (z->trend == TREND_RISING)  trend_str = "rising";
        if (z->trend == TREND_FALLING) trend_str = "falling";
        kprintf("  Zone %d: %s temp=%d (%d.%d°C) crit=%d pass=%d "
                "poll=%dms trend=%s samples=%d\n",
                i, z->name, z->temp,
                z->temp / 10 - 273, z->temp % 10,
                z->crit_temp, z->passive_temp,
                z->polling_ms, trend_str, z->consecutive_samples);
    }
}
