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
 *
 * ACPI thermal zone methods supported:
 *   _TMP  — Get current temperature (evaluated periodically)
 *   _PSV  — Passive cooling threshold temperature
 *   _AC0–_AC9 — Active cooling thresholds (10 levels)
 *   _CRT  — Critical temperature (system shutdown if reached)
 *   _HOT  — Hot temperature (may trigger OS-specific action)
 *
 * Trip point handling: when a threshold is crossed, the corresponding
 * cooling action is triggered (fan on, throttle CPU, emergency shutdown).
 *
 * Item 121 — ACPI thermal zone with _TMP, _PSV, _AC0-_AC9, _CRT
 */

#include "acpi_thermal.h"
#include "acpi.h"
#include "timers.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

#define MAX_THERMAL_ZONES       4
#define TEMP_TREND_HYSTERESIS   5   /* tenths K (0.5°C) to consider a trend change */
#define TREND_RISING            1
#define TREND_FALLING          (-1)
#define TREND_STABLE            0
#define DEFAULT_POLL_MS         1000
#define MIN_POLL_MS             100   /* every 100ms when near critical */
#define MAX_POLL_MS             10000 /* every 10s when cool and stable */

/* ── ACPI thermal methods ──────────────────────────────────────────── */
#define ACPI_ALERT_TMP    "_TMP"   /* Temperature */
#define ACPI_ALERT_PSV    "_PSV"   /* Passive threshold */
#define ACPI_ALERT_CRT    "_CRT"   /* Critical threshold */
#define ACPI_ALERT_HOT    "_HOT"   /* Hot threshold */
#define ACPI_ALERT_AC0    "_AC0"   /* Active cooling 0 */
#define ACPI_ALERT_AC1    "_AC1"   /* Active cooling 1 */
#define ACPI_ALERT_AC2    "_AC2"   /* Active cooling 2 */
#define ACPI_ALERT_AC3    "_AC3"   /* Active cooling 3 */
#define ACPI_ALERT_AC4    "_AC4"   /* Active cooling 4 */
#define ACPI_ALERT_AC5    "_AC5"   /* Active cooling 5 */
#define ACPI_ALERT_AC6    "_AC6"   /* Active cooling 6 */
#define ACPI_ALERT_AC7    "_AC7"   /* Active cooling 7 */
#define ACPI_ALERT_AC8    "_AC8"   /* Active cooling 8 */
#define ACPI_ALERT_AC9    "_AC9"   /* Active cooling 9 */

/* ── Trip point types ──────────────────────────────────────────────── */
#define TRIP_POINT_CRITICAL  0
#define TRIP_POINT_HOT       1
#define TRIP_POINT_PASSIVE   2
#define TRIP_POINT_ACTIVE    3   /* _AC0 through _AC9 */

struct trip_point {
    int type;          /* TRIP_POINT_* */
    int temperature;   /* temperature in tenths Kelvin */
    int enabled;       /* 1 if this trip point is active */
    int crossed;       /* 1 if the temperature has crossed this point */
    char method[8];    /* ACPI method name, e.g. "_AC0" */
};

/* ── Extended thermal zone state with trip points ──────────────────── */

struct thermal_zone_ext {
    struct acpi_thermal_zone base;
    struct trip_point trip_points[12]; /* _CRT, _HOT, _PSV, _AC0-_AC9 */
    int num_trip_points;
    int active_cooling_level;  /* Current active cooling level (0-9, -1=off) */
    int passive_cooling_active; /* 1 if passive cooling (throttling) is engaged */
};

static struct thermal_zone_ext g_thermal_zones_ext[MAX_THERMAL_ZONES];
#define zone_ext(i) (&g_thermal_zones_ext[i])
#define zone(i)     (&g_thermal_zones_ext[i].base)

static int g_thermal_zone_count = 0;
static int g_thermal_init_done = 0;
static int g_thermal_timer_id = -1;

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

/*
 * Evaluate an ACPI thermal method for a zone.
 * In a real system this would call acpi_evaluate_method().
 * For simulation, we return default values.
 */
