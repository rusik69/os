// SPDX-License-Identifier: GPL-2.0-only
/*
 * powercap.c — Powercap framework for RAPL
 *
 * Provides the powercap framework for reading and constraining
 * power consumption via RAPL (Running Average Power Limit) MSRs.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"

#define POWERCAP_MAX_ZONES 16

struct powercap_zone {
    int active;
    char name[32];
    uint64_t max_power_range_uw;
    uint64_t power_limit_uw;
    uint64_t energy_counter;
    uint64_t prev_energy_counter;
    uint64_t timestamp;
    int enabled;
};

static struct powercap_zone powercap_zones[POWERCAP_MAX_ZONES];
static int powercap_zone_count;
static spinlock_t powercap_lock;

/* Register a powercap zone */
int powercap_register_zone(const char *name, uint64_t max_power_uw)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&powercap_lock, &irq_flags);

    if (powercap_zone_count >= POWERCAP_MAX_ZONES) {
        spinlock_irqsave_release(&powercap_lock, irq_flags);
        return -ENOMEM;
    }

    struct powercap_zone *zone = &powercap_zones[powercap_zone_count];
    strncpy(zone->name, name, sizeof(zone->name) - 1);
    zone->max_power_range_uw = max_power_uw;
    zone->power_limit_uw = max_power_uw;
    zone->energy_counter = 0;
    zone->prev_energy_counter = 0;
    zone->timestamp = 0;
    zone->enabled = 1;
    zone->active = 1;
    powercap_zone_count++;

    spinlock_irqsave_release(&powercap_lock, irq_flags);

    kprintf("[POWERCAP] Zone '%s': max=%llu uW\n",
            name, (unsigned long long)max_power_uw);
    return powercap_zone_count - 1;
}

/* Set power limit */
int powercap_set_limit_uw(int zone_id, uint64_t limit_uw)
{
    if (zone_id < 0 || zone_id >= powercap_zone_count || !powercap_zones[zone_id].active)
        return -ENODEV;

    struct powercap_zone *zone = &powercap_zones[zone_id];
    if (limit_uw > zone->max_power_range_uw)
        limit_uw = zone->max_power_range_uw;

    zone->power_limit_uw = limit_uw;
    kprintf("[POWERCAP] Zone '%s': limit=%llu uW\n",
            zone->name, (unsigned long long)limit_uw);
    return 0;
}

/* Get power limit */
uint64_t powercap_get_limit_uw(int zone_id)
{
    if (zone_id < 0 || zone_id >= powercap_zone_count || !powercap_zones[zone_id].active)
        return 0;

    return powercap_zones[zone_id].power_limit_uw;
}

/* Update energy counter (call from RAPL interrupt or polling) */
void powercap_update_energy(int zone_id, uint64_t energy_uj)
{
    if (zone_id < 0 || zone_id >= powercap_zone_count || !powercap_zones[zone_id].active)
        return;

    struct powercap_zone *zone = &powercap_zones[zone_id];
    zone->prev_energy_counter = zone->energy_counter;
    zone->energy_counter = energy_uj;
    zone->timestamp = 0; /* timer_get_ticks() */
}

/* Get energy consumed */
uint64_t powercap_get_energy_uj(int zone_id)
{
    if (zone_id < 0 || zone_id >= powercap_zone_count || !powercap_zones[zone_id].active)
        return 0;

    return powercap_zones[zone_id].energy_counter;
}

void powercap_init(void)
{
    memset(powercap_zones, 0, sizeof(powercap_zones));
    powercap_zone_count = 0;
    spinlock_init(&powercap_lock);

    /* Register known RAPL domains (if RAPL is available) */
    /* Package domain: typically 0-250W range */
    /* Power plane domains (core, uncore, dram) */
    powercap_register_zone("package", 250000000); /* 250W */
    powercap_register_zone("core", 125000000);     /* 125W */
    powercap_register_zone("uncore", 125000000);   /* 125W */
    powercap_register_zone("dram", 50000000);       /* 50W */

    kprintf("[OK] Powercap — RAPL power capping framework (%d zones)\n",
            powercap_zone_count);
}
