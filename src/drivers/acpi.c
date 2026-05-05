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
    /* ... rest of FADT; we only need pm1a_cnt_blk */
} __attribute__((packed));

static uint16_t pm1a_cnt = 0;    /* PM1a control port */
static uint16_t slp_typa = 0;    /* SLP_TYP_a for S5 */
static int acpi_ready = 0;

static struct rsdp *find_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += 16) {
        if (memcmp((void *)addr, RSDP_SIG, 8) == 0) {
            /* Validate checksum */
            uint8_t *p = (uint8_t *)addr;
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) sum += p[i];
            if (sum == 0) return (struct rsdp *)addr;
        }
    }
    return NULL;
}

void acpi_init(void) {
    /* Search for RSDP in EBDA and BIOS ROM area */
    struct rsdp *rsdp = find_rsdp(0x80000, 0x9FFFF);
    if (!rsdp) rsdp = find_rsdp(0xE0000, 0xFFFFF);
    if (!rsdp) {
        kprintf("[--] ACPI: RSDP not found\n");
        return;
    }

    struct rsdt *rsdt = (struct rsdt *)(uint64_t)rsdp->rsdt_addr;
    if (!rsdt) return;

    /* Validate RSDT signature */
    if (memcmp(rsdt->header.signature, "RSDT", 4) != 0) return;

    /* Walk RSDT entries looking for FADT and MCFG */
    uint32_t num_entries = (rsdt->header.length - sizeof(struct acpi_header)) / 4;
    struct fadt *fadt = NULL;

    for (uint32_t i = 0; i < num_entries; i++) {
        struct acpi_header *hdr = (struct acpi_header *)(uint64_t)rsdt->entries[i];
        if (memcmp(hdr->signature, FADT_SIG, 4) == 0) {
            fadt = (struct fadt *)hdr;
        }
        /* MCFG table: PCIe memory-mapped config space base */
        if (memcmp(hdr->signature, "MCFG", 4) == 0) {
            /* MCFG body: 8 bytes reserved, then allocation structures */
            /* Each entry: 8B base addr, 2B pci segment, 1B start bus, 1B end bus, 4B reserved */
            uint8_t *body = (uint8_t *)hdr + sizeof(struct acpi_header) + 8;
            uint64_t ecam = 0;
            memcpy(&ecam, body, 8);
            if (ecam) {
                pcie_ecam_set_base(ecam);
                kprintf("[OK] PCIe ECAM base: 0x%x\n", ecam);
            }
        }
    }

    if (!fadt) {
        kprintf("[--] ACPI: FADT not found\n");
        return;
    }

    pm1a_cnt = (uint16_t)fadt->pm1a_cnt_blk;
    acpi_ready = 1;

    /* Try to find S5 sleep type from DSDT (simplified: try common values) */
    /* On QEMU, SLP_TYPa for S5 is typically 0x07: write (7<<10)|SLP_EN */
    slp_typa = 0x07;  /* QEMU default */

    kprintf("[OK] ACPI: PM1a control port 0x%x\n", (uint64_t)pm1a_cnt);
}

void acpi_shutdown(void) {
    if (acpi_ready && pm1a_cnt) {
        /* Write SLP_TYP | SLP_EN to PM1a control register */
        /* SLP_EN = bit 13, SLP_TYPa shifted to bits 10-12 */
        outw(pm1a_cnt, (slp_typa << 10) | 0x2000);
    }

    /* QEMU-specific fallback methods */
    outw(0x604, 0x2000);    /* QEMU ACPI poweroff */
    outw(0xB004, 0x2000);   /* Bochs / older QEMU */
    outw(0x4004, 0x3400);   /* Cloud Hypervisor */

    /* If we're still here, halt */
    __asm__ volatile("cli; hlt");
}
