#define KERNEL_INTERNAL
#include "battery.h"
#include "acpi.h"
#include "aml_exec.h"
#include "io.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "kernel.h"

/*
 * ACPI battery driver with AML-based _BIF / _BST / _BTP evaluation.
 *
 * Now that the AML interpreter supports method evaluation, we use it as
 * the PRIMARY path for battery data:
 *   - _BIF  (Battery Info)   → design capacity, model, serial, etc.
 *   - _BST  (Battery Status) → charge state, rate, remaining, voltage
 *   - _BTP  (Battery Trip Pt)→ set a capacity-change notification threshold
 *
 * If AML evaluation fails (e.g. no AML namespace, no battery devices in
 * DSDT/SSDT), we fall back to EC register-based reading.
 *
 * Aging-aware capacity reporting (Item 107):
 *   Design capacity   = theoretical max capacity when the battery was new
 *   Full charge cap   = actual max capacity the battery can hold now
 *   Wear level        = (1 - full_charge / design) * 100%
 *   Displayed %       = min(current_capacity / design_capacity * 100, 100)
 *                     = aging-corrected: shows charge as % of original design
 *
 * EC register-based battery data (common in QEMU, real laptops):
 *   EC command port:  0x66
 *   EC data port:     0x62
 */

/* ── EC Ports & Commands (fallback path) ────────────────────────────── */
#define EC_CMD_PORT  0x66
#define EC_DATA_PORT 0x62
#define EC_CMD_READ  0x80
#define PNP_BATTERY "PNP0C0A"

/* Battery data registers (offset into EC RAM) */
#define EC_BATTERY_PRESENT      0x00
#define EC_BATTERY_VOLTAGE      0x04  /* mV, 16-bit */
#define EC_BATTERY_RATE         0x08  /* mW, 16-bit */
#define EC_BATTERY_CAPACITY     0x0C  /* % remaining (0..100) */
#define EC_BATTERY_STATUS       0x10  /* 0=discharging, 1=charging, 2=full */

/* Battery info registers */
#define EC_BATTERY_INFO_FLAG        0x12  /* bit 0 = present */
#define EC_BATTERY_FULL_CAP_LOW     0x14  /* mAh, 16-bit low word */
#define EC_BATTERY_FULL_CAP_HIGH    0x16  /* mAh, 16-bit high word */
#define EC_BATTERY_DESIGN_CAP_LOW   0x18  /* mAh, 16-bit low word */
#define EC_BATTERY_DESIGN_CAP_HIGH  0x1A  /* mAh, 16-bit high word */
#define EC_BATTERY_CYCLE_COUNT      0x1C  /* cycles, 16-bit */

/* ── ACPI Battery Device Tracking ──────────────────────────────────── */

#define MAX_ACPI_BATTERIES      4
#define MAX_BATTERY_PATH        64

/* Known ACPI battery device paths to probe (in priority order) */
static const char *g_battery_paths[] = {
    "\\_SB_.BAT0",
    "\\_SB_.BAT1",
    "\\_SB_.BAT2",
    "\\_SB_.BAT3",
    "\\_SB_.PCI0.LPCB.BAT0",
    "\\_SB_.PCI0.EC0.BAT0",
    "\\_SB_.PCI0.SBRG.EC0.BAT0",
    "\\_SB_.PCI0.LPC.EC.BAT0",
};

/* Per-battery ACPI device state */
struct acpi_battery_dev {
    char    path[MAX_BATTERY_PATH]; /* Namespace path, e.g. "\\_SB_.BAT0" */
    int     present;               /* 1 = device present in namespace */
    int     have_bif;              /* 1 = _BIF method found & cached */
    int     have_bst;              /* 1 = _BST method found */
    int     have_btp;              /* 1 = _BTP method found */

    /* Cached _BIF data (static battery info, evaluated once) */
    uint32_t power_unit;           /* 0 = mWh, 1 = mAh */
    uint32_t design_capacity;      /* mAh or mWh */
    uint32_t last_full_charge;     /* last full charge capacity */
    uint32_t battery_technology;
    uint32_t design_voltage;       /* mV */
    uint32_t warn_capacity;
    uint32_t low_capacity;
    char     model_number[64];
    char     serial_number[64];
    char     battery_type[64];
    char     oem_info[64];
};