static int thermal_evaluate_method(int zone_idx, const char *method, int *result)
{
    (void)zone_idx;
    /* Check known ACPI thermal methods */
    if (strcmp(method, "_TMP") == 0) {
        *result = thermal_read_temp(zone_idx);
        return 0;
    }
    if (strcmp(method, "_PSV") == 0) {
        *result = 3581; /* ~85°C passive threshold */
        return 0;
    }
    if (strcmp(method, "_CRT") == 0) {
        *result = 3731; /* ~100°C critical threshold */
        return 0;
    }
    if (strcmp(method, "_HOT") == 0) {
        *result = 3681; /* ~95°C hot threshold */
        return 0;
    }
    if (strcmp(method, "_AC0") == 0) { *result = 3281; return 0; } /* ~55°C */
    if (strcmp(method, "_AC1") == 0) { *result = 3331; return 0; } /* ~60°C */
    if (strcmp(method, "_AC2") == 0) { *result = 3381; return 0; } /* ~65°C */
    if (strcmp(method, "_AC3") == 0) { *result = 3431; return 0; } /* ~70°C */
    if (strcmp(method, "_AC4") == 0) { *result = 3481; return 0; } /* ~75°C */
    if (strcmp(method, "_AC5") == 0) { *result = 3531; return 0; } /* ~80°C */
    if (strcmp(method, "_AC6") == 0) { *result = 3581; return 0; } /* ~85°C */
    if (strcmp(method, "_AC7") == 0) { *result = 3631; return 0; } /* ~90°C */
    if (strcmp(method, "_AC8") == 0) { *result = 3681; return 0; } /* ~95°C */
    if (strcmp(method, "_AC9") == 0) { *result = 3701; return 0; } /* ~97°C */

    return -1; /* Method not found */
}

/*
 * Check if a trip point has been crossed and handle it.
 * Returns 1 if the trip point was just crossed, 0 otherwise.
 */
static int thermal_check_trip_point(struct thermal_zone_ext *tze,
                                     struct trip_point *tp, int current_temp)
{
    if (!tp->enabled)
        return 0;

    if (current_temp >= tp->temperature && !tp->crossed) {
        tp->crossed = 1;
        kprintf("[ACPI_TZ] Trip point %s crossed at %d (%d.%d°C)\n",
                tp->method, current_temp,
                current_temp / 10 - 273, current_temp % 10);

        if (tp->type == TRIP_POINT_CRITICAL) {
            kprintf("[ACPI_TZ] CRITICAL temperature! Emergency shutdown!\n");
            /* In a real system: initiate emergency shutdown */
        } else if (tp->type == TRIP_POINT_HOT) {
            kprintf("[ACPI_TZ] HOT temperature! Taking action...\n");
        } else if (tp->type == TRIP_POINT_PASSIVE) {
            tze->passive_cooling_active = 1;
            kprintf("[ACPI_TZ] Passive cooling engaged (CPU throttling)\n");
        } else if (tp->type == TRIP_POINT_ACTIVE) {
            int level = tp->method[3] - '0'; /* Extract level from _ACn */
            if (level > tze->active_cooling_level) {
                tze->active_cooling_level = level;
                kprintf("[ACPI_TZ] Active cooling level %d engaged\n", level);
            }
        }
        return 1;
    }

    /* Deactivate when temperature drops below threshold with hysteresis */
    if (current_temp < tp->temperature - TEMP_TREND_HYSTERESIS && tp->crossed) {
        tp->crossed = 0;

        if (tp->type == TRIP_POINT_PASSIVE) {
            tze->passive_cooling_active = 0;
            kprintf("[ACPI_TZ] Passive cooling deactivated\n");
        } else if (tp->type == TRIP_POINT_ACTIVE) {
            int level = tp->method[3] - '0';
            if (level >= tze->active_cooling_level) {
                tze->active_cooling_level = level > 0 ? level - 1 : -1;
                kprintf("[ACPI_TZ] Active cooling level %d deactivated\n", level);
            }
        }
        return 1;
    }

    return 0;
}

/*
 * Evaluate all trip points for a thermal zone.
 */
static void thermal_evaluate_trip_points(struct thermal_zone_ext *tze, int temp)
{
    for (int i = 0; i < tze->num_trip_points; i++) {
        thermal_check_trip_point(tze, &tze->trip_points[i], temp);
    }
}

