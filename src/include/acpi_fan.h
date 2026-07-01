#ifndef ACPI_FAN_H
#define ACPI_FAN_H

#include "types.h"

/*
 * ACPI Fan Control (_FST, _FSL)
 *
 * _FST  — Fan Status, returns a Package:
 *          [0] Fan Speed        — DWORD (RPM, 0 = not spinning)
 *          [1] Fan Present      — DWORD (0 = not present, 1 = present)
 *          [2] Fan Set Level    — DWORD (0..100, current fan level)
 *
 * _FSL  — Fan Set Level, takes a DWORD argument (0..100)
 *         Sets the fan speed level.
 *
 * ACPI fan device objects (PNP0C0B) live under _SB_ in the namespace.
 */

#define MAX_ACPI_FANS         4
#define MAX_FAN_PATH          64

/* Fan states */
#define ACPI_FAN_OFF          0
#define ACPI_FAN_PRESENT      1

/* Per-fan device state */
struct acpi_fan_dev {
    char     path[MAX_FAN_PATH]; /* Namespace path, e.g. "\\_SB_.FAN0" */
    int      present;            /* 1 = device found in namespace */
    int      have_fst;           /* 1 = _FST method was found */
    int      have_fsl;           /* 1 = _FSL method was found */
    uint32_t speed;              /* Last read fan speed (RPM) */
    uint32_t level;              /* Last read / last set fan level (0..100) */
};

/* ── Public API ───────────────────────────────────────────────────── */

/* Initialise ACPI fan control.
 * Scans ACPI namespace for fan devices and caches their methods.
 * Returns the number of fans detected, or 0 if none found. */
int acpi_fan_init(void);

/* Return the number of detected ACPI fan devices. */
int acpi_fan_get_count(void);

/* Read the current fan speed in RPM for the given fan index.
 * Returns 0 on success, -1 if the fan index is invalid or _FST unavailable. */
int acpi_fan_get_speed(int fan, uint32_t *speed);

/* Read the current fan level (0..100) for the given fan index.
 * Returns 0 on success, -1 if the fan index is invalid or _FST unavailable. */
int acpi_fan_get_level(int fan, uint32_t *level);

/* Set the fan level (0..100) for the given fan index via _FSL.
 * Returns 0 on success, -1 if _FSL is not available or the value is invalid. */
int acpi_fan_set_level(int fan, uint32_t level);

/* Print information about all detected fans via kprintf. */
void acpi_fan_print_info(void);

#endif /* ACPI_FAN_H */
