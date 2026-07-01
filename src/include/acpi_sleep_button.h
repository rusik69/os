#ifndef ACPI_SLEEP_BUTTON_H
#define ACPI_SLEEP_BUTTON_H

#include "types.h"

/*
 * ACPI Sleep Button Fixed Event Driver
 *
 * Implements the ACPI fixed hardware sleep button (Fixed Event 0x01)
 * that allows the system to transition to a sleep state (typically S1
 * or S3) when the sleep button is pressed.
 *
 * The sleep button status is indicated by bit 9 (SLPBTN_STS) in the
 * PM1 status register, enabled by bit 9 (SLPBTN_EN) in the PM1 enable
 * register.
 *
 * Reference: ACPI v6.3, Sections 4.8.2.1.2 (Sleep Button), 5.2.9 (FADT).
 */

/* Sleep button event types */
#define ACPI_SLPBTN_PRESSED    1
#define ACPI_SLPBTN_RELEASED   0

/* Default sleep state to enter when sleep button is pressed */
#define ACPI_SLPBTN_DEFAULT_SLEEP_STATE  ACPI_S1

/* Sleep button callback type */
typedef void (*acpi_slpbtn_callback_t)(void);

/* ── Public API ───────────────────────────────────────────────────── */

/* Initialise the ACPI sleep button driver.
 * Reads the FADT to determine PM1a_EVT_BLK, enables the sleep button
 * event (bit 9 in PM1_EN), and prepares the handler.
 * Returns 0 on success, -1 on error. */
int acpi_sleep_button_init(void);

/* Check whether the sleep button has been pressed.
 * Returns ACPI_SLPBTN_PRESSED (1) if pressed and event was consumed,
 * or ACPI_SLPBTN_RELEASED (0) if no event pending. */
int acpi_sleep_button_read(void);

/* Register a callback to be invoked when the sleep button fires. */
int acpi_sleep_button_register_callback(acpi_slpbtn_callback_t cb);

/* Unregister the sleep button callback. */
void acpi_sleep_button_unregister_callback(void);

/* Check if the sleep button driver has been initialised. */
int acpi_sleep_button_is_initialized(void);

/* Set the target sleep state (e.g. ACPI_S1, ACPI_S3) for button press. */
void acpi_sleep_button_set_target_state(uint32_t state);

/* Get the current target sleep state. */
uint32_t acpi_sleep_button_get_target_state(void);

/* ── Fixed Event API ─────────────────────────────────────────────── */

/* ACPI fixed event handler for the sleep button.
 * Called from the SCI dispatch context (or poll loop) to check
 * PM1 status bit 9 (SLPBTN_STS), clear the event, and fire
 * any registered callbacks.
 * Returns 1 if the event was handled, 0 if not pending. */
int acpi_sleep_button_fixed_event_handler(void *context);

/* Initialize the ACPI sleep button fixed event handler.
 * Reads FADT to determine PM1a_EVT_BLK, enables the event, and
 * registers with the ACPI SCI subsystem.  Returns 0 on success. */
int acpi_sleep_button_fixed_event_init(void);

/* Trigger the sleep transition based on the configured target state.
 * Called after clearing the sleep button status bit. */
int acpi_sleep_button_trigger_sleep(void);

#endif /* ACPI_SLEEP_BUTTON_H */
