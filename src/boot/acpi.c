/*
 * acpi.c — ACPI RSDP/XSDT table walking (boot-time only)
 *
 * Scans the ACPI RSDP (Root System Description Pointer) and walks
 * the XSDT to find ACPI tables by signature. Validates checksums.
 *
 * This implementation runs at boot time before the kernel is fully
 * initialized, using direct physical memory access via PHYS_TO_VIRT.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "errno.h"

/* ── ACPI table signatures and structures ──────────────────────────────── */

#define ACPI_RSDP_SIGNATURE   "RSD PTR "
#define ACPI_XSDT_SIGNATURE   "XSDT"
#define ACPI_RSDT_SIGNATURE   "RSDT"

/* RSDP (version 2.0+) — 36 bytes */
struct acpi_rsdp {
    char     signature[8];        /* "RSD PTR " */
    uint8_t  checksum;            /* sum of first 20 bytes == 0 */
    char     oem_id[6];
    uint8_t  revision;            /* 0=v1, 2=v2+ */
    uint32_t rsdt_addr;           /* 32-bit physical address of RSDT (v1) */
    uint32_t length;              /* total length of RSDP (v2+) */
    uint64_t xsdt_addr;           /* 64-bit physical address of XSDT (v2+) */
    uint8_t  ext_checksum;        /* checksum over entire RSDP */
    uint8_t  reserved[3];
} __attribute__((packed));

