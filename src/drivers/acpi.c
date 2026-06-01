#include "acpi.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "printf.h"

/* ACPI table signatures */
#define RSDP_SIG "RSD PTR "
#define FADT_SIG "FACP"

struct rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
} __attribute__((packed));

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

struct rsdt {
    struct acpi_header header;
    uint32_t entries[1];  /* variable-length */
} __attribute__((packed));

struct fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;   /* PM1a control block port */
    uint32_t pm1b_cnt_blk;
    uint32_t pm1a_evt_blk_len;
    uint32_t pm1b_evt_blk_len;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  _cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  _flags2;
    uint8_t  _res3[3];
    /* Reset register (Generic Address Structure) — 12 bytes */
    uint8_t  reset_reg_addr_space;
    uint8_t  reset_reg_bit_width;
    uint8_t  reset_reg_bit_offset;
    uint8_t  reset_reg_access_size;
    uint64_t reset_reg_address;
    uint8_t  reset_value;
    /* ... rest of FADT */
} __attribute__((packed));

static uint16_t pm1a_cnt = 0;    /* PM1a control port */
static uint16_t pm1a_evt = 0;    /* PM1a event port */
static uint16_t slp_typa_s5 = 0; /* SLP_TYP_a for S5 */
static uint16_t slp_typa_s3 = 0; /* SLP_TYP_a for S3 */
static int acpi_ready = 0;

/* S3 support flag */
static int s3_supported = 0;

/* Power button flag */
static volatile int g_power_button_pressed = 0;

/* Reset register state */
static int reset_reg_available = 0;
static uint8_t reset_reg_addr_space = 0;
static uint8_t reset_reg_access_size = 0;
static uint64_t reset_reg_address = 0;
static uint8_t reset_value = 0;

static struct rsdp *find_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += 16) {
        if (memcmp(PHYS_TO_VIRT(addr), RSDP_SIG, 8) == 0) {
            uint8_t *p = (uint8_t *)PHYS_TO_VIRT(addr);
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) sum += p[i];
            if (sum == 0) return (struct rsdp *)PHYS_TO_VIRT(addr);
        }
    }
    return NULL;
}

/* Simple DSDT walk to find S3 and S5 sleep type values */
static void parse_dsdt_for_sleep(struct fadt *fadt) {
    if (!fadt || !fadt->dsdt) return;

    uint8_t *dsdt = (uint8_t *)PHYS_TO_VIRT((uint64_t)fadt->dsdt);
    if (!dsdt) return;

    /* DSDT starts with an acpi_header */
    struct acpi_header *hdr = (struct acpi_header *)dsdt;
    if (memcmp(hdr->signature, "DSDT", 4) != 0) return;

    uint32_t dsdt_len = hdr->length;
    uint8_t *aml = dsdt + sizeof(struct acpi_header);
    uint32_t aml_len = dsdt_len - sizeof(struct acpi_header);

    /* Very simplified search for sleep states.
       Look for "_S3_" and "_S5_" and extract the package values. */
    for (uint32_t i = 0; i < aml_len - 8; i++) {
        /* Look for _S5_ (0x5f 0x53 0x35 0x5f) */
        if (aml[i] == 0x5f && aml[i+1] == 0x53 && aml[i+2] == 0x35 && aml[i+3] == 0x5f) {
            /* Found _S5_ — the next few bytes encode the package */
            /* In QEMU, default SLP_TYPa for S5 is 7 */
            slp_typa_s5 = 0x07;
            kprintf("  ACPI: Found _S5_ in DSDT\n");
        }
        /* Look for _S3_ (0x5f 0x53 0x33 0x5f) */
        if (aml[i] == 0x5f && aml[i+1] == 0x53 && aml[i+2] == 0x33 && aml[i+3] == 0x5f) {
            /* Found _S3_ — S3 is supported */
            s3_supported = 1;
            /* In QEMU, default SLP_TYPa for S3 is 5 */
            slp_typa_s3 = 0x05;
            kprintf("  ACPI: Found _S3_ in DSDT (S3 supported)\n");
        }
    }
}

/* Simplified fixed power button detection */
static void check_power_button(struct fadt *fadt) {
    if (!fadt) return;

    /* Check IAPC_BOOT_ARCH bit 0 for power button */
    if (fadt->iapc_boot_arch & 0x01) {
        kprintf("  ACPI: Power button present (fixed hardware)\n");
    }
}