/* ── Static State ──────────────────────────────────────────────────── */

static int battery_ec_found = 0;       /* 1 if EC-based battery is present */
static int battery_present = 0;        /* 1 if any battery found */
static int g_aml_init_done = 0;        /* 1 after AML namespace scanned */

/* ── Forward declarations ──────────────────────────────────────────── */
static int battery_path_has_bif(const char *path);
static int battery_path_has_bst(const char *path);
static int battery_path_has_btp(const char *path);
static int battery_aml_evaluate_bif(int bat_idx);
static int battery_aml_evaluate_bst(int bat_idx, struct battery_status *status);
static int battery_aml_set_trip_point(uint32_t capacity);

static struct acpi_battery_dev g_batteries[MAX_ACPI_BATTERIES];
static int g_battery_count = 0;

/* EC fallback cache */
static uint32_t battery_design_capacity = 0;
static uint32_t battery_full_charge_capacity = 0;
static uint32_t battery_cycle_count = 0;

/* ── Embedded Controller access (fallback) ─────────────────────────── */

static int ec_wait_obf(void)
{
    for (int i = 0; i < 10000; i++) {
        if (inb(EC_CMD_PORT) & 0x01)  /* OBF */
            return 0;
        io_wait();
    }
    return -1; /* timeout */
}

static int ec_wait_ibe(void)
{
    for (int i = 0; i < 10000; i++) {
        if (!(inb(EC_CMD_PORT) & 0x02))  /* IBF clear */
            return 0;
        io_wait();
    }
    return -1;
}

static uint8_t ec_read_byte(uint8_t offset)
{
    if (ec_wait_ibe() < 0) return 0xFF;
    outb(EC_CMD_PORT, EC_CMD_READ);
    if (ec_wait_ibe() < 0) return 0xFF;
    outb(EC_DATA_PORT, offset);
    if (ec_wait_obf() < 0) return 0xFF;
    return inb(EC_DATA_PORT);
}

static uint16_t ec_read_word(uint8_t offset)
{
    uint16_t lo = ec_read_byte(offset);
    uint16_t hi = ec_read_byte(offset + 1);
    return lo | (hi << 8);
}

static uint32_t ec_read_dword(uint8_t offset)
{
    uint32_t lo = ec_read_word(offset);
    uint32_t hi = ec_read_word(offset + 2);
    return lo | (hi << 16);
}

/* ── DSDT scanner (fallback) ───────────────────────────────────────── */

struct fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    /* ... rest is unused here */
} __attribute__((packed));

