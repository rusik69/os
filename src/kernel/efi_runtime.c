#define KERNEL_INTERNAL
#include "efi_runtime.h"
#include "errno.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "sysfs.h"

/*
 * efi_runtime.c — UEFI Runtime Services for the Hermes OS kernel
 *
 * If the system was booted via UEFI, the boot stub passes the physical
 * address of the UEFI system table to efi_runtime_init().  We extract
 * the RuntimeServices pointer from it and provide thin wrappers for the
 * four main runtime services.
 *
 * If booted via BIOS (no UEFI), all calls return -ENODEV gracefully.
 *
 * On UEFI systems, /sys/firmware/efi/ entries are created to expose
 * the runtime state to userspace.
 */

/* ── UEFI types ─────────────────────────────────────────────────────── */

typedef uint64_t efi_status_t;
typedef uint16_t efi_char16_t;

/* UEFI time structure (packed, same layout as UEFI spec) */
struct efi_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  pad1;
    uint32_t nanosecond;
    int16_t  timezone;   /* minutes from UTC, -1440..1440 or 2048=local */
    uint8_t  daylight;   /* 0=standard, 1=daylight, 3=auto */
    uint8_t  pad2;
} __attribute__((packed));

struct efi_time_cap {
    uint32_t resolution;
    uint32_t accuracy;
    uint8_t  sets_to_zero;
} __attribute__((packed));

/* EFI status codes */
#define EFI_SUCCESS              0
#define EFI_UNSUPPORTED          ((efi_status_t)3)
#define EFI_INVALID_PARAMETER    ((efi_status_t)5)
#define EFI_NOT_FOUND            ((efi_status_t)14)
#define EFI_DEVICE_ERROR         ((efi_status_t)7)

/* EFI variable attribute flags */
#define EFI_VARIABLE_NON_VOLATILE       0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS     0x00000004
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD 0x00000008
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS 0x00000010
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020
#define EFI_VARIABLE_APPEND_WRITE       0x00000040

/* Reset types */
#define EFI_RESET_COLD     0
#define EFI_RESET_WARM     1
#define EFI_RESET_SHUTDOWN 2

/* ── UEFI Runtime Services Table ───────────────────────────────────── */

struct efi_runtime_services {
    uint64_t _pad_header[4];                          /* EFI_TABLE_HEADER */
    efi_status_t (*get_time)(struct efi_time *, struct efi_time_cap *);
    efi_status_t (*set_time)(struct efi_time *);
    efi_status_t (*get_wakeup_time)(uint8_t *, uint8_t *, struct efi_time *);
    efi_status_t (*set_wakeup_time)(uint8_t, struct efi_time *);
    efi_status_t (*set_virtual_address_map)(uint64_t, uint64_t, uint32_t, void *);
    efi_status_t (*convert_pointer)(uint64_t, void **);
    efi_status_t (*get_variable)(efi_char16_t *, uint8_t *, uint32_t *,
                                 uint64_t *, void *);
    efi_status_t (*get_next_variable_name)(uint64_t *, efi_char16_t *, uint8_t *);
    efi_status_t (*set_variable)(efi_char16_t *, uint8_t *, uint32_t,
                                 uint64_t, void *);
    efi_status_t (*get_next_high_mono_count)(uint32_t *);
    efi_status_t (*reset_system)(uint32_t, efi_status_t, uint64_t, void *);
    efi_status_t (*update_capsule)(void **, uint64_t, uint64_t);
    efi_status_t (*query_capsule_caps)(void **, uint64_t, uint64_t *, uint32_t *);
    efi_status_t (*query_variable_info)(uint32_t, uint64_t *, uint64_t *, uint64_t *);
} __attribute__((packed));

/* The runtime services pointer (NULL = no UEFI / BIOS boot) */
static struct efi_runtime_services *g_efi_rt = NULL;

/* Saved for /sys/firmware/efi/ entries */
static uint64_t g_systab_phys = 0;