/* SDTH (System Description Table Header) — shared by XSDT, RSDT, all SSDTs */
struct acpi_sdth {
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

/* ACPI table header type alias for API compatibility */
struct acpi_table_header {
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

/* ── RSDP scan range ───────────────────────────────────────────────────── */

/* The RSDP is typically found in the EBDA (Extended BIOS Data Area) or
 * in the main BIOS address space between 0xE0000 and 0xFFFFF. */
#define RSDP_SCAN_START     0x000E0000ULL
#define RSDP_SCAN_END       0x00100000ULL
#define RSDP_SCAN_ALIGN     16

/* EBDA segment pointer at 0x40E */
#define EBDA_PTR_ADDR       0x0000040EULL

/* ── Checksum validation ───────────────────────────────────────────────── */

/* Standard ACPI checksum: sum of all bytes must be 0 (mod 256) */
static int acpi_checksum(const void *table, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++)
        sum += bytes[i];
    return (sum == 0) ? 0 : -1;
}

/* ── RSDP discovery ────────────────────────────────────────────────────── */

static int find_rsdp(struct acpi_rsdp *out)
{
    uint64_t scan_start = RSDP_SCAN_START;

    /* Try EBDA first: read segment from BIOS data area */
    uint16_t ebda_seg;
    memcpy(&ebda_seg, (void *)(uintptr_t)EBDA_PTR_ADDR, 2);
    if (ebda_seg != 0) {
        uint64_t ebda_start = ((uint64_t)ebda_seg << 4);
        /* EBDA is typically 1 KB, scan first 1 KB */
        for (uint64_t addr = ebda_start;
             addr < ebda_start + 1024 && addr < RSDP_SCAN_START;
             addr += RSDP_SCAN_ALIGN) {
            char sig[8];
            memcpy(sig, (void *)(uintptr_t)addr, 8);
            if (memcmp(sig, ACPI_RSDP_SIGNATURE, 8) == 0) {
                memcpy(out, (void *)(uintptr_t)addr, sizeof(struct acpi_rsdp));
                /* Validate first-checksum */
                if (acpi_checksum(out, 20) == 0)
                    return 0;
            }
        }
    }

    /* Scan the main BIOS area */
    for (uint64_t addr = RSDP_SCAN_START; addr < RSDP_SCAN_END;
         addr += RSDP_SCAN_ALIGN) {
        char sig[8];
        memcpy(sig, (void *)(uintptr_t)addr, 8);
        if (memcmp(sig, ACPI_RSDP_SIGNATURE, 8) == 0) {
            memcpy(out, (void *)(uintptr_t)addr, sizeof(struct acpi_rsdp));
            /* Validate first-checksum */
            if (acpi_checksum(out, 20) == 0) {
                /* For v2+, also validate extended checksum */
                if (out->revision >= 2) {
                    if (acpi_checksum(out, out->length) == 0)
                        return 0;
                } else {
                    return 0;
                }
            }
        }
    }

    return -1; /* RSDP not found */
}

/* ── Table lookup by signature ─────────────────────────────────────────── */

/* Find an ACPI table by signature. Walks the XSDT (preferred) or RSDT.
 * Returns a pointer to the table (mapped via PHYS_TO_VIRT), or NULL. */
void *acpi_find_table(const char signature[4])
{
    struct acpi_rsdp rsdp;
    if (find_rsdp(&rsdp) < 0) {
        kprintf("[ACPI] RSDP not found\n");
        return NULL;
    }

    kprintf("[ACPI] RSDP found: rev=%u, OEM=%.6s\n",
            rsdp.revision, rsdp.oem_id);

    struct acpi_sdth *sdt_header = NULL;
    uint32_t entry_count = 0;
    uint64_t *entries = NULL;

    /* Prefer XSDT (64-bit entries) */
    if (rsdp.revision >= 2 && rsdp.xsdt_addr != 0) {
        sdt_header = (struct acpi_sdth *)(uintptr_t)rsdp.xsdt_addr;
        if (acpi_checksum(sdt_header, sdt_header->length) < 0) {
            kprintf("[ACPI] XSDT checksum failed\n");
            return NULL;
        }
        entry_count = (sdt_header->length - sizeof(struct acpi_sdth)) / 8;
        entries = (uint64_t *)((uintptr_t)sdt_header + sizeof(struct acpi_sdth));
        kprintf("[ACPI] XSDT: %u entries\n", entry_count);
    } else if (rsdp.rsdt_addr != 0) {
        /* Fall back to RSDT (32-bit entries) */
        sdt_header = (struct acpi_sdth *)(uintptr_t)rsdp.rsdt_addr;
        if (acpi_checksum(sdt_header, sdt_header->length) < 0) {
            kprintf("[ACPI] RSDT checksum failed\n");
            return NULL;
        }
        entry_count = (sdt_header->length - sizeof(struct acpi_sdth)) / 4;
        kprintf("[ACPI] RSDT: %u entries\n", entry_count);
    }

    if (!sdt_header) {
        kprintf("[ACPI] No RSDT/XSDT found\n");
        return NULL;
    }

    /* Walk the entry list */
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t table_addr;
        if (rsdp.revision >= 2 && rsdp.xsdt_addr != 0) {
            table_addr = entries[i];
        } else {
            uint32_t addr32 = ((uint32_t *)entries)[i];
            table_addr = addr32;
        }

        if (table_addr == 0)
            continue;

        struct acpi_sdth *header = (struct acpi_sdth *)(uintptr_t)table_addr;

        /* Check signature match */
        if (memcmp(header->signature, signature, 4) == 0) {
            /* Validate checksum */
            if (acpi_checksum(header, header->length) < 0) {
                kprintf("[ACPI] Table '%.4s' checksum failed\n", signature);
                return NULL;
            }
            kprintf("[ACPI] Found '%.4s' at 0x%llx (len=%u, rev=%u, OEM=%.6s)\n",
                    signature, (unsigned long long)table_addr,
                    header->length, header->revision, header->oem_id);
            return (void *)header;
        }
    }

    kprintf("[ACPI] Table '%.4s' not found\n", signature);
    return NULL;
}

/* ── FADT (Fixed ACPI Description Table) ──────────────────────────────── */

struct acpi_fadt {
    struct acpi_sdth header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable_val;
    uint8_t  acpi_disable_val;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
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
} __attribute__((packed));

/* ── MADT (Multiple APIC Description Table) parser ─────────────────────── */

