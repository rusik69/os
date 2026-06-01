#define KERNEL_INTERNAL
#include "battery.h"
#include "acpi.h"
#include "io.h"
#include "string.h"
#include "printf.h"

/*
 * ACPI battery driver.
 *
 * Reads battery status by searching the ACPI DSDT/SSDT for _BIF (battery info)
 * and _BST (battery status) control methods. Since we lack an AML interpreter,
 * we scan for known battery device PNP IDs and extract static info from
 * the DSDT definition blocks, then read the embedded controller (EC) ports
 * to get live battery data.
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
#define EC_BATTERY_PRESENT  0x00
#define EC_BATTERY_VOLTAGE  0x04  /* mV, 16-bit */
#define EC_BATTERY_RATE     0x08  /* mW, 16-bit */
#define EC_BATTERY_CAPACITY 0x0C  /* % remaining */
#define EC_BATTERY_STATUS   0x10  /* 0=discharging, 1=charging, 2=full */

/* Battery info registers */
#define EC_BATTERY_INFO_FLAG    0x12  /* bit 0 = present */
#define EC_BATTERY_FULL_CAP     0x14  /* mAh */

static int battery_ec_found = 0;   /* 1 if EC-based battery is present */
static int battery_present = 0;
static uint16_t battery_full_capacity = 0;  /* mAh */

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

/* ACPI header from acpi.c — re-declared for access */
struct acpi_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* FADT structure (subset from acpi.c) */
struct fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    /* ... rest is unused here */
} __attribute__((packed));

struct rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
} __attribute__((packed));

struct rsdt {
    struct acpi_header header;
    uint32_t entries[1];
} __attribute__((packed));

#define PHYS_TO_VIRT(addr) ((void *)((uint64_t)(addr) + 0xFFFF800000000000ULL))

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

    uint32_t num_entries = (rsdt->header.length - sizeof(struct acpi_header)) / 4;
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
    uint32_t aml_len = dsdt_len - sizeof(struct acpi_header);

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
            battery_full_capacity = ec_read_word(EC_BATTERY_FULL_CAP);
            kprintf("[OK] Battery: EC-based battery detected (full capacity: %u mAh)\n",
                    (uint32_t)battery_full_capacity);
            return 0;
        }
    }

    /* Fallback: scan DSDT for battery device */
    if (scan_dsdt_for_battery()) {
        battery_present = 1;
        battery_full_capacity = 4000; /* Default fallback value */
        battery_ec_found = 1;
        kprintf("[OK] Battery: ACPI battery device found in DSDT\n");
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
        status->voltage = ec_read_word(EC_BATTERY_VOLTAGE);   /* mV */
        status->rate    = ec_read_word(EC_BATTERY_RATE);      /* mW */
        status->percentage = ec_read_byte(EC_BATTERY_CAPACITY); /* % */

        uint8_t charge_status = ec_read_byte(EC_BATTERY_STATUS);
        status->charging = (charge_status == 1) ? 1 : 0;

        if (status->percentage > 100) status->percentage = 100;
        if (status->percentage < 0)   status->percentage = 0;

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
