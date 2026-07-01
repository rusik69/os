#ifndef ACPI_POWER_BUTTON_H
#define ACPI_POWER_BUTTON_H

#include "types.h"

/* Power button event types */
#define ACPI_PBTN_PRESSED    1
#define ACPI_PBTN_RELEASED   0

/* Power button callback type */
typedef void (*acpi_pbtn_callback_t)(void);

/* API — _ext variants to avoid symbol conflicts with acpi.c base implementations */
int  acpi_power_button_ext_init(void);
int  acpi_power_button_ext_read(void);
int  acpi_power_button_ext_register_callback(acpi_pbtn_callback_t cb);
void acpi_power_button_ext_unregister_callback(void);
int  acpi_power_button_ext_is_initialized(void);

/* ── Fixed Event API ─────────────────────────────────────────────── */

/* Initialize the ACPI power button fixed event handler.
 * Reads FADT to determine PM1a_EVT_BLK, enables the event, and
 * registers with the ACPI SCI subsystem.  Returns 0 on success. */
int acpi_power_button_fixed_event_init(void);

/* ACPI fixed event handler for the power button.
 * Called from the SCI dispatch context (or poll loop) to check
 * PM1 status bit 8 (PWRBTN_STS), clear the event, and fire
 * any registered callbacks.
 * Returns 1 if the event was handled, 0 if not pending. */
int acpi_power_button_fixed_event_handler(void *context);

#endif /* ACPI_POWER_BUTTON_H */