/* Periodic timer callback for thermal polling */
static void thermal_timer_cb(void *arg);

/*
 * Initialize a thermal zone's trip points from ACPI methods.
 */
static void thermal_init_trip_points(struct thermal_zone_ext *tze)
{
    int idx = 0;
    struct {
        const char *method;
        int type;
    } methods[] = {
        {"_CRT", TRIP_POINT_CRITICAL},
        {"_HOT", TRIP_POINT_HOT},
        {"_PSV", TRIP_POINT_PASSIVE},
        {"_AC0", TRIP_POINT_ACTIVE},
        {"_AC1", TRIP_POINT_ACTIVE},
        {"_AC2", TRIP_POINT_ACTIVE},
        {"_AC3", TRIP_POINT_ACTIVE},
        {"_AC4", TRIP_POINT_ACTIVE},
        {"_AC5", TRIP_POINT_ACTIVE},
        {"_AC6", TRIP_POINT_ACTIVE},
        {"_AC7", TRIP_POINT_ACTIVE},
        {"_AC8", TRIP_POINT_ACTIVE},
        {"_AC9", TRIP_POINT_ACTIVE},
    };

    for (uint32_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
        if (idx >= (int)(sizeof(tze->trip_points) / sizeof(tze->trip_points[0])))
            break;

        int temp;
        if (thermal_evaluate_method(0, methods[m].method, &temp) == 0) {
            struct trip_point *tp = &tze->trip_points[idx];
            tp->type = methods[m].type;
            tp->temperature = temp;
            tp->enabled = 1;
            tp->crossed = 0;
            strncpy(tp->method, methods[m].method, sizeof(tp->method) - 1);
            tp->method[sizeof(tp->method) - 1] = '\0';
            idx++;
        }
    }

    tze->num_trip_points = idx;
    tze->active_cooling_level = -1;
    tze->passive_cooling_active = 0;
}

int acpi_thermal_init(void)
{
    if (g_thermal_init_done)
        return 0;

    memset(g_thermal_zones_ext, 0, sizeof(g_thermal_zones_ext));

    /* Create one simulated thermal zone with governor parameters */
    struct thermal_zone_ext *tze = &g_thermal_zones_ext[0];
    struct acpi_thermal_zone *z = &tze->base;
    z->present = 1;
    memcpy(z->name, "CPUZ\0", 5);
    z->temp = thermal_read_temp(0);
    z->prev_temp = z->temp;
    z->crit_temp = 3731;     /* ~100°C critical */
    z->passive_temp = 3581;  /* ~85°C passive */
    z->polling_ms = DEFAULT_POLL_MS;
    z->polling_ms_min = MIN_POLL_MS;
    z->polling_ms_max = MAX_POLL_MS;
    z->trend = TREND_STABLE;
    z->consecutive_samples = 0;

    /* Initialize trip points from ACPI thermal methods */
    thermal_init_trip_points(tze);

    g_thermal_zone_count = 1;

    kprintf("[ACPI_TZ] Initialized: zone 0 \"%s\" temp=%d (%d.%d°C) "
            "poll=[%d..%d]ms, %d trip points\n",
            z->name, z->temp,
            z->temp / 10 - 273, (z->temp % 10) ? (z->temp % 10) : 0,
            z->polling_ms_min, z->polling_ms_max,
            tze->num_trip_points);

    /* Start periodic timer for thermal polling */
    g_thermal_timer_id = timer_schedule(thermal_timer_cb, NULL,
                                         (uint64_t)(z->polling_ms * TIMER_FREQ / 1000));

    g_thermal_init_done = 1;
    return 0;
}

