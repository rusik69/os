/*
 * uefi_runtime.c — UEFI Runtime Services stubs
 *
 * Provides thin wrappers for the four main UEFI runtime services that
 * a kernel might use: GetTime, SetTime, GetVariable, SetVariable, and
 * ResetSystem.
 *
 * In this implementation, the bootloader passes the runtime service
 * table pointer via a fixed location or we rely on the system table
 * saved by the boot stub.  Stubs that lack real UEFI runtime backing
 * return appropriate error codes.
 *
 * Item S157: UEFI runtime services
 */

#include "types.h"
#include "printf.h"
#include "string.h"

/* ── UEFI types ─────────────────────────────────────────────────────── */

typedef uint64_t efi_status;
typedef uint16_t efi_char16_t;
typedef uint64_t efi_physical_addr;
typedef void *efi_handle;

/* UEFI time structure */
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

/* UEFI time capabilities */
struct efi_time_cap {
    uint32_t resolution;  /* ns */
    uint32_t accuracy;    /* ns */
    uint8_t  sets_to_zero;/* TRUE if time below 0x10000 is zeroed */
} __attribute__((packed));

/* UEFI variable attributes */
#define EFI_VARIABLE_NON_VOLATILE       0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS     0x00000004
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD 0x00000008
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS 0x00000010
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020
#define EFI_VARIABLE_APPEND_WRITE       0x00000040

/* EFI status codes */
#define EFI_SUCCESS              0
#define EFI_UNSUPPORTED          ((efi_status)3)
#define EFI_INVALID_PARAMETER    ((efi_status)5)
#define EFI_NOT_FOUND            ((efi_status)14)
#define EFI_DEVICE_ERROR         ((efi_status)7)

/* ── Runtime services table structure (simplified) ────────────────── */
/* These are function pointer entries in the UEFI Runtime Services table. */
struct efi_runtime_services {
    uint64_t _pad_header[4];                          /* EFI_TABLE_HEADER */
    efi_status (*get_time)(struct efi_time *, struct efi_time_cap *);
    efi_status (*set_time)(struct efi_time *);
    efi_status (*get_wakeup_time)(uint8_t *, uint8_t *, struct efi_time *);
    efi_status (*set_wakeup_time)(uint8_t, struct efi_time *);
    efi_status (*set_virtual_address_map)(uint64_t, uint64_t, uint32_t, void *);
    efi_status (*convert_pointer)(uint64_t, void **);
    efi_status (*get_variable)(efi_char16_t *, uint8_t *, uint32_t *,
                               uint64_t *, void *);
    efi_status (*get_next_variable_name)(uint64_t *, efi_char16_t *, uint8_t *);
    efi_status (*set_variable)(efi_char16_t *, uint8_t *, uint32_t,
                               uint64_t, void *);
    efi_status (*get_next_high_mono_count)(uint32_t *);
    efi_status (*reset_system)(uint32_t, efi_status, uint64_t, void *);
    efi_status (*update_capsule)(void **, uint64_t, uint64_t);
    efi_status (*query_capsule_caps)(void **, uint64_t, uint64_t *, uint32_t *);
    efi_status (*query_variable_info)(uint32_t, uint64_t *, uint64_t *, uint64_t *);
} __attribute__((packed));

/* The runtime services pointer (set by boot stub, or NULL) */
static struct efi_runtime_services *g_efi_rt = NULL;

/*
 * uefi_runtime_init — Save the runtime services table pointer.
 *
 * Called very early in boot with the physical address of the UEFI
 * system table.  We extract the RuntimeServices pointer from it.
 *
 * UEFI System Table layout (simplified):
 *   offset 0x00: EFI_TABLE_HEADER (size 0x18)
 *   offset 0x18: firmware vendor (efi_char16_t *)
 *   offset 0x20: firmware revision (uint32_t)
 *   offset 0x28: ConsoleInHandle (efi_handle)
 *   offset 0x30: ConIn (void *)
 *   ...
 *   offset 0x60: RuntimeServices (void *)
 *   ...
 */
