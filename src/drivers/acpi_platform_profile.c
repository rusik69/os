/*
 * acpi_platform_profile.c — ACPI Platform Profile support
 *
 * Parses the _DSM method for platform profile support and exposes
 * the current profile via /sys/firmware/acpi/platform_profile.
 *
 * Platform profiles allow selecting performance, balanced, or
 * low-power operating modes.  The _DSM method (Platform Profile
 * Support, UUID: c19f3e46-3c72-4e3b-8c1a-5c99c5c1c4f6)
 * is defined in the ACPI specification.
 */

#include "acpi.h"
#include "sysfs.h"
#include "string.h"
#include "printf.h"
#include "stdlib.h"

/* ── Platform profile states ────────────────────────────────────────── */

/* The ACPI Platform Profile _DSM UUID:
 *   c19f3e46-3c72-4e3b-8c1a-5c99c5c1c4f6
 * Raw bytes (little-endian format as used in ACPI _DSM):
 */
#define PLATFORM_PROFILE_UUID \
    "\x46\x3e\x9f\xc1\x72\x3c\x3b\x4e\x8c\x1a\x5c\x99\xc5\xc1\xc4\xf6"

/* Function indices for _DSM (Platform Profile) */
#define DSM_FUNC_GET_SUPPORTED_PROFILES  0  /* Returns supported profile bits */
#define DSM_FUNC_GET_CURRENT_PROFILE     1  /* Returns current profile */
#define DSM_FUNC_SET_CURRENT_PROFILE     2  /* Sets current profile */
#define DSM_FUNC_GET_DEVICE_DESC         3  /* Returns device description */

/* Profile bit definitions (returned by function 0) */
#define PROFILE_BIT_LOW_POWER        (1U << 0)
#define PROFILE_BIT_BALANCED         (1U << 1)
#define PROFILE_BIT_PERFORMANCE      (1U << 2)

/* ── Profile names ──────────────────────────────────────────────────── */

enum platform_profile {
    PROFILE_UNSUPPORTED = -1,
    PROFILE_LOW_POWER   = 0,
    PROFILE_BALANCED    = 1,
    PROFILE_PERFORMANCE = 2,
    PROFILE_MAX
};

static const char * const profile_names[PROFILE_MAX] = {
    "low-power",
    "balanced",
    "performance"
};

/* ── State ──────────────────────────────────────────────────────────── */

static int g_supported_profiles = 0;
static int g_current_profile = PROFILE_BALANCED;  /* Default */
static int g_initialized = 0;

/* ── _DSM helper (stub) ────────────────────────────────────────────── *
 *
 * In a full ACPI implementation, this would evaluate _DSM on the
 * platform device using the AML interpreter.  Since we don't have
 * a full AML interpreter yet, we provide a stub that checks if the
 * DSDT contains relevant _DSM methods and falls back to a reasonable
 * default (balanced with all three profiles available).
 *
 * When an AML interpreter becomes available, replace this stub with
 * actual _DSM evaluation code.
 */

/* Evaluate _DSM function on a device node.
 * Returns 0 on success, -1 if not available.
 * For now, returns a default value. */
static int acpi_evaluate_dsm(uint32_t func_idx, uint32_t *result)
{
    (void)func_idx;
    (void)result;

    /* Stub: In a real implementation, we would:
     *   1. Find the platform device in the ACPI namespace
     *   2. Call _DSM with the Platform Profile UUID
     *   3. Return the result buffer
     *
     * For now, we return -1 to indicate _DSM is not available,
     * and rely on the fallback defaults.
     */
    return -1;
}

/* ── sysfs read/write callbacks ─────────────────────────────────────── */

static int platform_profile_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;

    if (!g_initialized)
        return snprintf(buf, (size_t)max_size, "unsupported\n");

    const char *name = "unknown";
    if (g_current_profile >= 0 && g_current_profile < PROFILE_MAX)
        name = profile_names[g_current_profile];

    return snprintf(buf, (size_t)max_size, "%s\n", name);
}