struct acpi_madt {
    struct acpi_sdth header;
    uint32_t local_apic_addr;
    uint32_t flags;           /* bit 0 = PC-AT compatibility */
    /* Followed by type-length-value entries */
} __attribute__((packed));

/* MADT entry types */
#define MADT_TYPE_LOCAL_APIC      0
#define MADT_TYPE_IO_APIC         1
#define MADT_TYPE_INTERRUPT_SRC   2

struct acpi_madt_entry {
    uint8_t  type;
    uint8_t  length;
    /* type-specific data follows */
} __attribute__((packed));

struct acpi_madt_local_apic {
    uint8_t  type;
    uint8_t  length;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;           /* bit 0 = enabled */
} __attribute__((packed));

/* Walk MADT entries, calling callback for each.
 * Returns 0 on success, -1 if MADT not found. */
int acpi_walk_madt(int (*callback)(struct acpi_madt_entry *entry, void *ctx),
                   void *ctx)
{
    struct acpi_madt *madt = (struct acpi_madt *)acpi_find_table("APIC");
    if (!madt)
        return -1;

    uint32_t madt_len = madt->header.length;
    uint32_t offset = sizeof(struct acpi_madt);

    while (offset + sizeof(struct acpi_madt_entry) <= madt_len) {
        struct acpi_madt_entry *entry =
            (struct acpi_madt_entry *)((uintptr_t)madt + offset);
        if (entry->length == 0)
            break; /* malformed */
        if (offset + entry->length > madt_len)
            break;

        if (callback) {
            int ret = callback(entry, ctx);
            if (ret != 0)
                return ret;
        }

        offset += entry->length;
    }

    return 0;
}

/* ── ACPI subsystem initialisation ─────────────────────────────────────── */

void acpi_init(void)
{
    kprintf("[ACPI] ACPI table walker initialised\n");

    /* Probe MADT for SMP information */
    struct acpi_madt *madt = (struct acpi_madt *)acpi_find_table("APIC");
    if (madt) {
        kprintf("[ACPI] MADT: local APIC at 0x%x, flags=0x%x\n",
                madt->local_apic_addr, madt->flags);

        /* Count enabled local APICs */
        int cpu_count = 0;
        uint32_t madt_len = madt->header.length;
        uint32_t offset = sizeof(struct acpi_madt);

        while (offset + sizeof(struct acpi_madt_entry) <= madt_len) {
            struct acpi_madt_entry *entry =
                (struct acpi_madt_entry *)((uintptr_t)madt + offset);
            if (entry->length == 0) break;
            if (offset + entry->length > madt_len) break;

            if (entry->type == MADT_TYPE_LOCAL_APIC) {
                struct acpi_madt_local_apic *lapic =
                    (struct acpi_madt_local_apic *)entry;
                if (lapic->flags & 1) {
                    cpu_count++;
                    kprintf("[ACPI]   CPU #%d: APIC ID %d (enabled)\n",
                            lapic->acpi_processor_id, lapic->apic_id);
                }
            }
            offset += entry->length;
        }
        kprintf("[ACPI] %d CPU(s) found in MADT\n", cpu_count);
    }

    /* Probe HPET table */
    void *hpet = acpi_find_table("HPET");
    if (hpet) {
        kprintf("[ACPI] HPET table found\n");
    }

    /* Probe FADT for power management info */
    void *fadt = acpi_find_table("FACP");
    if (fadt) {
        kprintf("[ACPI] FADT found\n");
    }
}

/* ── acpi_enable ─────────────────────────────────────────────────────── */

