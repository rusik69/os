#define KERNEL_INTERNAL
#include "battery.h"
#include "acpi.h"
#include "io.h"
#include "string.h"
#include "printf.h"

/*
 * ACPI battery driver with aging-aware capacity reporting (Item 107).
 *
 * Reads battery status by searching the ACPI DSDT/SSDT for _BIF (battery info)
 * and _BST (battery status) control methods. Since we lack an AML interpreter,
 * we scan for known battery device PNP IDs and extract static info from
 * the DSDT definition blocks, then read the embedded controller (EC) ports
 * to get live battery data.
 *
 * Aging correction:
 *   Design capacity   = theoretical max capacity when the battery was new
 *   Full charge cap   = actual max capacity the battery can hold now (decreases with wear)
 *   Wear level        = (1 - full_charge / design) * 100%
 *   Displayed %       = min(current_capacity / design_capacity * 100, 100)
 *                     = aging-corrected: shows charge as % of original design capacity
 *
 * EC register-based battery data (common in QEMU, real laptops):
 *   EC command port:  0x66
 *   EC data port:     0x62
 *   EC _BIF data typically at EC offset 0x00..0x13
 *   EC _BST data typically at EC offset 0x14..0x1F
 *
 * Note: This is a platform-independent approach; we probe for the EC
 * and scan the DSDT for battery PNP0C0A device.
 */

/* EC ports */
#define EC_CMD_PORT  0x66
#define EC_DATA_PORT 0x62

/* EC commands */
#define EC_CMD_READ  0x80

/* Battery PNP ID */
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

static int battery_ec_found = 0;      /* 1 if EC-based battery is present */
static int battery_present = 0;

/* Aging-aware capacity tracking (Item 107).
 * design_capacity:   theoretical max when new (from _BIF / EC info registers).
 * full_charge_cap:   actual full capacity now — decreases as the battery ages.
 * These are stored as mAh.  If only one value is available, design == full_charge
 * (no aging data) and wear level is reported as 0. */
static uint32_t battery_design_capacity = 0;     /* mAh */
static uint32_t battery_full_charge_capacity = 0; /* mAh */
static uint32_t battery_cycle_count = 0;          /* 0 = unknown */

/* ── Embedded Controller access ─────────────────────────────────────── */

/* Wait for EC to be ready (OBF = output buffer full) */
static int ec_wait_obf(void) {
    for (int i = 0; i < 10000; i++) {
        if (inb(EC_CMD_PORT) & 0x01)  /* OBF */
            return 0;
        io_wait();
    }
    return -1; /* timeout */
}

/* Wait for EC IBF (input buffer empty) */
static int ec_wait_ibe(void) {
    for (int i = 0; i < 10000; i++) {
        if (!(inb(EC_CMD_PORT) & 0x02))  /* IBF clear */
            return 0;
        io_wait();
    }
    return -1;
}

/* Read a byte from EC RAM at the given offset */
static uint8_t ec_read_byte(uint8_t offset) {
    if (ec_wait_ibe() < 0) return 0xFF;
    outb(EC_CMD_PORT, EC_CMD_READ);
    if (ec_wait_ibe() < 0) return 0xFF;
    outb(EC_DATA_PORT, offset);
    if (ec_wait_obf() < 0) return 0xFF;
    return inb(EC_DATA_PORT);
}

static uint16_t ec_read_word(uint8_t offset) {
    uint16_t lo = ec_read_byte(offset);
    uint16_t hi = ec_read_byte(offset + 1);
    return lo | (hi << 8);
}

/* Read a 32-bit little-endian value from EC RAM */
static uint32_t ec_read_dword(uint8_t offset) {
    uint32_t lo = ec_read_word(offset);
    uint32_t hi = ec_read_word(offset + 2);
    return lo | (hi << 16);
}

/* ── DSDT scanner ───────────────────────────────────────────────────── */

/*
 * Simple DSDT scanner to find battery device presence.
 * We look for the PNP0C0A HID string in the DSDT AML bytecode.
 * This tells us if a battery is described in ACPI.
 *
 * The DSDT is located via the FADT, which is already parsed by acpi.c.
 * We re-use the ACPI table structures from acpi.c.
 */
extern struct rsdp *find_rsdp(uint64_t start, uint64_t end);

/*
 * ACPI header from acpi.h — used for table parsing.
 */
#include "acpi.h"

/* FADT structure (subset from acpi.c) */
struct fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    /* ... rest is unused here */
} __attribute__((packed));