static int scan_dsdt_for_battery(void)
{
    struct rsdp *rsdp = NULL;

    for (uint64_t addr = 0x80000; addr < 0x9FFFF; addr += 16) {
        if (memcmp(PHYS_TO_VIRT(addr), "RSD PTR ", 8) == 0) {
            uint8_t *p = (uint8_t *)PHYS_TO_VIRT(addr);
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) sum += p[i];
            if (sum == 0) { rsdp = (struct rsdp *)PHYS_TO_VIRT(addr); break; }
        }
    }
    if (!rsdp) {
        for (uint64_t addr = 0xE0000; addr < 0xFFFFF; addr += 16) {
            if (memcmp(PHYS_TO_VIRT(addr), "RSD PTR ", 8) == 0) {
                uint8_t *p = (uint8_t *)PHYS_TO_VIRT(addr);
                uint8_t sum = 0;
                for (int i = 0; i < 20; i++) sum += p[i];
                if (sum == 0) { rsdp = (struct rsdp *)PHYS_TO_VIRT(addr); break; }
            }
        }
    }
    if (!rsdp) return 0;

    struct rsdt *rsdt = (struct rsdt *)PHYS_TO_VIRT((uint64_t)rsdp->rsdt_addr);
    if (!rsdt || memcmp(rsdt->header.signature, "RSDT", 4) != 0) return 0;

    uint32_t num_entries = (uint32_t)((rsdt->header.length - sizeof(struct acpi_header)) / 4);
    uint32_t dsdt_addr = 0;

    for (uint32_t i = 0; i < num_entries; i++) {
        struct acpi_header *hdr = (struct acpi_header *)PHYS_TO_VIRT((uint64_t)rsdt->entries[i]);
        if (memcmp(hdr->signature, "FACP", 4) == 0) {
            struct fadt *fadt = (struct fadt *)hdr;
            dsdt_addr = fadt->dsdt;
            break;
        }
    }
    if (!dsdt_addr) return 0;

    uint8_t *dsdt = (uint8_t *)PHYS_TO_VIRT((uint64_t)dsdt_addr);
    struct acpi_header *hdr = (struct acpi_header *)dsdt;
    if (memcmp(hdr->signature, "DSDT", 4) != 0) return 0;

    uint32_t dsdt_len = hdr->length;
    uint8_t *aml = dsdt + sizeof(struct acpi_header);
    uint32_t aml_len = (uint32_t)(dsdt_len - sizeof(struct acpi_header));

    const char *pnp = "PNP0C0A";
    for (uint32_t i = 0; i + 7 < aml_len; i++) {
        if (memcmp(&aml[i], pnp, 7) == 0)
            return 1;
    }
    return 0;
}

/* ── Extract an integer from an AML object ─────────────────────────── */

static uint32_t aml_obj_to_uint32(struct aml_object *obj)
{
    if (!obj) return 0;
    if (obj->type == AML_OBJ_INTEGER)
        return (uint32_t)(obj->value.integer & 0xFFFFFFFFULL);
    return 0;
}

/*
 * Extract a string from an AML object into a fixed-size buffer.
 * Handles AML_OBJ_STRING and AML_OBJ_BUFFER (for Unicode strings
 * that appear in _BIF package elements).
 */
static void aml_obj_to_string(struct aml_object *obj, char *buf, uint32_t buf_size)
{
    if (!obj || !buf || buf_size == 0)
        return;

    buf[0] = '\0';

    if (obj->type == AML_OBJ_STRING && obj->value.string.ptr) {
        uint32_t copy_len = obj->value.string.len;
        if (copy_len >= buf_size)
            copy_len = buf_size - 1;
        memcpy(buf, obj->value.string.ptr, copy_len);
        buf[copy_len] = '\0';
    } else if (obj->type == AML_OBJ_BUFFER && obj->value.buffer.data && obj->value.buffer.length > 0) {
        /* Unicode _BIF strings are often stored as buffers with alternating
         * ASCII/zero bytes.  We collapse to plain ASCII. */
        uint32_t out = 0;
        for (uint32_t i = 0; i < obj->value.buffer.length && out + 1 < buf_size; i++) {
            uint8_t c = obj->value.buffer.data[i];
            if (c == 0)
                continue;  /* skip null bytes (Unicode padding) */
            if (c >= 0x20 && c < 0x7F)
                buf[out++] = (char)c;
        }
        buf[out] = '\0';
    }
}

/* ── ACPI Battery Device Discovery via AML Namespace ───────────────── */

/*
 * Check if a given ACPI namespace path corresponds to a battery device
 * by verifying that _BIF (or _BST) exists as a child method.
 */
static int battery_path_has_bif(const char *path)
{
    char method_path[MAX_BATTERY_PATH];
    struct aml_ns_node *node;

    snprintf(method_path, sizeof(method_path), "%s._BIF", path);
    node = aml_ns_lookup(method_path);
    if (node && node->type == AML_NS_METHOD)
        return 1;

    return 0;
}

/*
 * Probe known ACPI battery device paths in the AML namespace.
 * For each path that has _BIF, populate g_batteries[].
 */