/* ── Initialisation ──────────────────────────────────────────────────── */

void efi_runtime_init(uint64_t system_table_phys)
{
    if (!system_table_phys) {
        kprintf("[EFI] No system table — runtime services unavailable\n");
        g_efi_rt = NULL;
        g_systab_phys = 0;
        return;
    }

    g_systab_phys = system_table_phys;

    /* The RuntimeServices pointer is at offset 0x60 in the system table */
    g_efi_rt = *(struct efi_runtime_services **)
                    ((uintptr_t)system_table_phys + 0x60);

    if (!g_efi_rt) {
        kprintf("[EFI] No runtime services table\n");
    } else {
        kprintf("[EFI] Runtime services registered at %p\n",
                (void *)g_efi_rt);
    }
}

/* ── GetTime ─────────────────────────────────────────────────────────── */

int efi_get_time(uint16_t *year, uint8_t *month, uint8_t *day,
                 uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (!g_efi_rt || !g_efi_rt->get_time)
        return -ENODEV;

    struct efi_time et;
    memset(&et, 0, sizeof(et));

    efi_status_t st = g_efi_rt->get_time(&et, NULL);
    if (st != EFI_SUCCESS)
        return -1;

    if (year)   *year   = et.year;
    if (month)  *month  = et.month;
    if (day)    *day    = et.day;
    if (hour)   *hour   = et.hour;
    if (minute) *minute = et.minute;
    if (second) *second = et.second;
    return 0;
}

/* ── SetTime ─────────────────────────────────────────────────────────── */

int efi_set_time(uint16_t year, uint8_t month, uint8_t day,
                 uint8_t hour, uint8_t minute, uint8_t second)
{
    if (!g_efi_rt || !g_efi_rt->set_time)
        return -ENODEV;

    struct efi_time et;
    memset(&et, 0, sizeof(et));
    et.year   = year;
    et.month  = month;
    et.day    = day;
    et.hour   = hour;
    et.minute = minute;
    et.second = second;
    et.timezone = 2048; /* local time */

    efi_status_t st = g_efi_rt->set_time(&et);
    return (st == EFI_SUCCESS) ? 0 : -1;
}

/* ── GetVariable ─────────────────────────────────────────────────────── */

int efi_get_variable(const char *name, uint8_t *guid,
                     uint32_t *attributes,
                     uint64_t *data_size, void *data)
{
    if (!g_efi_rt || !g_efi_rt->get_variable || !name)
        return -ENODEV;

    /* Convert ASCII name to UCS-2 (EFI variable names are UCS-2) */
    efi_char16_t ucs2_name[256];
    int i = 0;
    while (*name && i < 255) {
        ucs2_name[i++] = (efi_char16_t)(*name++);
    }
    ucs2_name[i] = 0;

    efi_status_t st = g_efi_rt->get_variable(ucs2_name, guid,
                                             attributes, data_size, data);
    if (st == EFI_SUCCESS)
        return 0;
    if (st == EFI_NOT_FOUND)
        return -ENOENT;
    return -1;
}

/* ── SetVariable ─────────────────────────────────────────────────────── */

int efi_set_variable(const char *name, uint8_t *guid,
                     uint32_t attributes,
                     uint64_t data_size, const void *data)
{
    if (!g_efi_rt || !g_efi_rt->set_variable || !name)
        return -ENODEV;

    efi_char16_t ucs2_name[256];
    int i = 0;
    while (*name && i < 255) {
        ucs2_name[i++] = (efi_char16_t)(*name++);
    }
    ucs2_name[i] = 0;

    efi_status_t st = g_efi_rt->set_variable(ucs2_name, guid,
                                             attributes, data_size, (void *)data);
    return (st == EFI_SUCCESS) ? 0 : -1;
}

/* ── ResetSystem ─────────────────────────────────────────────────────── */

