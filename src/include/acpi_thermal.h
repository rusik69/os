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
    int   crit_temp;      /* Critical temperature (tenths K) */
    int   passive_temp;   /* Passive cooling temperature (tenths K) */
    int   polling_ms;     /* Polling interval in ms */
};

/* API */
int  acpi_thermal_init(void);
int  acpi_thermal_get_temp(int zone, int *temp_k);
int  acpi_thermal_get_state(int zone);
void acpi_thermal_print_info(void);
int  acpi_thermal_zone_count(void);

#endif /* ACPI_THERMAL_H */