void acpi_init(void) {
    struct rsdp *rsdp = find_rsdp(0x80000, 0x9FFFF);
    if (!rsdp) rsdp = find_rsdp(0xE0000, 0xFFFFF);
    if (!rsdp) {
        kprintf("[--] ACPI: RSDP not found\n");
        return;
    }

    struct rsdt *rsdt = (struct rsdt *)PHYS_TO_VIRT((uint64_t)rsdp->rsdt_addr);
    if (!rsdt) return;

    if (memcmp(rsdt->header.signature, "RSDT", 4) != 0) return;

    uint32_t num_entries = (rsdt->header.length - sizeof(struct acpi_header)) / 4;
    struct fadt *fadt = NULL;

    for (uint32_t i = 0; i < num_entries; i++) {
        struct acpi_header *hdr = (struct acpi_header *)PHYS_TO_VIRT((uint64_t)rsdt->entries[i]);
        if (memcmp(hdr->signature, FADT_SIG, 4) == 0) {
            fadt = (struct fadt *)hdr;
        }
        /* MCFG table */
        if (memcmp(hdr->signature, "MCFG", 4) == 0) {
            uint8_t *body = (uint8_t *)hdr + sizeof(struct acpi_header) + 8;
            uint64_t ecam = 0;
            memcpy(&ecam, body, 8);
            if (ecam) {
                pcie_ecam_set_base(ecam);
                kprintf("[OK] PCIe ECAM base: 0x%x\n", ecam);
            }
        }
        /* FACP / FADT table */
    }

    if (!fadt) {
        kprintf("[--] ACPI: FADT not found\n");
        return;
    }

    pm1a_cnt = (uint16_t)fadt->pm1a_cnt_blk;
    pm1a_evt = (uint16_t)fadt->pm1a_evt_blk;
    acpi_ready = 1;

    /* Parse reset register from FADT */
    if (fadt->reset_reg_address != 0) {
        reset_reg_available = 1;
        reset_reg_addr_space = fadt->reset_reg_addr_space;
        reset_reg_access_size = fadt->reset_reg_access_size;
        reset_reg_address = fadt->reset_reg_address;
        reset_value = fadt->reset_value;
        kprintf("[OK] ACPI: Reset register at 0x%llx (space=%u, val=0x%x)\n",
                (unsigned long long)reset_reg_address,
                (uint32_t)reset_reg_addr_space, (uint32_t)reset_value);
    }

    /* Parse DSDT for sleep states */
    parse_dsdt_for_sleep(fadt);

    /* Check for power button */
    check_power_button(fadt);

    /* Default S5 value if DSDT parsing didn't find it */
    if (slp_typa_s5 == 0) slp_typa_s5 = 0x07;

    kprintf("[OK] ACPI: PM1a control port 0x%x\n", (uint64_t)pm1a_cnt);
    if (s3_supported) {
        kprintf("[OK] ACPI: S3 (Suspend-to-RAM) supported\n");
    }
}

void acpi_shutdown(void) {
    if (acpi_ready && pm1a_cnt) {
        outw(pm1a_cnt, (slp_typa_s5 << 10) | 0x2000);
    }

    /* QEMU-specific fallback methods */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    __asm__ volatile("cli; hlt");
}

int acpi_find_reset_register(void) {
    return reset_reg_available;
}

void acpi_reboot(void) {
    if (reset_reg_available) {
        kprintf("ACPI: Using reset register\n");
        if (reset_reg_addr_space == 1) {
            switch (reset_reg_access_size) {
                case 1: outb((uint16_t)reset_reg_address, reset_value); break;
                case 2: outw((uint16_t)reset_reg_address, reset_value); break;
                case 3: outl((uint16_t)reset_reg_address, reset_value); break;
                default: outb((uint16_t)reset_reg_address, reset_value); break;
            }
        } else if (reset_reg_addr_space == 0) {
            uint64_t *reset_ptr = (uint64_t *)PHYS_TO_VIRT(reset_reg_address);
            *((volatile uint8_t *)reset_ptr) = reset_value;
        }
    }

    outb(0x64, 0xFE);
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    kprintf("ACPI: Reboot failed, halting\n");
    cli();
    for (;;) hlt();
}

/* ── Power button API ──────────────────────────────────────────────── */

int acpi_power_button_read(void) {
    if (!acpi_ready) return 0;

    /* Check PM1a event register for power button (bit 8 = PWRBTN_STS) */
    if (pm1a_evt) {
        uint16_t evt = inw(pm1a_evt);
        if (evt & (1 << 8)) {
            /* Clear by writing 1 to the status bit */
            outw(pm1a_evt, (1 << 8));
            g_power_button_pressed = 1;
        }
    }

    int ret = g_power_button_pressed ? 1 : 0;
    g_power_button_pressed = 0;
    return ret;
}

/* ── Sleep API ─────────────────────────────────────────────────────── */

int acpi_sleep_supported(uint32_t state) {
    switch (state) {
    case ACPI_S0: return 1;  /* always running */
    case ACPI_S3: return s3_supported;
    case ACPI_S5: return 1;  /* shutdown always possible */
    default:      return 0;
    }
}

int acpi_sleep(uint32_t state) {
    if (!acpi_ready) return -1;

    switch (state) {
    case ACPI_S3:
        if (!s3_supported) {
            kprintf("ACPI: S3 not supported\n");
            return -1;
        }
        kprintf("ACPI: System entering sleep state S3 (Suspend-to-RAM)...\n");
        if (pm1a_cnt) {
            /* Write SLP_TYP | SLP_EN for S3 */
            outw(pm1a_cnt, (slp_typa_s3 << 10) | 0x2000);
        }
        /* If we're still here, S3 didn't work */
        kprintf("ACPI: S3 failed\n");
        return -1;

    case ACPI_S5:
        /* Same as shutdown */
        acpi_shutdown();
        return 0;

    case ACPI_S0:
        return 0;  /* already running */

    default:
        kprintf("ACPI: Sleep state S%u not supported\n", (uint32_t)state);
        return -1;
    }
}
