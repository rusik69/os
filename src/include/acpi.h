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

/* ── Dock/Undock Notification (Item 106) ─────────────────────────── */

/* Dock station state flags */
#define ACPI_DOCK_NOT_PRESENT  0  /* no dock hardware found */
#define ACPI_DOCK_UNDOCKED     1  /* dock present but undocked */
#define ACPI_DOCK_DOCKED       2  /* device docked */

/* Maximum number of dock notification callbacks */
#define ACPI_DOCK_MAX_CB       8

/* Callback type for dock events: called with dock_state (ACPI_DOCK_DOCKED
 * or ACPI_DOCK_UNDOCKED) and an opaque user pointer. */
typedef void (*acpi_dock_callback_t)(int dock_state, void *user_data);

/* Register a callback for dock state change notifications.
 * Returns 0 on success, -1 if the callback table is full. */
int acpi_dock_register_notify(acpi_dock_callback_t cb, void *user_data);

/* Unregister a previously registered dock notification callback. */
void acpi_dock_unregister_notify(acpi_dock_callback_t cb, void *user_data);

/* Query the current dock station state.
 * Returns ACPI_DOCK_NOT_PRESENT, ACPI_DOCK_UNDOCKED, or ACPI_DOCK_DOCKED. */
int acpi_dock_get_state(void);

/* Poll the dock hardware for state changes.
 * Called periodically (e.g., from the idle loop or a timer) to detect
 * hotplug events when no ACPI notification mechanism is available. */
void acpi_dock_poll(void);

/* ── DSDT global information (exported for ACPI drivers) ──────────── */

/* Virtual address of DSDT base (mapped via PHYS_TO_VIRT).
 * Set during acpi_init() after the FADT is parsed. */
extern uint8_t *g_dsdt_base;
/* Total length of the DSDT table (including ACPI header) in bytes. */
extern uint32_t g_dsdt_length;

/* ── ACPI table header (common to all ACPI tables) ────────────────── */

struct acpi_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

#endif