static void battery_scan_acpi_devices(void)
{
    uint32_t num_paths = ARRAY_SIZE(g_battery_paths);

    for (uint32_t i = 0; i < num_paths && g_battery_count < MAX_ACPI_BATTERIES; i++) {
        const char *p = g_battery_paths[i];

        /* First check if the device node exists */
        struct aml_ns_node *dev_node = aml_ns_lookup(p);
        if (!dev_node)
            continue;
        if (dev_node->type != AML_NS_DEVICE)
            continue;

        /* Verify it has _BIF (battery info method) */
        if (!battery_path_has_bif(p))
            continue;

        /* Found an ACPI battery device */
        struct acpi_battery_dev *bat = &g_batteries[g_battery_count];
        memset(bat, 0, sizeof(*bat));

        strncpy(bat->path, p, sizeof(bat->path) - 1);
        bat->path[sizeof(bat->path) - 1] = '\0';
        bat->present = 1;
        bat->have_bif = 1;
        bat->have_bst = battery_path_has_bst(p);
        bat->have_btp = battery_path_has_btp(p);

        /* Evaluate _BIF to cache static battery info */
        if (bat->have_bif)
            battery_aml_evaluate_bif(g_battery_count);

        g_battery_count++;
        kprintf("[BATTERY] ACPI battery device found at %s "
                "(BIF=%d BST=%d BTP=%d)\n",
                p, bat->have_bif, bat->have_bst, bat->have_btp);
    }
}

/* ── _BIF Evaluation ────────────────────────────────────────────────── */

/*
 * Evaluate the _BIF control method for the given battery index.
 * Populates the cached battery info structure.
 *
 * _BIF returns a Package (ACPI v6.3, Section 10.2.2.2):
 *   [0] Power Unit          – DWORD (0=mWh, 1=mAh)
 *   [1] Design Capacity     – DWORD
 *   [2] Last Full Charge    – DWORD
 *   [3] Battery Technology  – DWORD (0=rechargeable, 1=non-rechargeable)
 *   [4] Design Voltage      – DWORD (mV)
 *   [5] Warn Capacity       – DWORD (capacity of warning)
 *   [6] Low Capacity        – DWORD (capacity of low)
 *   [7] Model Number        – Unicode String
 *   [8] Serial Number       – Unicode String
 *   [9] Battery Type        – Unicode String
 *   [10] OEM Info           – Unicode String
 */