int acpi_enable(void)
{
    struct acpi_fadt *fadt = (struct acpi_fadt *)acpi_find_table("FACP");
    if (!fadt) {
        kprintf("[ACPI] acpi_enable: FADT not found\n");
        return -ENODEV;
    }

    if (fadt->smi_cmd != 0 && fadt->acpi_enable_val != 0) {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable_val);
        kprintf("[ACPI] acpi_enable: wrote 0x%x to SMI_CMD port 0x%x\n",
                fadt->acpi_enable_val, (unsigned)fadt->smi_cmd);
        return 0;
    }

    kprintf("[ACPI] acpi_enable: no SMI_CMD, assuming already enabled\n");
    return 0;
}

/* ── acpi_disable ────────────────────────────────────────────────────── */

int acpi_disable(void)
{
    struct acpi_fadt *fadt = (struct acpi_fadt *)acpi_find_table("FACP");
    if (!fadt) {
        kprintf("[ACPI] acpi_disable: FADT not found\n");
        return -ENODEV;
    }

    if (fadt->smi_cmd != 0 && fadt->acpi_disable_val != 0) {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_disable_val);
        kprintf("[ACPI] acpi_disable: wrote 0x%x to SMI_CMD port 0x%x\n",
                fadt->acpi_disable_val, (unsigned)fadt->smi_cmd);
        return 0;
    }

    kprintf("[ACPI] acpi_disable: no SMI_CMD\n");
    return 0;
}

/* ── acpi_get_table ──────────────────────────────────────────────────── */

static int find_table_by_instance(const char *signature, uint32_t instance,
                                  struct acpi_table_header **out_table)
{
    struct acpi_rsdp rsdp;
    if (find_rsdp(&rsdp) < 0)
        return -ENOENT;

    struct acpi_sdth *sdt_header = NULL;
    uint32_t entry_count = 0;
    int entry_size = 0;
    void *entries = NULL;

    /* Prefer XSDT (64-bit entries) */
    if (rsdp.revision >= 2 && rsdp.xsdt_addr != 0) {
        sdt_header = (struct acpi_sdth *)(uintptr_t)rsdp.xsdt_addr;
        if (acpi_checksum(sdt_header, sdt_header->length) < 0)
            return -ENOENT;
        entry_count = (sdt_header->length - sizeof(struct acpi_sdth)) / 8;
        entries = (void *)((uintptr_t)sdt_header + sizeof(struct acpi_sdth));
        entry_size = 8;
    } else if (rsdp.rsdt_addr != 0) {
        sdt_header = (struct acpi_sdth *)(uintptr_t)rsdp.rsdt_addr;
        if (acpi_checksum(sdt_header, sdt_header->length) < 0)
            return -ENOENT;
        entry_count = (sdt_header->length - sizeof(struct acpi_sdth)) / 4;
        entries = (void *)((uintptr_t)sdt_header + sizeof(struct acpi_sdth));
        entry_size = 4;
    }

    if (!sdt_header)
        return -ENOENT;

    uint32_t found = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t table_addr;
        if (entry_size == 8)
            table_addr = ((uint64_t *)entries)[i];
        else
            table_addr = ((uint32_t *)entries)[i];

        if (table_addr == 0)
            continue;

        struct acpi_sdth *hdr = (struct acpi_sdth *)(uintptr_t)table_addr;
        if (memcmp(hdr->signature, signature, 4) == 0) {
            if (found == instance) {
                if (acpi_checksum(hdr, hdr->length) < 0)
                    return -ENOENT;
                *out_table = (struct acpi_table_header *)hdr;
                return 0;
            }
            found++;
        }
    }

    return -ENOENT;
}

int acpi_get_table(const char *signature, uint32_t instance,
                   struct acpi_table_header **out_table)
{
    if (!signature || !out_table)
        return -EINVAL;

    int ret = find_table_by_instance(signature, instance, out_table);
    if (ret == 0) {
        kprintf("[ACPI] acpi_get_table: found '%.4s' instance %u\n",
                signature, instance);
    } else {
        kprintf("[ACPI] acpi_get_table: '%.4s' instance %u not found\n",
                signature, instance);
        *out_table = NULL;
    }
    return ret;
}

/* ── acpi_put_table ──────────────────────────────────────────────────── */