static int scan_dsdt_for_battery(void) {
    /* Find RSDP (same as acpi.c does) */
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

    /* Scan AML for "PNP0C0A" string (battery device) */
    const char *pnp = "PNP0C0A";
    for (uint32_t i = 0; i + 7 < aml_len; i++) {
        if (memcmp(&aml[i], pnp, 7) == 0) {
            return 1;  /* Battery found in DSDT */
        }
    }

    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int battery_init(void) {
    /* Try to probe EC-based battery */
    /* Check if EC responds by reading status register */
    uint8_t ec_status = inb(EC_CMD_PORT);
    if (ec_status != 0xFF) {
        /* EC might exist — try to probe battery */
        battery_ec_found = 1;

        /* Read battery presence byte */
        uint8_t present = ec_read_byte(EC_BATTERY_PRESENT);
        if (present == 0x01) {
            battery_present = 1;

            /* Read full-charge capacity (now) and design capacity (when new).
             * If both are available we can compute wear level; if only one
             * we treat them as equal (no aging data). */
            battery_design_capacity = ec_read_dword(EC_BATTERY_DESIGN_CAP_LOW);
            battery_full_charge_capacity = ec_read_dword(EC_BATTERY_FULL_CAP_LOW);
            battery_cycle_count = ec_read_word(EC_BATTERY_CYCLE_COUNT);

            /* Sanity: if design capacity is 0 or less than full charge,
             * fall back to using full-charge as design (conservative). */
            if (battery_design_capacity == 0 ||
                battery_design_capacity < battery_full_charge_capacity) {
                battery_design_capacity = battery_full_charge_capacity;
            }
            if (battery_full_charge_capacity == 0) {
                /* No full-charge data either — use a default */
                battery_full_charge_capacity = battery_design_capacity;
            }

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
        battery_design_capacity = 4000;     /* Default fallback for design */
        battery_full_charge_capacity = 4000; /* Assume no wear when unknown */
        battery_cycle_count = 0;
        battery_ec_found = 1;
        kprintf("[OK] Battery: ACPI battery device found in DSDT (no EC, using defaults)\n");
        return 0;
    }

    kprintf("[--] Battery: No ACPI battery found\n");
    return -1;
}

int battery_get_status(struct battery_status *status) {
    if (!status) return -1;

    status->present = 0;
    status->charging = 0;
    status->percentage = 0;
    status->voltage = 0;
    status->rate = 0;

    if (!battery_present) return -1;

    if (battery_ec_found) {
        /* Read live battery data from EC */
        uint8_t present = ec_read_byte(EC_BATTERY_PRESENT);
        if (present != 0x01) {
            status->present = 0;
            return 0;
        }

        status->present = 1;
        status->voltage = ec_read_word(EC_BATTERY_VOLTAGE);    /* mV */
        status->rate    = ec_read_word(EC_BATTERY_RATE);       /* mW */
        status->percentage = ec_read_byte(EC_BATTERY_CAPACITY); /* % of current full */

        uint8_t charge_status = ec_read_byte(EC_BATTERY_STATUS);
        status->charging = (charge_status == 1) ? 1 : 0;

        if (status->percentage > 100) status->percentage = 100;
        if (status->percentage < 0)   status->percentage = 0;

        /* ── Aging correction (Item 107) ────────────────────────────────
         * The EC reports percentage relative to the CURRENT full charge
         * capacity (which decreases with wear).  To give a more accurate
         * picture of remaining life, we scale the percentage to represent
         * charge as a fraction of the ORIGINAL design capacity.
         *
         *   corrected_pct = pct * (full_charge / design)
         *
         * Example: design=4000mAh, full_charge=3200mAh (20% wear),
         *          EC reports 50% → true remaining = 50% * 3200/4000 = 40%
         *
         * This way 0% means truly flat, and 100% means fully charged
         * *relative to the original battery*.  The wear level is reported
         * separately via battery_get_health(). */
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

    /* No EC found, return static data */
    status->present = 1;
    status->charging = 1; /* Assume plugged in */
    status->percentage = 100;
    status->voltage = 0;
    status->rate = 0;

    return 0;
}

/* ── Battery health / aging (Item 107) ────────────────────────────── */

int battery_get_health(struct battery_health *health) {
    if (!health) return -1;

    memset(health, 0, sizeof(*health));

    if (!battery_present) return -1;

    health->present = 1;
    health->design_capacity = battery_design_capacity;
    health->full_charge_capacity = battery_full_charge_capacity;
    health->cycle_count = (int)battery_cycle_count;

    /* Read current remaining capacity from EC if available */
    health->current_capacity = 0;
    if (battery_ec_found) {
        uint8_t present = ec_read_byte(EC_BATTERY_PRESENT);
        if (present == 0x01) {
            uint8_t pct = ec_read_byte(EC_BATTERY_CAPACITY);
            if (pct > 100) pct = 100;
            /* current_capacity = full_charge_capacity * pct / 100 */
            health->current_capacity = (battery_full_charge_capacity * (uint32_t)pct) / 100U;
        }
    }

    /* Wear level = (1 - full_charge / design) * 100 %.
     * If design == 0, wear is unknown (report 0). */
    if (battery_design_capacity > 0 && battery_full_charge_capacity > 0) {
        uint32_t fcc = battery_full_charge_capacity;
        uint32_t dc  = battery_design_capacity;
        if (fcc > dc) fcc = dc;  /* clamp: full charge cannot exceed design */
        health->wear_level_pct = (int)((dc - fcc) * 100U / dc);
    } else {
        health->wear_level_pct = 0;
    }

    return 0;
}
#include "module.h"
module_init(battery_init);

/* ── Stub: battery_get_info ─────────────────────────────── */
int battery_get_info(int id, void *info)
{
    (void)id;
    (void)info;
    kprintf("[battery] battery_get_info: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: battery_register ─────────────────────────────── */
int battery_register(void *dev)
{
    (void)dev;
    kprintf("[battery] battery_register: not yet implemented\n");
    return -ENOSYS;
}
