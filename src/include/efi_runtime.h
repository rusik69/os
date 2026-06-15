#ifndef EFI_RUNTIME_H
#define EFI_RUNTIME_H

/*
 * efi_runtime.h — UEFI Runtime Services API
 *
 * Provides wrappers for UEFI runtime services (GetTime, SetTime,
 * GetVariable, SetVariable, ResetSystem).  If the system was booted
 * via BIOS (no UEFI), all functions return -ENODEV gracefully.
 *
 * Usage:
 *   efi_runtime_init()      — called early in boot with system table phys addr
 *   efi_get_time(...)       — get current UEFI time
 *   efi_set_time(...)       — set UEFI time
 *   efi_reset_system(...)   — reboot via UEFI ResetSystem
 *   efi_get_variable(...)   — read an EFI variable
 *   efi_set_variable(...)   — write an EFI variable
 *   efi_is_available()      — check if UEFI runtime is present
 */

#include "types.h"

/* ── Initialisation ───────────────────────────────────────────────── */

/*
 * efi_runtime_init — Save the EFI system table pointer.
 *
 * Called very early in boot with the physical address of the UEFI
 * system table (obtained from the boot stub / multiboot info).
 * If @system_table_phys is 0 (BIOS boot), this is a no-op and all
 * subsequent calls return -ENODEV.
 *
 * The RuntimeServices pointer is extracted from offset 0x60 of the
 * system table.
 */
void efi_runtime_init(uint64_t system_table_phys);

/* ── Time services ────────────────────────────────────────────────── */

/*
 * efi_get_time — Get current UEFI wall-clock time.
 *
 * Returns 0 on success, -ENODEV if no UEFI runtime, or -1 on error.
 * All output pointers are optional (pass NULL to ignore).
 */
int efi_get_time(uint16_t *year, uint8_t *month, uint8_t *day,
                 uint8_t *hour, uint8_t *minute, uint8_t *second);

/*
 * efi_set_time — Set UEFI wall-clock time.
 *
 * Returns 0 on success, -ENODEV if no UEFI runtime, or -1 on error.
 */
int efi_set_time(uint16_t year, uint8_t month, uint8_t day,
                 uint8_t hour, uint8_t minute, uint8_t second);

/* ── Variable services ────────────────────────────────────────────── */

/*
 * efi_get_variable — Read an EFI variable by name.
 *
 * @name:       ASCII variable name (converted to UCS-2 internally).
 * @guid:       Pointer to 16-byte EFI GUID.
 * @attributes: (out) Variable attributes flags (may be NULL).
 * @data_size:  (in/out) Size of data buffer / bytes returned.
 * @data:       Output buffer for variable value.
 *
 * Returns 0 on success, -ENODEV if no UEFI runtime, -ENOENT if the
 * variable does not exist, or -1 on other error.
 */
int efi_get_variable(const char *name, uint8_t *guid,
                     uint32_t *attributes,
                     uint64_t *data_size, void *data);

/*
 * efi_set_variable — Create or update an EFI variable.
 *
 * @name:       ASCII variable name (converted to UCS-2 internally).
 * @guid:       Pointer to 16-byte EFI GUID.
 * @attributes: Variable attribute flags.
 * @data_size:  Size of data in bytes.
 * @data:       Variable value to store.
 *
 * Returns 0 on success, -ENODEV if no UEFI runtime, or -1 on error.
 */
int efi_set_variable(const char *name, uint8_t *guid,
                     uint32_t attributes,
                     uint64_t data_size, const void *data);

/* ── Reset system ─────────────────────────────────────────────────── */

/*
 * efi_reset_system — Reboot or shut down via UEFI ResetSystem.
 *
 * @shutdown: If non-zero, perform a clean shutdown (EFI_RESET_SHUTDOWN).
 *            If zero, perform a cold reset (EFI_RESET_COLD).
 *
 * If no UEFI runtime is available, falls back to a port 0x64 reset
 * (PS/2 controller reset pulse).  Does not return on success (halts
 * CPU if reset fails).
 */
void efi_reset_system(int shutdown);

/* ── Sysfs ─────────────────────────────────────────────────────────── */

/*
 * efi_sysfs_init — Create /sys/firmware/efi/ entries.
 *
 * Must be called after sysfs is mounted.  Creates the directory
 * /sys/firmware/efi/ with files systab, runtime, and vars to
 * expose UEFI runtime state to userspace.
 */
void efi_sysfs_init(void);

/* ── Availability ─────────────────────────────────────────────────── */

/*
 * efi_is_available — Check whether UEFI runtime services are present.
 *
 * Returns 1 if a valid UEFI runtime services table was found during
 * efi_runtime_init(), 0 otherwise (BIOS boot).
 */
int efi_is_available(void);

#endif /* EFI_RUNTIME_H */