int acpi_put_table(struct acpi_table_header *table)
{
    if (!table)
        return -EINVAL;
    kprintf("[ACPI] acpi_put_table: released table '%.4s'\n", (char *)table);
    return 0;
}

/* ── acpi_sleep ──────────────────────────────────────────────────────── */

int acpi_sleep(uint32_t sleep_state)
{
    if (sleep_state > 5) {
        kprintf("[ACPI] acpi_sleep: invalid sleep state %u\n", sleep_state);
        return -EINVAL;
    }

    /* Ensure ACPI is enabled before entering sleep */
    int ret = acpi_enable();
    if (ret != 0) {
        kprintf("[ACPI] acpi_sleep: acpi_enable failed\n");
        return ret;
    }

    struct acpi_fadt *fadt = (struct acpi_fadt *)acpi_find_table("FACP");
    if (!fadt) {
        kprintf("[ACPI] acpi_sleep: FADT not found\n");
        return -ENODEV;
    }

    if (fadt->pm1a_cnt_blk == 0) {
        kprintf("[ACPI] acpi_sleep: PM1a_CNT block not available\n");
        return -ENODEV;
    }

    /* Write sleep state to PM1a_CNT: SLP_TYP (bits 12-15) = sleep_state,
     * SLP_EN (bit 13) = 1. Per ACPI spec: (sleep_state << 10) | 0x2000 */
    uint16_t pm1a_cnt_val = (uint16_t)((sleep_state << 10) | 0x2000);
    kprintf("[ACPI] acpi_sleep: entering S%u state (PM1a_CNT=0x%x, val=0x%x)\n",
            sleep_state, (unsigned)fadt->pm1a_cnt_blk, pm1a_cnt_val);

    outw((uint16_t)fadt->pm1a_cnt_blk, pm1a_cnt_val);

    /* If we're still here, the sleep didn't work */
    kprintf("[ACPI] acpi_sleep: S%u failed - system did not enter sleep\n",
            sleep_state);
    return -EIO;
}

/* ── acpi_reboot ─────────────────────────────────────────────────────── */

int acpi_reboot(void)
{
    kprintf("[ACPI] acpi_reboot: system reset via ACPI\n");
    struct acpi_fadt *fadt = (struct acpi_fadt *)acpi_find_table("FACP");
    if (!fadt) {
        kprintf("[ACPI] acpi_reboot: FADT not found, falling back to keyboard reset\n");
        outb(0x64, 0xFE);
        return 0;
    }

    kprintf("[ACPI] acpi_reboot: using FADT reset register\n");

    /* If the reset register is in I/O space (space_id == 1) */
    if (fadt->reset_reg_addr_space == 1 && fadt->reset_reg_address != 0) {
        uint16_t reset_port = (uint16_t)fadt->reset_reg_address;
        switch (fadt->reset_reg_access_size) {
        case 2:
            outw(reset_port, fadt->reset_value);
            break;
        case 3:
            outl(reset_port, fadt->reset_value);
            break;
        case 0:
        case 1:
        default:
            outb(reset_port, fadt->reset_value);
            break;
        }
        kprintf("[ACPI] acpi_reboot: wrote 0x%x to reset port 0x%x\n",
                fadt->reset_value, reset_port);
    } else if (fadt->reset_reg_addr_space == 0 && fadt->reset_reg_address != 0) {
        /* SystemMemory space — write via memory-mapped register */
        volatile uint8_t *reset_ptr =
            (volatile uint8_t *)(uintptr_t)fadt->reset_reg_address;
        *reset_ptr = fadt->reset_value;
        kprintf("[ACPI] acpi_reboot: wrote 0x%x to memory reset register\n",
                fadt->reset_value);
    } else {
        /* No usable reset register, fall back to keyboard controller reset */
        kprintf("[ACPI] acpi_reboot: no usable reset register, falling back\n");
        outb(0x64, 0xFE);
    }

    return 0;
}