static int battery_aml_evaluate_bif(int bat_idx)
{
    struct acpi_battery_dev *bat;
    struct aml_object *result, *elem;
    char method_path[80];
    int ret = -1;

    if (bat_idx < 0 || bat_idx >= g_battery_count)
        return -1;

    bat = &g_batteries[bat_idx];
    if (!bat->present || !bat->have_bif)
        return -1;

    snprintf(method_path, sizeof(method_path), "%s._BIF", bat->path);

    result = aml_evaluate_method(method_path, NULL, 0);
    if (!result) {
        kprintf("[BATTERY] _BIF evaluation failed for %s\n", bat->path);
        return -1;
    }

    if (result->type != AML_OBJ_PACKAGE) {
        kprintf("[BATTERY] _BIF did not return a Package (type=%u)\n",
                (unsigned int)result->type);
        goto done;
    }

    if (result->value.package.count < 7) {
        kprintf("[BATTERY] _BIF package too short (%u elements)\n",
                (unsigned int)result->value.package.count);
        goto done;
    }

    /* Element [0]: Power Unit */
    elem = &result->value.package.elements[0];
    bat->power_unit = aml_obj_to_uint32(elem);

    /* Element [1]: Design Capacity */
    elem = &result->value.package.elements[1];
    bat->design_capacity = aml_obj_to_uint32(elem);

    /* Element [2]: Last Full Charge Capacity */
    elem = &result->value.package.elements[2];
    bat->last_full_charge = aml_obj_to_uint32(elem);

    /* Element [3]: Battery Technology */
    elem = &result->value.package.elements[3];
    bat->battery_technology = aml_obj_to_uint32(elem);

    /* Element [4]: Design Voltage */
    elem = &result->value.package.elements[4];
    bat->design_voltage = aml_obj_to_uint32(elem);

    /* Element [5]: Design Capacity of Warning */
    elem = &result->value.package.elements[5];
    bat->warn_capacity = aml_obj_to_uint32(elem);

    /* Element [6]: Design Capacity of Low */
    elem = &result->value.package.elements[6];
    bat->low_capacity = aml_obj_to_uint32(elem);

    /* Element [7]: Model Number (string, optional) */
    if (result->value.package.count > 7) {
        elem = &result->value.package.elements[7];
        aml_obj_to_string(elem, bat->model_number, sizeof(bat->model_number));
    }

    /* Element [8]: Serial Number (string, optional) */
    if (result->value.package.count > 8) {
        elem = &result->value.package.elements[8];
        aml_obj_to_string(elem, bat->serial_number, sizeof(bat->serial_number));
    }

    /* Element [9]: Battery Type (string, optional) */
    if (result->value.package.count > 9) {
        elem = &result->value.package.elements[9];
        aml_obj_to_string(elem, bat->battery_type, sizeof(bat->battery_type));
    }

    /* Element [10]: OEM Info (string, optional) */
    if (result->value.package.count > 10) {
        elem = &result->value.package.elements[10];
        aml_obj_to_string(elem, bat->oem_info, sizeof(bat->oem_info));
    }

    kprintf("[BATTERY] %s _BIF: unit=%u design=%u full=%u "
            "tech=%u volt=%u warn=%u low=%u\n",
            bat->path,
            (unsigned int)bat->power_unit,
            (unsigned int)bat->design_capacity,
            (unsigned int)bat->last_full_charge,
            (unsigned int)bat->battery_technology,
            (unsigned int)bat->design_voltage,
            (unsigned int)bat->warn_capacity,
            (unsigned int)bat->low_capacity);

    if (bat->model_number[0])
        kprintf("[BATTERY]   Model: %s\n", bat->model_number);
    if (bat->serial_number[0])
        kprintf("[BATTERY]   S/N:   %s\n", bat->serial_number);
    if (bat->battery_type[0])
        kprintf("[BATTERY]   Type:  %s\n", bat->battery_type);

    ret = 0;

done:
    aml_free_object(result);
    return ret;
}

/* ── _BST Evaluation ────────────────────────────────────────────────── */

/*
 * Evaluate the _BST control method for the given battery index.
 * Returns battery status data in the provided struct.
 *
 * _BST returns a Package:
 *   [0] Battery State     – DWORD (bit 0=discharging, bit 1=charging, bit 2=critical)
 *   [1] Present Rate      – DWORD (mW or mA)
 *   [2] Remaining Capacity – DWORD (mWh or mAh)
 *   [3] Present Voltage   – DWORD (mV)
 */
static int battery_aml_evaluate_bst(int bat_idx, struct battery_status *status)
{
    struct acpi_battery_dev *bat;
    struct aml_object *result, *elem;
    char method_path[80];
    int ret = -1;
    uint32_t bst_state = 0;
    uint32_t bst_rate = 0;
    uint32_t bst_capacity = 0;
    uint32_t bst_voltage = 0;

    if (!status || bat_idx < 0 || bat_idx >= g_battery_count)
        return -1;

    bat = &g_batteries[bat_idx];
    if (!bat->present || !bat->have_bst)
        return -1;

    snprintf(method_path, sizeof(method_path), "%s._BST", bat->path);

    result = aml_evaluate_method(method_path, NULL, 0);
    if (!result) {
        kprintf("[BATTERY] _BST evaluation failed for %s\n", bat->path);
        return -1;
    }

    if (result->type != AML_OBJ_PACKAGE) {
        kprintf("[BATTERY] _BST did not return a Package (type=%u)\n",
                (unsigned int)result->type);
        goto done;
    }

    if (result->value.package.count < 4) {
        kprintf("[BATTERY] _BST package too short (%u elements)\n",
                (unsigned int)result->value.package.count);
        goto done;
    }

    /* Element [0]: Battery State */
    elem = &result->value.package.elements[0];
    bst_state = aml_obj_to_uint32(elem);

    /* Element [1]: Present Rate */
    elem = &result->value.package.elements[1];
    bst_rate = aml_obj_to_uint32(elem);

    /* Element [2]: Remaining Capacity */
    elem = &result->value.package.elements[2];
    bst_capacity = aml_obj_to_uint32(elem);

    /* Element [3]: Present Voltage */
    elem = &result->value.package.elements[3];
    bst_voltage = aml_obj_to_uint32(elem);

    /* Fill in the status struct */
    status->present = 1;
    status->charging = (bst_state & 0x02) ? 1 : 0;
    status->voltage = bst_voltage;
    status->rate = bst_rate;

    /* Compute percentage from remaining capacity vs design capacity */
    if (bat->design_capacity > 0) {
        uint32_t pct = bst_capacity * 100U / bat->design_capacity;
        if (pct > 100) pct = 100;
        status->percentage = (int)pct;
    } else {
        status->percentage = 50; /* unknown, report mid-range */
    }

    ret = 0;

done:
    aml_free_object(result);
    return ret;
}