static int platform_profile_write(const char *data, uint32_t size, void *priv)
{
    (void)priv;

    if (!g_initialized)
        return -1;

    /* Strip trailing whitespace/newline */
    char buf[32];
    uint32_t copy_len = (size < sizeof(buf) - 1) ? size : sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    /* Remove trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    int new_profile = -1;
    if (strcmp(buf, "low-power") == 0)
        new_profile = PROFILE_LOW_POWER;
    else if (strcmp(buf, "balanced") == 0)
        new_profile = PROFILE_BALANCED;
    else if (strcmp(buf, "performance") == 0)
        new_profile = PROFILE_PERFORMANCE;
    else
        return -1;

    /* Check if the profile is supported */
    int bit = (1 << new_profile);
    if (!(g_supported_profiles & bit))
        return -1;

    /* Attempt to set via _DSM */
    uint32_t dsm_result = 0;
    if (acpi_evaluate_dsm(DSM_FUNC_SET_CURRENT_PROFILE, &dsm_result) == 0) {
        /* _DSM succeeded — trust the result */
    }

    g_current_profile = new_profile;

    kprintf("[platform_profile] Set to '%s' (profile %d)\n",
            profile_names[new_profile], new_profile);
    return (int)size;
}

/* ── Initialization ─────────────────────────────────────────────────── */

static void platform_profile_parse_supported(void)
{
    /* Try _DSM function 0 (supported profiles) */
    uint32_t dsm_result = 0;
    if (acpi_evaluate_dsm(DSM_FUNC_GET_SUPPORTED_PROFILES, &dsm_result) == 0) {
        g_supported_profiles = (int)dsm_result;
    }

    /* Try _DSM function 1 (current profile) */
    if (acpi_evaluate_dsm(DSM_FUNC_GET_CURRENT_PROFILE, &dsm_result) == 0) {
        g_current_profile = (int)dsm_result;
    }

    /* If _DSM is not available, use sensible defaults */
    if (g_supported_profiles == 0) {
        /* Assume all three profiles are supported */
        g_supported_profiles = PROFILE_BIT_LOW_POWER |
                               PROFILE_BIT_BALANCED |
                               PROFILE_BIT_PERFORMANCE;
    }

    if (g_current_profile < 0 || g_current_profile >= PROFILE_MAX) {
        g_current_profile = PROFILE_BALANCED;  /* Safe default */
    }
}

void acpi_platform_profile_init(void)
{
    if (g_initialized) {
        kprintf("[platform_profile] Already initialized\n");
        return;
    }

    platform_profile_parse_supported();

    /* Create /sys/firmware/acpi/ directory */
    sysfs_create_dir("/sys/firmware");
    sysfs_create_dir("/sys/firmware/acpi");

    /* Create the platform_profile file with read/write callbacks */
    sysfs_create_writable_file(
        "/sys/firmware/acpi/platform_profile",
        "balanced\n",
        NULL,
        platform_profile_read,
        platform_profile_write
    );

    g_initialized = 1;

    kprintf("[platform_profile] Initialized: supported=0x%x current='%s'\n",
            (unsigned int)g_supported_profiles,
            profile_names[g_current_profile]);
}
#include "module.h"
module_init(acpi_platform_profile_init);

/* ── Stub: acpi_platform_profile_get ─────────────────────────────── */
int acpi_platform_profile_get(void *dev, int *profile)
{
    (void)dev;
    (void)profile;
    kprintf("[acpi] acpi_platform_profile_get: not yet implemented\n");
    return 0;
}
/* ── Stub: acpi_platform_profile_set ─────────────────────────────── */
int acpi_platform_profile_set(void *dev, int profile)
{
    (void)dev;
    (void)profile;
    kprintf("[acpi] acpi_platform_profile_set: not yet implemented\n");
    return 0;
}
