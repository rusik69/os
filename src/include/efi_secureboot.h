#ifndef EFI_SECUREBOOT_H
#define EFI_SECUREBOOT_H

/*
 * efi_secureboot.h — UEFI Secure Boot enforcement
 *
 * Checks UEFI SecureBoot variable at boot and enforces:
 *   - Module signing required when SecureBoot is enabled
 *   - Lockdown INTEGRITY level set
 *   - Exposes /sys/firmware/efi/secureboot status
 *
 * EFI variables:
 *   SetupMode  = 0x0001 (1 = setup mode, 0 = user mode)
 *   SecureBoot = 0x0002 (1 = enabled, 0 = disabled)
 *   MokList    (shim/MOK variables)
 */

#include "types.h"

/* ── EFI variable GUIDs (converted to 16-byte arrays) ──────────────── */

/* EFI_GLOBAL_VARIABLE (8BE4DF61-93CA-11d2-AA0D-00E098032B8C) */
#define EFI_GLOBAL_VARIABLE_GUID \
    {0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xd2, 0x11, \
     0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}

/* SHIM_LOCK_GUID (605dab50-e046-4300-abb6-3dd810dd8b23) */
#define SHIM_LOCK_GUID \
    {0x50, 0xAB, 0x5D, 0x60, 0x46, 0xE0, 0x00, 0x43, \
     0xAB, 0xB6, 0x3D, 0xD8, 0x10, 0xDD, 0x8B, 0x23}

/* ── Secure Boot states for /sys/firmware/efi/secureboot ──────────── */
#define EFI_SECUREBOOT_DISABLED     0   /* Secure Boot disabled */
#define EFI_SECUREBOOT_SETUP_MODE   1   /* Setup mode (keys can be enrolled) */
#define EFI_SECUREBOOT_USER_MODE    2   /* User mode (Secure Boot enabled) */

/* ── Public API ────────────────────────────────────────────────────── */

/* Initialize Secure Boot enforcement — called at boot.
 * Reads EFI variables and sets lockdown + module signing accordingly. */
void efi_secureboot_init(void);

/* Get current Secure Boot state (0=disabled, 1=setup, 2=user) */
int efi_secureboot_get_mode(void);

/* Check if module signing enforcement is required by Secure Boot */
int efi_secureboot_requires_module_signing(void);

/* Check if the system is in Secure Boot user mode */
int efi_secureboot_is_enabled(void);

/* Create /sys/firmware/efi/secureboot entry */
void efi_secureboot_sysfs_init(void);

#endif /* EFI_SECUREBOOT_H */
