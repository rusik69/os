#ifndef ACPI_AC_ADAPTER_H
#define ACPI_AC_ADAPTER_H

#include "types.h"

/*
 * ACPI AC Adapter (Power Source) Status Driver
 *
 * Monitors AC adapter (power supply) presence via the ACPI _PSR
 * (Power Source) control method.  AC adapter devices (ACPI0003)
 * are found under _SB_ in the ACPI namespace.
 *
 * _PSR returns a DWORD:
 *   0 = Offline (system is on battery power)
 *   1 = Online  (system is connected to AC power)
 *
 * Reference: ACPI v6.3, Section 10.2.1 — AC Adapter Device.
 */

#define MAX_ACPI_AC_ADAPTERS      4
#define MAX_AC_ADAPTER_PATH       64

/* Per-AC-adapter device state */
struct acpi_ac_adapter_dev {
    char     path[MAX_AC_ADAPTER_PATH]; /* Namespace path, e.g. "\\_SB_.AC0" */
    int      present;                   /* 1 = device found in namespace */
    int      have_psr;                  /* 1 = _PSR method was found */
    uint32_t last_status;              /* Last read _PSR value (0=offline, 1=online) */
};

/* ── Public API ───────────────────────────────────────────────────── */

/* Initialise ACPI AC adapter monitoring.
 * Scans ACPI namespace for AC adapter devices and caches their paths.
 * Returns the number of AC adapters detected, or 0 if none found. */
int acpi_ac_adapter_init(void);

/* Return the number of detected ACPI AC adapter devices. */
int acpi_ac_adapter_get_count(void);

/* Check if the given AC adapter is online (connected to AC power).
 * Returns 1 if online, 0 if offline/on battery, -1 on error. */
int acpi_ac_adapter_is_online(int idx);

/* Get the last known status of the given AC adapter (0=offline, 1=online).
 * Returns 0 on success, -1 if the index is invalid. */
int acpi_ac_adapter_get_status(int idx, uint32_t *status);

/* Refresh the cached _PSR value for the given AC adapter.
 * Returns 0 on success, -1 if the adapter is not present or _PSR not available. */
int acpi_ac_adapter_refresh(int idx);

/* Print information about all detected AC adapters via kprintf. */
void acpi_ac_adapter_print_info(void);

#endif /* ACPI_AC_ADAPTER_H */
