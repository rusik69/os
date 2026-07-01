#ifndef BATTERY_H
#define BATTERY_H

#include "types.h"

/* Battery status */
struct battery_status {
    int present;           /* 1 if battery detected */
    int charging;          /* 1 if charging, 0 if discharging */
    int percentage;        /* remaining capacity in percent (0..100) */
    uint32_t voltage;      /* current voltage in mV */
    uint32_t rate;         /* charge/discharge rate in mW */
};

/* Battery health / aging information (Item 107) */
struct battery_health {
    int    present;              /* 1 if battery / info available */
    uint32_t design_capacity;    /* mAh — theoretical max when new (from _BIF) */
    uint32_t full_charge_capacity; /* mAh — actual full capacity now, adjusted for aging */
    uint32_t current_capacity;   /* mAh — current remaining charge */
    int     wear_level_pct;      /* 0..100 — (1 - fcc/design) * 100 */
    int     cycle_count;         /* charge/discharge cycles (0 if unknown) */
};

/* Battery extended info from ACPI _BIF */
struct battery_info {
    uint32_t power_unit;         /* 0 = mWh, 1 = mAh */
    uint32_t design_capacity;    /* mAh or mWh */
    uint32_t last_full_charge;   /* last full charge capacity */
    uint32_t battery_technology; /* 0 = rechargeable, 1 = non-rechargeable */
    uint32_t design_voltage;     /* mV */
    uint32_t warn_capacity;      /* design capacity of warning */
    uint32_t low_capacity;       /* design capacity of low */
    char     model_number[64];   /* model number string */
    char     serial_number[64];  /* serial number string */
    char     battery_type[64];   /* battery type string */
    char     oem_info[64];       /* OEM info string */
};

/* Initialise ACPI battery monitoring.
   Returns 0 on success, -1 if no battery interface available. */
int battery_init(void);

/* Get current battery status.
   Returns 0 on success, -1 on error. */
int battery_get_status(struct battery_status *status);

/* Get battery health / aging information (Item 107).
 * Fills in design_capacity, full_charge_capacity, wear_level_pct, etc.
 * Returns 0 on success, -1 if no battery / info not available. */
int battery_get_health(struct battery_health *health);

/* Get detailed battery info from ACPI _BIF method.
 * Fills in power_unit, design_capacity, model_number, serial_number, etc.
 * Returns 0 on success, -1 if not available. */
int battery_get_info(int id, struct battery_info *info);

/* Set ACPI _BTP (Battery Trip Point).
 * Sets a capacity threshold (in mAh/mWh) that generates a notification
 * when the battery's remaining capacity crosses it.
 * Returns 0 on success, -1 if not supported. */
int battery_set_trip_point(uint32_t capacity);

#endif /* BATTERY_H */
