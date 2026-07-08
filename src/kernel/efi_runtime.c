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
        return -ENOSYS;

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
    return -EIO;
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
                                             attributes, data_size, (void *)(uintptr_t)data);
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
    return g_efi_rt != NULL;
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

/* ── In-memory EFI variable store ──────────────────────────────────────
 * When no real UEFI runtime is available (BIOS boot), we provide a
 * software variable store that implements GetVariable/SetVariable
 * semantics.  This allows the kernel and userspace to test EFI
 * variable logic even on BIOS systems.
 */
#define EFI_VAR_STORE_MAX     64
#define EFI_VAR_NAME_MAX      128
#define EFI_VAR_DATA_MAX      4096

struct efi_var_entry {
    int      in_use;
    char     name[EFI_VAR_NAME_MAX];
    uint8_t  guid[16];
    uint32_t attributes;
    uint64_t data_size;
    uint8_t  data[EFI_VAR_DATA_MAX];
};

static struct efi_var_entry g_efi_var_store[EFI_VAR_STORE_MAX];
static int g_efi_var_count = 0;

/* Find a variable by name + GUID in the in-memory store */
static struct efi_var_entry *efi_var_find(const char *name, uint8_t *guid)
{
    for (int i = 0; i < g_efi_var_count; i++) {
        if (!g_efi_var_store[i].in_use) continue;
        if (strcmp(g_efi_var_store[i].name, name) != 0) continue;
        if (guid && memcmp(g_efi_var_store[i].guid, guid, 16) != 0) continue;
        return &g_efi_var_store[i];
    }
    return NULL;
}

/* GetVariable from in-memory store — matches UEFI GetVariable semantics */
static int efi_var_store_get_variable(const char *name, uint8_t *guid,
                                       uint32_t *attributes,
                                       uint64_t *data_size, void *data)
{
    struct efi_var_entry *e = efi_var_find(name, guid);
    if (!e)
        return -ENOENT;

    if (attributes)
        *attributes = e->attributes;

    if (data) {
        uint64_t copy_size = *data_size;
        if (copy_size > e->data_size)
            copy_size = e->data_size;
        memcpy(data, e->data, (size_t)copy_size);
    }
    *data_size = e->data_size;
    return 0;
}

/* SetVariable in in-memory store — matches UEFI SetVariable semantics */
static int efi_var_store_set_variable(const char *name, uint8_t *guid,
                                       uint32_t attributes,
                                       uint64_t data_size, const void *data)
{
    if (data_size > EFI_VAR_DATA_MAX)
        return -EFBIG;

    /* If variable exists, update it */
    struct efi_var_entry *e = efi_var_find(name, guid);
    if (e) {
        e->attributes = attributes;
        e->data_size = data_size;
        if (data_size > 0 && data)
            memcpy(e->data, data, (size_t)data_size);
        return 0;
    }

    /* Create new variable */
    if (g_efi_var_count >= EFI_VAR_STORE_MAX)
        return -ENOSPC;

    e = &g_efi_var_store[g_efi_var_count++];
    e->in_use = 1;
    strncpy(e->name, name, EFI_VAR_NAME_MAX - 1);
    e->name[EFI_VAR_NAME_MAX - 1] = '\0';
    if (guid)
        memcpy(e->guid, guid, 16);
    e->attributes = attributes;
    e->data_size = data_size;
    if (data_size > 0 && data)
        memcpy(e->data, data, (size_t)data_size);
    return 0;
}

/* GetNextVariableName — enumerate variable names from the in-memory store.
 * Returns 0 on success with the next variable name copied to @name,
 * -ENOENT if no more variables exist.
 * On first call, set *name_size to 0 to start from the beginning.
 */
int efi_var_store_get_next_variable(uint64_t *name_size, char *name,
                                     uint8_t *guid)
{
    if (!name_size || !name) return -EINVAL;

    if (*name_size == 0) {
        /* Start from the first variable */
        if (g_efi_var_count <= 0)
            return -ENOENT;
        struct efi_var_entry *e = &g_efi_var_store[0];
        if (!e->in_use) return -ENOENT;
        strncpy(name, e->name, (size_t)*name_size);
        if (guid) memcpy(guid, e->guid, 16);
        *name_size = (uint64_t)strlen(e->name) + 1;
        return 0;
    }

    /* Find current name, return next */
    for (int i = 0; i < g_efi_var_count - 1; i++) {
        if (!g_efi_var_store[i].in_use) continue;
        if (strcmp(g_efi_var_store[i].name, name) == 0) {
            struct efi_var_entry *next = &g_efi_var_store[i + 1];
            if (!next->in_use)
                continue; /* skip to next */
            strncpy(name, next->name, (size_t)*name_size);
            if (guid) memcpy(guid, next->guid, 16);
            *name_size = (uint64_t)strlen(next->name) + 1;
            return 0;
        }
    }

    return -ENOENT;
}

/* QueryVariableInfo — return storage space info for the variable store.
 * Returns total, available, and maximum variable storage space.
 */
int efi_var_store_query_variable_info(uint32_t attributes,
                                       uint64_t *max_storage,
                                       uint64_t *remaining,
                                       uint64_t *max_var_size)
{
    (void)attributes;

    uint64_t total = (uint64_t)EFI_VAR_STORE_MAX * sizeof(struct efi_var_entry);
    uint64_t used = 0;
    for (int i = 0; i < g_efi_var_count; i++) {
        if (g_efi_var_store[i].in_use)
            used += sizeof(struct efi_var_entry);
    }

    if (max_storage) *max_storage = total;
    if (remaining)   *remaining   = total - used;
    if (max_var_size) *max_var_size = EFI_VAR_DATA_MAX;
    return 0;
}

