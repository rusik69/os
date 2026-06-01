#ifndef ACPI_H
#define ACPI_H

#include "types.h"

void acpi_init(void);
void acpi_shutdown(void);
void acpi_reboot(void);
int  acpi_find_reset_register(void);

/* Power button */
int  acpi_power_button_read(void);  /* returns 1 if pressed, clears flag */

/* Sleep states */
#define ACPI_S0  0
#define ACPI_S1  1
#define ACPI_S2  2
#define ACPI_S3  3  /* Suspend-to-RAM */
#define ACPI_S4  4
#define ACPI_S5  5

/* Request a sleep state transition.
   Returns 0 if the sleep request was accepted, -1 if unsupported. */
int  acpi_sleep(uint32_t state);

/* Check if a given sleep state is supported. */
int  acpi_sleep_supported(uint32_t state);

#endif