void uefi_runtime_init(uint64_t system_table_phys)
{
    if (!system_table_phys) {
        kprintf("[UEFI] No system table — runtime services unavailable\n");
        g_efi_rt = NULL;
        return;
    }

    /* The RuntimeServices pointer is at offset 0x60 in the system table */
    g_efi_rt = *(struct efi_runtime_services **)
                    ((uintptr_t)system_table_phys + 0x60);

    if (!g_efi_rt) {
        kprintf("[UEFI] No runtime services table\n");
    } else {
        kprintf("[UEFI] Runtime services registered at %p\n",
                (void*)g_efi_rt);
    }
}

/* ── GetTime ─────────────────────────────────────────────────────────── */

int uefi_get_time(uint16_t *year, uint8_t *month, uint8_t *day,
                  uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (!g_efi_rt || !g_efi_rt->get_time)
        return -1;

    struct efi_time et;
    memset(&et, 0, sizeof(et));

    efi_status st = g_efi_rt->get_time(&et, NULL);
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

int uefi_set_time(uint16_t year, uint8_t month, uint8_t day,
                  uint8_t hour, uint8_t minute, uint8_t second)
{
    if (!g_efi_rt || !g_efi_rt->set_time)
        return -1;

    struct efi_time et;
    memset(&et, 0, sizeof(et));
    et.year   = year;
    et.month  = month;
    et.day    = day;
    et.hour   = hour;
    et.minute = minute;
    et.second = second;
    et.timezone = 2048; /* local time */

    efi_status st = g_efi_rt->set_time(&et);
    return (st == EFI_SUCCESS) ? 0 : -1;
}

/* ── GetVariable ─────────────────────────────────────────────────────── */

int uefi_get_variable(const char *name, uint8_t *guid,
                      uint32_t *attributes,
                      uint64_t *data_size, void *data)
{
    if (!g_efi_rt || !g_efi_rt->get_variable || !name)
        return -1;

    /* Convert ASCII name to UCS-2 (EFI variable names are UCS-2) */
    efi_char16_t ucs2_name[256];
    int i = 0;
    while (*name && i < 255) {
        ucs2_name[i++] = (efi_char16_t)(*name++);
    }
    ucs2_name[i] = 0;

    efi_status st = g_efi_rt->get_variable(ucs2_name, guid,
                                           attributes, data_size, data);
    return (st == EFI_SUCCESS) ? 0 : -1;
}

/* ── SetVariable ─────────────────────────────────────────────────────── */

int uefi_set_variable(const char *name, uint8_t *guid,
                      uint32_t attributes,
                      uint64_t data_size, const void *data)
{
    if (!g_efi_rt || !g_efi_rt->set_variable || !name)
        return -1;

    efi_char16_t ucs2_name[256];
    int i = 0;
    while (*name && i < 255) {
        ucs2_name[i++] = (efi_char16_t)(*name++);
    }
    ucs2_name[i] = 0;

    efi_status st = g_efi_rt->set_variable(ucs2_name, guid,
                                           attributes, data_size, (void*)data);
    return (st == EFI_SUCCESS) ? 0 : -1;
}

/* ── ResetSystem ─────────────────────────────────────────────────────── */

#define EFI_RESET_COLD    0
#define EFI_RESET_WARM    1
#define EFI_RESET_SHUTDOWN 2
#define EFI_RESET_PLATFORM_SPECIFIC 3

void uefi_reset_system(int shutdown)
{
    if (!g_efi_rt || !g_efi_rt->reset_system) {
        /* Fallback: issue a triple-fault reset via outb */
        kprintf("[UEFI] No ResetSystem, falling back to port 0x64 reset\n");
        __asm__ volatile("mov $0x64, %%dx; inb %%dx, %%al" ::: "eax", "edx");
        for (;;) __asm__ volatile("cli; hlt");
    }

    uint32_t reset_type = shutdown ? EFI_RESET_SHUTDOWN : EFI_RESET_COLD;
    g_efi_rt->reset_system(reset_type, EFI_SUCCESS, 0, NULL);

    /* Should not return */
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── Stub when no UEFI runtime is available ─────────────────────────── */

int uefi_is_available(void)
{
    return (g_efi_rt != NULL);
}