/* ── _BTP Evaluation ────────────────────────────────────────────────── */

/*
 * Check if a battery path has _BTP (Battery Trip Point) defined.
 */
static int battery_path_has_btp(const char *path)
{
    char method_path[MAX_BATTERY_PATH];
    struct aml_ns_node *node;

    snprintf(method_path, sizeof(method_path), "%s._BTP", path);
    node = aml_ns_lookup(method_path);
    if (node && node->type == AML_NS_METHOD)
        return 1;

    return 0;
}

/*
 * Check if a battery path has _BST (Battery Status) defined.
 */
static int battery_path_has_bst(const char *path)
{
    char method_path[MAX_BATTERY_PATH];
    struct aml_ns_node *node;

    snprintf(method_path, sizeof(method_path), "%s._BST", path);
    node = aml_ns_lookup(method_path);
    if (node && node->type == AML_NS_METHOD)
        return 1;

    return 0;
}

/*
 * Evaluate _BTP (Battery Trip Point) for the first battery device.
 * Sets a capacity threshold in mAh/mWh; when the remaining capacity
 * crosses this value (falling below or rising above), the ACPI firmware
 * generates a Notify(0x80) event.
 *
 * @param capacity  Trip point capacity in mAh or mWh (depends on power unit)
 * Returns 0 on success, -1 if _BTP not supported or evaluation failed.
 */