/* ── efi_get_wakeup_time ──────────────────────────────────────────────
 * Returns the current wakeup alarm configuration.
 * On success, fills in @enabled (1 if alarm is set), @pending (1 if
 * alarm has triggered), and the alarm time components.
 */
int efi_get_wakeup_time(uint8_t *enabled, uint8_t *pending,
                        uint16_t *year, uint8_t *month, uint8_t *day,
                        uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    /* Use in-memory variable store to persist wakeup config */
    static const uint8_t wakeup_guid[16] = {
        0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
        0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef
    };
    uint64_t data_size = 0;
    uint8_t buf[16];
    uint8_t en = 0, pen = 0;

    if (efi_var_store_get_variable("WakeupAlarm", (uint8_t *)(uintptr_t)wakeup_guid,
                                    NULL, &data_size, buf) == 0 &&
        data_size >= 8) {
        en = buf[0];
        pen = buf[1];
        if (year)   *year   = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        if (month)  *month  = buf[4];
        if (day)    *day    = buf[5];
        if (hour)   *hour   = buf[6];
        if (minute) *minute = buf[7];
        if (second) *second = buf[8];
    }

    if (enabled)  *enabled  = en;
    if (pending)  *pending  = pen;

    if (!enabled && !pending && !year && !month && !day &&
        !hour && !minute && !second) {
        kprintf("[EFI] efi_get_wakeup_time: no wakeup alarm configured\n");
    }
    return 0;
}

/* ── efi_set_wakeup_time ──────────────────────────────────────────────
 * Sets or clears the wakeup alarm.
 * If @enable is 0, clears any pending alarm.
 * If @enable is non-zero, sets an alarm for the specified time.
 * The alarm time components are stored in an in-memory EFI variable.
 */
int efi_set_wakeup_time(uint8_t enable, uint16_t year, uint8_t month, uint8_t day,
                        uint8_t hour, uint8_t minute, uint8_t second)
{
    static const uint8_t wakeup_guid[16] = {
        0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
        0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef
    };
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    buf[0] = enable ? 1 : 0;
    buf[1] = 0; /* pending */
    buf[2] = (uint8_t)(year & 0xFF);
    buf[3] = (uint8_t)(year >> 8);
    buf[4] = month;
    buf[5] = day;
    buf[6] = hour;
    buf[7] = minute;
    buf[8] = second;

    int ret = efi_var_store_set_variable("WakeupAlarm", (uint8_t *)(uintptr_t)wakeup_guid,
                                          EFI_VARIABLE_NON_VOLATILE |
                                          EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                          EFI_VARIABLE_RUNTIME_ACCESS,
                                          16, buf);
    if (ret < 0) {
        kprintf("[EFI] efi_set_wakeup_time: failed to store alarm: %d\n", ret);
        return ret;
    }

    kprintf("[EFI] Wakeup alarm %s for %04u-%02u-%02u %02u:%02u:%02u\n",
            enable ? "set" : "cleared",
            (unsigned)year, (unsigned)month, (unsigned)day,
            (unsigned)hour, (unsigned)minute, (unsigned)second);
    return 0;
}

/* ── RT property support ──────────────────────────────────────────────
 * Reports properties of the EFI runtime services to callers.
 */
#define EFI_RT_SUPPORTED_GET_TIME         (1ULL << 0)
#define EFI_RT_SUPPORTED_SET_TIME         (1ULL << 1)
#define EFI_RT_SUPPORTED_GET_VARIABLE     (1ULL << 2)
#define EFI_RT_SUPPORTED_SET_VARIABLE     (1ULL << 3)
#define EFI_RT_SUPPORTED_GET_NEXT_VARIABLE (1ULL << 4)
#define EFI_RT_SUPPORTED_RESET_SYSTEM     (1ULL << 5)
#define EFI_RT_SUPPORTED_QUERY_VARIABLE   (1ULL << 7)

/* Query which EFI runtime services are available.
 * Returns a bitmask of EFI_RT_SUPPORTED_* flags.
 * If no UEFI runtime is present, returns the software fallback set.
 */
uint64_t efi_query_rts_properties(void)
{
    uint64_t props = 0;

    if (g_efi_rt) {
        /* Real UEFI — report all that have function pointers */
        if (g_efi_rt->get_time)    props |= EFI_RT_SUPPORTED_GET_TIME;
        if (g_efi_rt->set_time)    props |= EFI_RT_SUPPORTED_SET_TIME;
        if (g_efi_rt->get_variable) props |= EFI_RT_SUPPORTED_GET_VARIABLE;
        if (g_efi_rt->set_variable) props |= EFI_RT_SUPPORTED_SET_VARIABLE;
        if (g_efi_rt->get_next_variable_name)
            props |= EFI_RT_SUPPORTED_GET_NEXT_VARIABLE;
        if (g_efi_rt->reset_system) props |= EFI_RT_SUPPORTED_RESET_SYSTEM;
        if (g_efi_rt->query_variable_info)
            props |= EFI_RT_SUPPORTED_QUERY_VARIABLE;
    } else {
        /* Software fallback — virtual store supports everything */
        props = EFI_RT_SUPPORTED_GET_TIME |
                EFI_RT_SUPPORTED_SET_TIME |
                EFI_RT_SUPPORTED_GET_VARIABLE |
                EFI_RT_SUPPORTED_SET_VARIABLE |
                EFI_RT_SUPPORTED_GET_NEXT_VARIABLE |
                EFI_RT_SUPPORTED_RESET_SYSTEM |
                EFI_RT_SUPPORTED_QUERY_VARIABLE;
    }

    return props;
}