static void thermal_timer_cb(void *arg)
{
    (void)arg;
    if (!g_thermal_init_done) return;

    /* Poll each zone */
    for (int i = 0; i < g_thermal_zone_count; i++) {
        struct thermal_zone_ext *tze = &g_thermal_zones_ext[i];
        if (!tze->base.present) continue;

        int new_temp;
        if (thermal_evaluate_method(i, "_TMP", &new_temp) == 0) {
            /* Run governor */
            int old_temp = tze->base.temp;
            tze->base.prev_temp = old_temp;
            tze->base.temp = new_temp;

            /* Re-run the governor decision */
            int diff = new_temp - old_temp;
            if (diff > TEMP_TREND_HYSTERESIS) {
                if (tze->base.trend == TREND_RISING)
                    tze->base.consecutive_samples++;
                else {
                    tze->base.trend = TREND_RISING;
                    tze->base.consecutive_samples = 1;
                }
            } else if (diff < -TEMP_TREND_HYSTERESIS) {
                if (tze->base.trend == TREND_FALLING)
                    tze->base.consecutive_samples++;
                else {
                    tze->base.trend = TREND_FALLING;
                    tze->base.consecutive_samples = 1;
                }
            } else {
                if (tze->base.trend != TREND_STABLE) {
                    tze->base.trend = TREND_STABLE;
                    tze->base.consecutive_samples = 0;
                }
            }

            /* Evaluate trip points */
            thermal_evaluate_trip_points(tze, new_temp);
        }
    }

    /* Reschedule */
    struct acpi_thermal_zone *z = &g_thermal_zones_ext[0].base;
    g_thermal_timer_id = timer_schedule(thermal_timer_cb, NULL,
                                         (uint64_t)(z->polling_ms * TIMER_FREQ / 1000));
}

int acpi_thermal_get_temp(int zone_idx, int *temp_k)
{
    if (!g_thermal_init_done || zone_idx < 0 || zone_idx >= MAX_THERMAL_ZONES)
        return -1;
    if (!g_thermal_zones_ext[zone_idx].base.present || !temp_k)
        return -1;

    struct thermal_zone_ext *tze = &g_thermal_zones_ext[zone_idx];
    int new_temp = thermal_read_temp(zone_idx);

    /* Run the polling governor before returning */
    thermal_governor_tick(&tze->base, new_temp);

    /* Evaluate trip points */
    thermal_evaluate_trip_points(tze, new_temp);

    *temp_k = tze->base.temp;
    return 0;
}

int acpi_thermal_get_state(int zone_idx)
{
    if (!g_thermal_init_done || zone_idx < 0 || zone_idx >= MAX_THERMAL_ZONES)
        return -1;
    if (!g_thermal_zones_ext[zone_idx].base.present)
        return -1;

    int temp = g_thermal_zones_ext[zone_idx].base.temp;
    if (temp >= g_thermal_zones_ext[zone_idx].base.crit_temp)
        return ACPI_THERMAL_CRIT;
    if (temp >= g_thermal_zones_ext[zone_idx].base.passive_temp)
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
    if (!g_thermal_zones_ext[zone_idx].base.present)
        return -1;
    return g_thermal_zones_ext[zone_idx].base.polling_ms;
}

void acpi_thermal_print_info(void)
{
    kprintf("ACPI Thermal Zones: %d\n", g_thermal_zone_count);
    for (int i = 0; i < g_thermal_zone_count; i++) {
        struct thermal_zone_ext *tze = &g_thermal_zones_ext[i];
        struct acpi_thermal_zone *z = &tze->base;
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

        /* Print trip points */
        kprintf("    Trip points (%d):\n", tze->num_trip_points);
        for (int j = 0; j < tze->num_trip_points; j++) {
            struct trip_point *tp = &tze->trip_points[j];
            const char *type_str = "unknown";
            switch (tp->type) {
                case TRIP_POINT_CRITICAL: type_str = "CRIT"; break;
                case TRIP_POINT_HOT:      type_str = "HOT"; break;
                case TRIP_POINT_PASSIVE:  type_str = "PSV"; break;
                case TRIP_POINT_ACTIVE:   type_str = "ACT"; break;
            }
            kprintf("      [%d] %s %s: %d (%s)\n",
                    j, tp->method, type_str, tp->temperature,
                    tp->crossed ? "crossed" : "active");
        }
        kprintf("    Active cooling level: %d, Passive cooling: %s\n",
                tze->active_cooling_level,
                tze->passive_cooling_active ? "YES" : "no");
    }
}

/* ── Stub: acpi_thermal_set_policy ─────────────────────────────── */
int acpi_thermal_set_policy(void *dev, int policy)
{
    (void)dev;
    (void)policy;
    kprintf("[acpi] acpi_thermal_set_policy: not yet implemented\n");
    return -ENOSYS;
}