void efi_reset_system(int shutdown)
{
    if (!g_efi_rt || !g_efi_rt->reset_system) {
        /* Fallback: issue a reset via the PS/2 controller port */
        kprintf("[EFI] No ResetSystem, falling back to port 0x64 reset\n");
        __asm__ volatile("mov $0x64, %%dx; inb %%dx, %%al" ::: "eax", "edx");
        for (;;) __asm__ volatile("cli; hlt");
    }

    uint32_t reset_type = shutdown ? EFI_RESET_SHUTDOWN : EFI_RESET_COLD;
    g_efi_rt->reset_system(reset_type, EFI_SUCCESS, 0, NULL);

    /* Should not return — halt if it does */
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── Availability ────────────────────────────────────────────────────── */

int efi_is_available(void)
{
    return (g_efi_rt != NULL) ? 1 : 0;
}

/* ── Sysfs entries: /sys/firmware/efi/ ──────────────────────────────── */

/* Read callback for /sys/firmware/efi/systab — exposes the system table
 * physical address so userspace can verify UEFI is present. */
static int efi_systab_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    if (!g_efi_rt)
        return snprintf(buf, max_size, "0\n");

    return snprintf(buf, max_size, "0x%llx\n",
                    (unsigned long long)g_systab_phys);
}

/* Read callback for /sys/firmware/efi/runtime — exposes the runtime
 * services table virtual address. */
static int efi_runtime_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    if (!g_efi_rt)
        return snprintf(buf, max_size, "0\n");

    return snprintf(buf, max_size, "%p\n", (void *)g_efi_rt);
}

/* Read callback for /sys/firmware/efi/vars — lists known EFI variables
 * (stub: just indicates support presence). */
static int efi_vars_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    if (!g_efi_rt)
        return snprintf(buf, max_size, "not supported\n");

    return snprintf(buf, max_size, "EFI variable services active\n");
}

/*
 * efi_sysfs_init — Create /sys/firmware/efi/ entries.
 *
 * Called after sysfs is mounted.  Creates:
 *   /sys/firmware/efi/          (directory)
 *   /sys/firmware/efi/systab    (system table phys addr)
 *   /sys/firmware/efi/runtime   (runtime services ptr)
 *   /sys/firmware/efi/vars      (variable service status)
 */
void efi_sysfs_init(void)
{
    /* Create /sys/firmware/ + /sys/firmware/efi/ directories */
    if (sysfs_create_dir("firmware") < 0)
        return;
    if (sysfs_create_dir("firmware/efi") < 0)
        return;

    /* Create sysfs files with dynamic read callbacks */
    sysfs_create_writable_file("firmware/efi/systab", "0\n",
                                NULL, efi_systab_read, NULL);
    sysfs_create_writable_file("firmware/efi/runtime", "0\n",
                                NULL, efi_runtime_read, NULL);
    sysfs_create_writable_file("firmware/efi/vars", "not supported\n",
                                NULL, efi_vars_read, NULL);

    kprintf("[EFI] /sys/firmware/efi/ entries created\n");
}

/* ── Stub: efi_get_wakeup_time ─────────────────────────────────────── */
int efi_get_wakeup_time(uint8_t *enabled, uint8_t *pending,
                        uint16_t *year, uint8_t *month, uint8_t *day,
                        uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    (void)enabled; (void)pending; (void)year; (void)month; (void)day;
    (void)hour; (void)minute; (void)second;
    kprintf("[EFI] efi_get_wakeup_time: not yet implemented\n");
    return 0;
}

/* ── Stub: efi_set_wakeup_time ─────────────────────────────────────── */
int efi_set_wakeup_time(uint8_t enable, uint16_t year, uint8_t month, uint8_t day,
                        uint8_t hour, uint8_t minute, uint8_t second)
{
    (void)enable; (void)year; (void)month; (void)day;
    (void)hour; (void)minute; (void)second;
    kprintf("[EFI] efi_set_wakeup_time: not yet implemented\n");
    return 0;
}
