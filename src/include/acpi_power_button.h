#ifndef ACPI_POWER_BUTTON_H
#define ACPI_POWER_BUTTON_H

#include "types.h"

/* Power button event types */
#define ACPI_PBTN_PRESSED    1
#define ACPI_PBTN_RELEASED   0

/* Power button callback type */
typedef void (*acpi_pbtn_callback_t)(void);

/* API */
int  acpi_power_button_init(void);
int  acpi_power_button_read(void);
int  acpi_power_button_register_callback(acpi_pbtn_callback_t cb);
void acpi_power_button_unregister_callback(void);
int  acpi_power_button_is_initialized(void);

#endif /* ACPI_POWER_BUTTON_H */
