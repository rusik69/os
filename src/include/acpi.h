#ifndef ACPI_H
#define ACPI_H

#include "types.h"

void acpi_init(void);
void acpi_shutdown(void);
void acpi_reboot(void);
int  acpi_find_reset_register(void);

#endif
