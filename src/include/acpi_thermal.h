#ifndef ACPI_THERMAL_H
#define ACPI_THERMAL_H

#include "types.h"

/* ACPI thermal zone states */
#define ACPI_THERMAL_OK      0
#define ACPI_THERMAL_HOT     1
#define ACPI_THERMAL_CRIT    2

/* Thermal zone descriptors */
struct acpi_thermal_zone {
    int   present;
    char  name[16];
    int   temp;           /* Current temperature in tenths of Kelvin */
    int   prev_temp;      /* Previous temperature reading for trend */
    int   crit_temp;      /* Critical temperature (tenths K) */
    int   passive_temp;   /* Passive cooling temperature (tenths K) */
    int   polling_ms;     /* Current polling interval in ms */
    int   polling_ms_min; /* Minimum polling interval (e.g. 100ms when hot) */
    int   polling_ms_max; /* Maximum polling interval (e.g. 10000ms when cool) */
    int   trend;          /* Temperature trend: -1 falling, 0 stable, +1 rising */
    int   consecutive_samples; /* Count of samples in current trend direction */
};

/* API */
int  acpi_thermal_init(void);
int  acpi_thermal_get_temp(int zone, int *temp_k);
int  acpi_thermal_get_state(int zone);
int  acpi_thermal_get_polling_ms(int zone);
void acpi_thermal_print_info(void);
int  acpi_thermal_zone_count(void);

#endif /* ACPI_THERMAL_H */
