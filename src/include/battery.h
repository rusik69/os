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

/* Initialise ACPI battery monitoring.
   Returns 0 on success, -1 if no battery interface available. */
int battery_init(void);

/* Get current battery status.
   Returns 0 on success, -1 on error. */
int battery_get_status(struct battery_status *status);

#endif /* BATTERY_H */