static int battery_aml_set_trip_point(uint32_t capacity)
{
    struct aml_object *args[1];
    struct aml_object *result;
    struct aml_object *cap_obj;
    char method_path[80];
    int ret = -1;

    /* Use the first battery that supports _BTP */
    for (int i = 0; i < g_battery_count; i++) {
        if (g_batteries[i].present && g_batteries[i].have_btp) {
            snprintf(method_path, sizeof(method_path), "%s._BTP",
                     g_batteries[i].path);

            cap_obj = aml_create_integer(capacity);
            if (!cap_obj)
                return -1;

            args[0] = cap_obj;
            result = aml_evaluate_method(method_path, args, 1);

            aml_free_object(cap_obj);

            if (result) {
                kprintf("[BATTERY] _BTP set to %u on %s\n",
                        (unsigned int)capacity, g_batteries[i].path);
                aml_free_object(result);
                ret = 0;
            } else {
                kprintf("[BATTERY] _BTP evaluation failed on %s\n",
                        g_batteries[i].path);
            }
            return ret;
        }
    }

    kprintf("[BATTERY] No battery with _BTP support found\n");
    return -1;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int battery_init(void)
{
    /*
     * Phase 1: Try to discover ACPI battery devices via AML namespace.
     * This is the primary path now that AML interpreter is available.
     */
    if (aml_ns_get_count() > 0) {
        battery_scan_acpi_devices();
    }

    /*
     * Phase 2: If AML found batteries, we're done (the EC path is only
     * needed if the AML namespace doesn't have battery data).
     */
    if (g_battery_count > 0) {
        battery_present = 1;
        kprintf("[BATTERY] %d ACPI battery device(s) detected via AML namespace\n",
                g_battery_count);

        /* Set a default trip point at 10% of design capacity */
        for (int i = 0; i < g_battery_count; i++) {
            if (g_batteries[i].present && g_batteries[i].have_btp &&
                g_batteries[i].design_capacity > 0) {
                uint32_t trip = g_batteries[i].design_capacity / 10;
                if (trip < 100) trip = 100;
                battery_aml_set_trip_point(trip);
                break;
            }
        }

        return 0;
    }

    /*
     * Phase 3: Fallback to EC-based battery detection.
     */
    uint8_t ec_status = inb(EC_CMD_PORT);
    if (ec_status != 0xFF) {
        battery_ec_found = 1;
        uint8_t present = ec_read_byte(EC_BATTERY_PRESENT);
        if (present == 0x01) {
            battery_present = 1;
            battery_design_capacity = ec_read_dword(EC_BATTERY_DESIGN_CAP_LOW);
            battery_full_charge_capacity = ec_read_dword(EC_BATTERY_FULL_CAP_LOW);
            battery_cycle_count = ec_read_word(EC_BATTERY_CYCLE_COUNT);

            if (battery_design_capacity == 0 ||
                battery_design_capacity < battery_full_charge_capacity) {
                battery_design_capacity = battery_full_charge_capacity;
            }
            if (battery_full_charge_capacity == 0)
                battery_full_charge_capacity = battery_design_capacity;

            uint32_t wear = 0;
            if (battery_design_capacity > 0) {
                wear = (battery_design_capacity - battery_full_charge_capacity) * 100U
                       / battery_design_capacity;
            }

            kprintf("[OK] Battery: EC-based battery detected "
                    "(design=%u mAh, full=%u mAh, wear=%u%%, cycles=%u)\n",
                    (unsigned int)battery_design_capacity,
                    (unsigned int)battery_full_charge_capacity,
                    (unsigned int)wear,
                    (unsigned int)battery_cycle_count);
            return 0;
        }
    }

    /* Fallback: scan DSDT for battery device */
    if (scan_dsdt_for_battery()) {
        battery_present = 1;
        battery_design_capacity = 4000;
        battery_full_charge_capacity = 4000;
        battery_cycle_count = 0;
        battery_ec_found = 1;
        kprintf("[OK] Battery: ACPI battery device found in DSDT (no EC, using defaults)\n");
        return 0;
    }

    kprintf("[--] Battery: No ACPI battery found\n");
    return -1;
}

int battery_get_status(struct battery_status *status)
{
    if (!status)
        return -EINVAL;

    memset(status, 0, sizeof(*status));

    if (!battery_present)
        return -ENODEV;

    /* Primary path: AML _BST evaluation */
    if (g_battery_count > 0) {
        int ret = battery_aml_evaluate_bst(0, status);
        if (ret == 0)
            return 0;
        /* Fall through to EC-based reading on failure */
    }

    /* Fallback path: EC register-based reading */
    if (battery_ec_found) {
        uint8_t present = ec_read_byte(EC_BATTERY_PRESENT);
        if (present != 0x01) {
            status->present = 0;
            return 0;
        }

        status->present = 1;
        status->voltage = ec_read_word(EC_BATTERY_VOLTAGE);
        status->rate    = ec_read_word(EC_BATTERY_RATE);
        status->percentage = (int)ec_read_byte(EC_BATTERY_CAPACITY);

        uint8_t charge_status = ec_read_byte(EC_BATTERY_STATUS);
        status->charging = (charge_status == 1) ? 1 : 0;

        if (status->percentage > 100) status->percentage = 100;
        if (status->percentage < 0)   status->percentage = 0;

        /* Aging correction (Item 107) */
        if (battery_design_capacity > 0 &&
            battery_full_charge_capacity <= battery_design_capacity) {
            uint32_t ratio_x100 = battery_full_charge_capacity * 100U
                                  / battery_design_capacity;
            status->percentage = (status->percentage * (int)ratio_x100) / 100;
            if (status->percentage > 100) status->percentage = 100;
            if (status->percentage < 0)   status->percentage = 0;
        }

        return 0;
    }

    /* No EC found, return static fallback data */
    status->present = 1;
    status->charging = 1;
    status->percentage = 100;
    status->voltage = 0;
    status->rate = 0;

    return 0;
}

int battery_get_health(struct battery_health *health)
{
    if (!health)
        return -EINVAL;

    memset(health, 0, sizeof(*health));

    if (!battery_present)
        return -ENODEV;

    /* Primary path: AML _BIF cached data */
    if (g_battery_count > 0) {
        struct acpi_battery_dev *bat = &g_batteries[0];

        health->present = 1;
        health->design_capacity = bat->design_capacity;
        health->full_charge_capacity = bat->last_full_charge;
        health->cycle_count = 0; /* _BIF doesn't provide cycle count */

        /* Try to get current remaining capacity from _BST */
        struct battery_status bst;
        if (battery_aml_evaluate_bst(0, &bst) == 0) {
            /* bst.percentage is % of design capacity, compute current capacity */
            if (bat->design_capacity > 0) {
                health->current_capacity = bat->design_capacity
                    * (uint32_t)bst.percentage / 100U;
            }
        }

        /* Wear level */
        if (bat->design_capacity > 0 && bat->last_full_charge > 0) {
            uint32_t fcc = bat->last_full_charge;
            uint32_t dc  = bat->design_capacity;
            if (fcc > dc) fcc = dc;
            health->wear_level_pct = (int)((dc - fcc) * 100U / dc);
        }

        return 0;
    }

    /* Fallback path: EC-based or cached data */
    health->present = 1;
    health->design_capacity = battery_design_capacity;
    health->full_charge_capacity = battery_full_charge_capacity;
    health->cycle_count = (int)battery_cycle_count;
    health->current_capacity = 0;

    if (battery_ec_found) {
        uint8_t present = ec_read_byte(EC_BATTERY_PRESENT);
        if (present == 0x01) {
            uint8_t pct = ec_read_byte(EC_BATTERY_CAPACITY);
            if (pct > 100) pct = 100;
            health->current_capacity = (battery_full_charge_capacity * (uint32_t)pct) / 100U;
        }
    }

    if (battery_design_capacity > 0 && battery_full_charge_capacity > 0) {
        uint32_t fcc = battery_full_charge_capacity;
        uint32_t dc  = battery_design_capacity;
        if (fcc > dc) fcc = dc;
        health->wear_level_pct = (int)((dc - fcc) * 100U / dc);
    }

    return 0;
}

int battery_get_info(int id, struct battery_info *info)
{
    if (!info)
        return -EINVAL;

    memset(info, 0, sizeof(*info));

    /* Ignore id for now — use the first battery */
    (void)id;

    if (!battery_present)
        return -ENODEV;

    /* Try AML-based data first */
    if (g_battery_count > 0) {
        struct acpi_battery_dev *bat = &g_batteries[0];

        info->power_unit = bat->power_unit;
        info->design_capacity = bat->design_capacity;
        info->last_full_charge = bat->last_full_charge;
        info->battery_technology = bat->battery_technology;
        info->design_voltage = bat->design_voltage;
        info->warn_capacity = bat->warn_capacity;
        info->low_capacity = bat->low_capacity;

        memcpy(info->model_number, bat->model_number, sizeof(info->model_number));
        memcpy(info->serial_number, bat->serial_number, sizeof(info->serial_number));
        memcpy(info->battery_type, bat->battery_type, sizeof(info->battery_type));
        memcpy(info->oem_info, bat->oem_info, sizeof(info->oem_info));

        return 0;
    }

    /* Fallback: EC-based data */
    info->power_unit = 1;   /* mAh */
    info->design_capacity = battery_design_capacity;
    info->last_full_charge = battery_full_charge_capacity;
    info->battery_technology = 0; /* rechargeable */
    info->design_voltage = 0;
    info->warn_capacity = 0;
    info->low_capacity = 0;

    return 0;
}

int battery_set_trip_point(uint32_t capacity)
{
    return battery_aml_set_trip_point(capacity);
}

#include "module.h"
module_init(battery_init);
