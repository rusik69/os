/*
 * efi_secureboot.c — UEFI Secure Boot enforcement
 *
 * At boot, reads the EFI SecureBoot and SetupMode variables to
 * determine the Secure Boot state.  When Secure Boot is enabled:
 *   1. Module signing enforcement is enabled (unsigned modules refused)
 *   2. Kernel lockdown is raised to INTEGRITY level
 *   3. /sys/firmware/efi/secureboot exposes the state (0, 1, or 2)
 *
 * References:
 *   - UEFI Specification 2.9, Section 3.3 (Secure Boot)
 *   - EFI_GLOBAL_VARIABLE: SetupMode=0x0001, SecureBoot=0x0002
 */

#define KERNEL_INTERNAL
#include "efi_secureboot.h"
#include "efi_runtime.h"
#include "lockdown.h"
#include "module_signature.h"
#include "printf.h"
#include "string.h"
#include "sysfs.h"

/* ── EFI Global Variable GUID (8BE4DF61-93CA-11d2-AA0D-00E098032B8C) ─ */
static uint8_t efi_global_guid[16] = EFI_GLOBAL_VARIABLE_GUID;

/* ── State ──────────────────────────────────────────────────────────── */
static int g_secureboot_mode = EFI_SECUREBOOT_DISABLED;
static int g_initialized = 0;

/* ── Read an EFI variable as a uint8_t ──────────────────────────────── */

static int efi_read_uint8_var(const char *name, uint8_t *value)
{
    uint64_t data_size = sizeof(uint8_t);
    uint8_t data;
    int ret = efi_get_variable(name, efi_global_guid, NULL, &data_size, &data);
    if (ret != 0)
        return ret;
    if (data_size < sizeof(uint8_t))
        return -ENOSYS;
    *value = data;
    return 0;
}

/* ── Determine Secure Boot mode ────────────────────────────────────── */
/*
 * Logic:
 *   1. Read SetupMode. If 1 → SETUP_MODE (keys can be enrolled).
 *   2. Read SecureBoot. If 1 → USER_MODE (Secure Boot is on).
 *   3. If both are 0 → DISABLED.
 *   4. If EFI variables are not accessible → fall back to DISABLED.
 */
static int efi_detect_secureboot_mode(void)
{
    uint8_t setup_mode = 0;
    uint8_t secureboot = 0;
    int has_setup, has_sb;

    has_setup = efi_read_uint8_var("SetupMode", &setup_mode);
    has_sb    = efi_read_uint8_var("SecureBoot", &secureboot);

    if (has_sb == 0 && secureboot == 1) {
        /* SecureBoot is 1: either USER_MODE or SETUP_MODE if SetupMode also 1 */
        if (has_setup == 0 && setup_mode == 1)
            return EFI_SECUREBOOT_SETUP_MODE;
        return EFI_SECUREBOOT_USER_MODE;
    }

    if (has_setup == 0 && setup_mode == 1) {
        /* SetupMode is 1 but SecureBoot is not 1 → SETUP_MODE */
        return EFI_SECUREBOOT_SETUP_MODE;
    }

    /* Neither active or variables not found */
    return EFI_SECUREBOOT_DISABLED;
}

/* ── Public API ────────────────────────────────────────────────────── */

void efi_secureboot_init(void)
{
    if (g_initialized)
        return;

    if (!efi_is_available()) {
        kprintf("[SECUREBOOT] No UEFI runtime — Secure Boot not applicable\n");
        g_secureboot_mode = EFI_SECUREBOOT_DISABLED;
        g_initialized = 1;
        return;
    }

    /* Detect Secure Boot mode */
    g_secureboot_mode = efi_detect_secureboot_mode();

    kprintf("[SECUREBOOT] Mode: ");
    switch (g_secureboot_mode) {
        case EFI_SECUREBOOT_DISABLED:
            kprintf("Disabled\n");
            break;
        case EFI_SECUREBOOT_SETUP_MODE:
            kprintf("Setup Mode (enrolling keys)\n");
            /* In setup mode, we don't enforce yet */
            break;
        case EFI_SECUREBOOT_USER_MODE:
            kprintf("User Mode (ENFORCED)\n");

            /* 1. Enable module signing enforcement */
            module_sig_set_enforce(1);
            kprintf("[SECUREBOOT] Module signing enforcement enabled\n");

            /* 2. Set lockdown to INTEGRITY level */
            lock_down(LOCKDOWN_INTEGRITY);
            kprintf("[SECUREBOOT] Lockdown raised to INTEGRITY\n");
            break;
    }

    g_initialized = 1;
}

int efi_secureboot_get_mode(void)
{
    return g_secureboot_mode;
}

int efi_secureboot_requires_module_signing(void)
{
    return (g_secureboot_mode == EFI_SECUREBOOT_USER_MODE) ? 1 : 0;
}

int efi_secureboot_is_enabled(void)
{
    return (g_secureboot_mode == EFI_SECUREBOOT_USER_MODE) ? 1 : 0;
}

/* ── Sysfs interface ───────────────────────────────────────────────── */

static int secureboot_read_cb(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;

    const char *state_str;
    switch (g_secureboot_mode) {
        case EFI_SECUREBOOT_SETUP_MODE:
            state_str = "1 (setup mode)";
            break;
        case EFI_SECUREBOOT_USER_MODE:
            state_str = "2 (user mode)";
            break;
        default:
            state_str = "0 (disabled)";
            break;
    }

    return snprintf(buf, (size_t)max_size, "%d\n%s\n",
                    g_secureboot_mode, state_str);
}

void efi_secureboot_sysfs_init(void)
{
    /* Create /sys/firmware/efi directory if not already present */
    sysfs_create_dir("/sys/firmware");
    sysfs_create_dir("/sys/firmware/efi");

    /* Create /sys/firmware/efi/secureboot file */
    sysfs_create_writable_file("/sys/firmware/efi/secureboot",
                               "0\n", NULL,
                               secureboot_read_cb, NULL);

    kprintf("[SECUREBOOT] /sys/firmware/efi/secureboot created\n");
}

/* ── Stub: efi_secureboot_enabled ──────────────────────────────────── */
int efi_secureboot_enabled(void)
{
    kprintf("[SECUREBOOT] efi_secureboot_enabled: not yet implemented\n");
    return 0;
}

/* ── Stub: efi_secureboot_verify ───────────────────────────────────── */
int efi_secureboot_verify(const uint8_t *data, size_t data_len,
                          const uint8_t *sig, size_t sig_len)
{
    (void)data; (void)data_len; (void)sig; (void)sig_len;
    kprintf("[SECUREBOOT] efi_secureboot_verify: not yet implemented\n");
    return 0;
}

/* ── Stub: efi_secureboot_db_lookup ────────────────────────────────── */
int efi_secureboot_db_lookup(const uint8_t *hash, size_t hash_len)
{
    (void)hash; (void)hash_len;
    kprintf("[SECUREBOOT] efi_secureboot_db_lookup: not yet implemented\n");
    return 0;
}

/* ── Stub: efi_secureboot_forbidden_signature ──────────────────────── */
int efi_secureboot_forbidden_signature(const uint8_t *hash, size_t hash_len)
{
    (void)hash; (void)hash_len;
    kprintf("[SECUREBOOT] efi_secureboot_forbidden_signature: not yet implemented\n");
    return 0;
}
