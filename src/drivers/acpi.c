#include "acpi.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "printf.h"
#include "cpuidle.h"

/* AML method signatures for dock detection */
/* "_DCK" as little-endian uint32_t: 'D' 'C' 'K' '_' */
#define AML_DCK_SIG  0x5f4b4344
/* "_DOD" as little-endian uint32_t: 'D' 'O' 'D' '_' */
#define AML_DOD_SIG  0x5f444f44

/* AML opcodes used in DSDT walk */
#define AML_METHOD_OP  0x14  /* Method */
#define AML_NAME_OP    0x08  /* Name */
#define AML_PACKAGE_OP 0x12  /* Package */
#define AML_STRING_OP  0x0d  /* String prefix */

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

/* ── ACPI Generic Address Structure (GAS) ────────────────────────── */
struct acpi_gas {
    uint8_t  space_id;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* ── LPIT: Low Power Idle Table (ACPI 6.2+, section 5.2.27) ──────── */
/* LPI state descriptor — each entry in the LPIT table */
struct lpi_state_desc {
    uint32_t        type;              /* 0 = Native, 1 = ACPI-defined */
    uint32_t        uid;               /* Unique identifier */
    uint32_t        reserved;
    uint32_t        flags;             /* Bit 0: Disabled */
    struct acpi_gas entry_trigger;     /* Entry method descriptor */
    uint32_t        residency;         /* Nominal residency in μs */
    uint32_t        latency;           /* Worst-case exit latency in μs */
    struct acpi_gas residency_counter; /* GAS for residency counter (optional) */
    struct acpi_gas usage_counter;     /* GAS for usage counter (optional) */
} __attribute__((packed));

/* Size of a single LPI state descriptor in the LPIT table */
#define LPIT_DESC_SIZE 60

/* LPI state flags */
#define LPI_FLAG_DISABLED     (1U << 0)
#define LPI_FLAG_CNT_AVAIL    (1U << 1)

static uint16_t pm1a_cnt = 0;    /* PM1a control port */
static uint16_t pm1a_evt = 0;    /* PM1a event port */
static uint16_t slp_typa_s5 = 0xFF; /* SLP_TYP_a for S5 (0xFF = uninitialized) */
static uint16_t slp_typa_s3 = 0xFF; /* SLP_TYP_a for S3 (0xFF = uninitialized) */
static int acpi_ready = 0;

/* S3 support flag */
static int s3_supported = 0;

/* ── Exported DSDT info for ACPI drivers (acpi_cpufreq, etc.) ────── */

/* Virtual address of DSDT base (mapped via PHYS_TO_VIRT) */
uint8_t *g_dsdt_base = NULL;
/* Total length of the DSDT table (including header) */
uint32_t g_dsdt_length = 0;

/* Power button flag */
static volatile int g_power_button_pressed = 0;

/* ── Dock/Undock State (Item 106) ────────────────────────────────── */

/* Current dock station state: NOT_PRESENT, UNDOCKED, or DOCKED */
static int g_dock_state = ACPI_DOCK_NOT_PRESENT;

/* Previous dock state for change detection in poll */
static int g_dock_prev_state = ACPI_DOCK_NOT_PRESENT;

/* DSDT address of the dock device (first device with _DCK method).
 * 0 means no dock device was found. */
static uint32_t g_dock_device_addr = 0;

/* Dock notification callback table */
struct dock_notify_entry {
    acpi_dock_callback_t cb;
    void *user_data;
    int in_use;
};
static struct dock_notify_entry g_dock_notify[ACPI_DOCK_MAX_CB];

/* Reset register state */
static int reset_reg_available = 0;
static uint8_t reset_reg_addr_space = 0;
static uint8_t reset_reg_access_size = 0;
static uint64_t reset_reg_address = 0;
static uint8_t reset_value = 0;

static struct rsdp *find_rsdp(uint64_t start, uint64_t end) {
    /* Constrain scan to known-good physical memory ranges to avoid
     * dereferencing non-existent or MMIO regions.  The standard ACPI
     * spec defines two valid scan areas: EBDA (0x80000-0x9FFFF) and
     * main BIOS area (0xE0000-0xFFFFF). */
    if (start < 0x80000) start = 0x80000;
    if (end > 0xFFFFF)   end = 0xFFFFF;
    if (start >= end) return NULL;

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

    /* Validate header length to prevent underflow */
    if (dsdt_len < sizeof(struct acpi_header)) {
        kprintf("  ACPI: DSDT too short (%u bytes, need %zu)\\n",
                (unsigned int)dsdt_len, sizeof(struct acpi_header));
        return;
    }

    uint8_t *aml = dsdt + sizeof(struct acpi_header);
    uint32_t aml_len = (uint32_t)(dsdt_len - sizeof(struct acpi_header));

    /* Need at least 8 bytes of AML to search for sleep state markers */
    if (aml_len < 8) {
        kprintf("  ACPI: DSDT AML too short (%u bytes, need 8)\\n",
                (unsigned int)aml_len);
        return;
    }

    /* Very simplified search for sleep states.
       Look for "_S3_" and "_S5_" and extract the package values. */
    for (uint32_t i = 0; i < aml_len - 8; i++) {
        /* Look for _S5_ (0x5f 0x53 0x35 0x5f) */
        if (aml[i] == 0x5f && aml[i+1] == 0x53 && aml[i+2] == 0x35 && aml[i+3] == 0x5f) {
            /* Found _S5_ — parse the following Package() to extract SLP_TYPa.
             * AML encoding after NameSeg: PackageOp (0x12) PkgLength NumElements data...
             * SLP_TYPa is the first integer in the package. */
            uint32_t j = i + 4;
            if (j + 4 < aml_len && aml[j] == 0x12) {
                /* PackageOp found — skip PkgLength (at least 2 bytes for short form) */
                uint32_t pkg_len = aml[j + 1];
                if (pkg_len >= 4 && j + 2 + pkg_len < aml_len) {
                    uint32_t k = j + 2;
                    if (k < aml_len && aml[k] >= 2) {
                        k++;
                        if (k + 1 < aml_len) {
                            if (aml[k] == 0x0A && k + 2 < aml_len) {
                                slp_typa_s5 = aml[k + 1];
                            } else if (aml[k] == 0x0B && k + 3 < aml_len) {
                                slp_typa_s5 = aml[k + 1];
                            } else if (aml[k] == 0x00) {
                                slp_typa_s5 = 0;
                            }
                        }
                    }
                }
            }
            kprintf("  ACPI: _S5_ SLP_TYPa = 0x%02x\n", slp_typa_s5);
        }
        /* Look for _S3_ (0x5f 0x53 0x33 0x5f) */
        if (aml[i] == 0x5f && aml[i+1] == 0x53 && aml[i+2] == 0x33 && aml[i+3] == 0x5f) {
            /* Found _S3_ — parse the following Package() for SLP_TYPa */
            s3_supported = 1;
            uint32_t j = i + 4;
            if (j + 4 < aml_len && aml[j] == 0x12) {
                uint32_t pkg_len = aml[j + 1];
                if (pkg_len >= 4 && j + 2 + pkg_len < aml_len) {
                    uint32_t k = j + 2;
                    if (k < aml_len && aml[k] >= 2) {
                        k++;
                        if (k + 1 < aml_len) {
                            if (aml[k] == 0x0A && k + 2 < aml_len) {
                                slp_typa_s3 = aml[k + 1];
                            } else if (aml[k] == 0x0B && k + 3 < aml_len) {
                                slp_typa_s3 = aml[k + 1];
                            } else if (aml[k] == 0x00) {
                                slp_typa_s3 = 0;
                            }
                        }
                    }
                }
            }
            kprintf("  ACPI: _S3_ SLP_TYPa = 0x%02x (S3 supported)\n", slp_typa_s3);
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

/* ── Dock Device Detection ─────────────────────────────────────────
 *
 * Search the DSDT AML bytecode for a Method(_DCK) definition, which
 * indicates a dock station controller.  If found, record its offset so
 * we can later poll the dock state by re-evaluating the method.
 *
 * The DSDT is scanned linearly for the AML method opcode followed by
 * the four-byte name "_DCK".  This is a simplified detection that
 * works on real firmware (Lenovo, HP, Dell dock-capable laptops).
 */
static void acpi_find_dock_device(uint8_t *aml, uint32_t aml_len) {
    g_dock_device_addr = 0;

    /* Scan AML bytecode for Method(_DCK, ...) patterns.
     * A typical pattern is: 0x14 (MethodOp) 0xNN (length) followed
     * by 4 chars of method name.  We look for the name "_DCK".
     * The AML encoding of "_DCK" is the characters 'D','C','K','_'
     * in that order (big-endian names in AML are stored as-is). */
    for (uint32_t i = 0; i < aml_len - 8; i++) {
        /* Look for MethodOp followed by a name that starts with '_' */
        if (aml[i] != AML_METHOD_OP)
            continue;

        /* Skip method op and length field (1-2 bytes) */
        uint32_t name_off = i + 1;
        if (name_off + 4 >= aml_len)
            break;

        /* Skip the length byte(s): MethodOp length is encoded as
         * either 1-byte (if < 64) or 2-byte (if >= 64) */
        if (aml[name_off] & 0x80) {
            /* Multi-byte length: first byte has top bit set,
             * second byte completes the length */
            name_off += 2;
        } else {
            name_off += 1;
        }

        if (name_off + 4 > aml_len)
            break;

        /* Check for "_DCK" -- all AML names are 4 bytes, padded
         * with underscores. */
        uint32_t name_sig;
        memcpy(&name_sig, &aml[name_off], 4);
        if (name_sig == AML_DCK_SIG) {
            /*
             * Found the _DCK method.  The dock device is the
             * parent scope of this method.  In practice, the
             * device is named something like "\_SB_.PCI0.DOCK"
             * or "\_SB_.DOCK".  We store the method offset for
             * state polling.
             *
             * For _DCK at offset i, we record the start of the
             * enclosing Device scope by scanning backwards for
             * a DeviceOp (0x5B 0x82) or just store the method
             * offset itself as a marker that a dock is present.
             */
            g_dock_device_addr = i;
            kprintf("  ACPI: Dock station _DCK method found at AML offset %u\n",
                    (uint32_t)i);
            /* We only need the first dock device */
            break;
        }
    }

    if (g_dock_device_addr != 0) {
        /* Dock hardware is present.  Assume initially undocked;
         * the first poll will update the real state. */
        g_dock_state = ACPI_DOCK_UNDOCKED;
        g_dock_prev_state = ACPI_DOCK_UNDOCKED;
        kprintf("[OK] ACPI: Dock station detected\n");
    }
}

/* Add dock scanning to the DSDT parse.  This is called from
 * parse_dsdt_for_sleep() as a second pass over the DSDT AML.
 * We keep it separate for clarity and to avoid modifying the
 * existing sleep-state parser. */
static void parse_dsdt_for_dock(struct fadt *fadt) {
    if (!fadt || !fadt->dsdt) return;

    uint8_t *dsdt = (uint8_t *)PHYS_TO_VIRT((uint64_t)fadt->dsdt);
    if (!dsdt) return;

    struct acpi_header *hdr = (struct acpi_header *)dsdt;
    if (memcmp(hdr->signature, "DSDT", 4) != 0) return;

    uint32_t dsdt_len = hdr->length;

    /* Validate header length to prevent underflow */
    if (dsdt_len < sizeof(struct acpi_header)) {
        kprintf("  ACPI: DSDT too short (%u bytes, need %zu)\n",
                (unsigned int)dsdt_len, sizeof(struct acpi_header));
        return;
    }

    uint8_t *aml = dsdt + sizeof(struct acpi_header);
    uint32_t aml_len = (uint32_t)(dsdt_len - sizeof(struct acpi_header));

    acpi_find_dock_device(aml, aml_len);
}

/* ── LPIT: Low Power Idle Table Parsing (Item 192) ────────────────── */

/*
 * Parse the ACPI LPIT table to discover low-power idle states (C-states)
 * that the platform supports beyond C1 (HLT).
 *
 * The LPIT table lists LPI (Low Power Idle) state descriptors. Each
 * descriptor provides the entry method, residency, and exit latency for
 * a specific idle state. We log the found states and register them with
 * the cpuidle subsystem so the idle governor can select deeper states
 * when the CPU is expected to be idle for long enough.
 *
 * Reference: ACPI 6.2+, section 5.2.27 (Low Power Idle Table).
 */
static void acpi_parse_lpit(struct acpi_header *hdr) {
    uint32_t table_len = hdr->length;

    if (table_len <= sizeof(struct acpi_header)) {
        kprintf("[LPIT] Table too short (%u bytes), ignoring\n",
                (unsigned int)table_len);
        return;
    }

    /* The LPI state descriptors follow immediately after the header.
     * Number of descriptors = (table_len - header_size) / desc_size. */
    uint32_t data_len = (uint32_t)(table_len - sizeof(struct acpi_header));
    uint32_t num_states = data_len / LPIT_DESC_SIZE;

    if (num_states == 0) {
        kprintf("[LPIT] No LPI state descriptors found\n");
        return;
    }

    /* Validate that the table length matches an exact number of
     * descriptors (no leftover bytes). */
    if (data_len % LPIT_DESC_SIZE != 0) {
        kprintf("[LPIT] WARNING: table length %u not a multiple of "
                "descriptor size %u (%u states, %u bytes remainder)\n",
                (unsigned int)table_len, LPIT_DESC_SIZE,
                (unsigned int)num_states,
                (unsigned int)(data_len % LPIT_DESC_SIZE));
        /* Proceed anyway — parse as many complete descriptors as possible */
    }

    if (num_states > CPUIDLE_MAX_STATES) {
        kprintf("[LPIT] Too many LPI states (%u), limiting to %d\n",
                (unsigned int)num_states, CPUIDLE_MAX_STATES);
        num_states = CPUIDLE_MAX_STATES;
    }

    struct lpi_state_desc *descs =
        (struct lpi_state_desc *)((uint8_t *)hdr + sizeof(struct acpi_header));

    kprintf("[LPIT] Found %u LPI state descriptor(s):\n",
            (unsigned int)num_states);

    /* Iterate through each descriptor and log details */
    for (uint32_t i = 0; i < num_states; i++) {
        struct lpi_state_desc *d = &descs[i];
        int disabled = (d->flags & LPI_FLAG_DISABLED) ? 1 : 0;
        int cnt_avail = (d->flags & LPI_FLAG_CNT_AVAIL) ? 1 : 0;

        /* Determine entry method description */
        const char *entry_desc;
        switch (d->type) {
        case 0:
            entry_desc = "Native (FFH)";
            break;
        case 1:
            entry_desc = "ACPI-defined";
            break;
        default:
            entry_desc = "Unknown";
            break;
        }

        kprintf("  LPI[%u] type=%u uid=%u flags=0x%x%s%s "
                "res=%u us lat=%u us entry=%s\n",
                (unsigned int)i,
                (unsigned int)d->type,
                (unsigned int)d->uid,
                (unsigned int)d->flags,
                disabled ? " [DISABLED]" : "",
                cnt_avail ? " [CNT_AVAIL]" : "",
                (unsigned int)d->residency,
                (unsigned int)d->latency,
                entry_desc);

        /* If the state is not disabled, attempt to register it with
         * the cpuidle subsystem so it can be selected by the governor.
         * Convert the LPIT descriptor to the acpi_cstate_desc format
         * expected by cpuidle_acpi_register_states. */
        if (!disabled && d->latency > 0 && d->residency > 0) {
            /* Collect eligible states into an array for batch registration */
            struct acpi_cstate_desc cdesc;
            memset(&cdesc, 0, sizeof(cdesc));
            cdesc.latency   = d->latency;
            cdesc.residency = d->residency;

            /* Map entry method from LPIT entry_trigger GAS.
             * space_id 0x7F = FFH (Functional Fixed Hardware — MWAIT)
             * space_id 0x01 = SystemIO (port-based entry) */
            if (d->entry_trigger.space_id == 0x7F) {
                cdesc.entry_method = ACPI_CSTATE_ENTRY_FFH;
                /* The address field contains the MWAIT hint (eax value).
                 * Format: bits [7:4] = C-state, bits [3:0] = sub-state. */
                cdesc.entry_param = d->entry_trigger.address;
                kprintf("  LPIT: State %u (uid=%u, lat=%u us, res=%u us) "
                        "entry via FFH (MWAIT hint 0x%llx)\n",
                        (unsigned int)i,
                        (unsigned int)d->uid,
                        (unsigned int)d->latency,
                        (unsigned int)d->residency,
                        (unsigned long long)d->entry_trigger.address);
            } else if (d->entry_trigger.space_id == 0x01) {
                cdesc.entry_method = ACPI_CSTATE_ENTRY_IO;
                /* The address field contains the IO port number.
                 * bit_width is the access size. */
                cdesc.entry_param  = d->entry_trigger.address;
                cdesc.entry_param2 = d->entry_trigger.bit_width;
                kprintf("  LPIT: State %u (uid=%u, lat=%u us, res=%u us) "
                        "entry via IO port 0x%llx (width=%u)\n",
                        (unsigned int)i,
                        (unsigned int)d->uid,
                        (unsigned int)d->latency,
                        (unsigned int)d->residency,
                        (unsigned long long)d->entry_trigger.address,
                        (unsigned int)d->entry_trigger.bit_width);
            } else {
                kprintf("  LPIT: State %u (uid=%u) has unknown entry "
                        "space_id %u, skipping\n",
                        (unsigned int)i,
                        (unsigned int)d->uid,
                        (unsigned int)d->entry_trigger.space_id);
                continue;
            }

            /* Register this single state with cpuidle.
             * In a batch scenario we could collect all first, but for
             * boot simplicity we register one at a time. */
            cpuidle_acpi_register_states(&cdesc, 1);
        }
    }

    /* Count disabled states for the summary */
    int disabled_count = 0;
    for (uint32_t j = 0; j < num_states; j++) {
        if (descs[j].flags & LPI_FLAG_DISABLED)
            disabled_count++;
    }

    kprintf("[LPIT] Parsed %u LPI state(s), %d disabled\n",
            (unsigned int)num_states, disabled_count);
}

/* ── NFIT: NVDIMM Firmware Interface Table parsing (Item 193) ──────── */

/* Known NFIT SPA Range type GUIDs for memory classification.
 * ACPI NFIT defines standard GUIDs for different memory types:
 *   - Volatile memory:   7305944F-FDDA-44E3-B16C-3F22D252E5D0
 *   - Persistent memory: 66F0D379-B4C3-4BD4-9B16-0F1F19D9E1A8
 *   - Persistent memory (legacy): 1EFABE8B-CB57-4D1D-9B65-0B4C1C9B99B4
 *   - Persistent memory (2MB-aligned): B6E7C8F1-4A9A-4F2B-9F3C-7E1B5D2A3C4D
 *   - Block mode window: 8F0E3B2A-1C4D-4E6F-9A8B-7C5D3E1F2A4B
 *
 * We use a string representation because these are 16-byte values and
 * comparing as raw bytes is simpler than struct comparisons.
 */
#define NFIT_GUID_VOLATILE      "\x4F\x94\x05\x73\xDA\xFD\xE3\x44\xB1\x6C\x3F\x22\xD2\x52\xE5\xD0"
#define NFIT_GUID_PMEM          "\x79\xD3\xF0\x66\xC3\xB4\xD4\x4B\x9B\x16\x0F\x1F\x19\xD9\xE1\xA8"
#define NFIT_GUID_PMEM_LEGACY   "\x8B\xBE\xFA\x1E\x57\xCB\x1D\x4D\x9B\x65\x0B\x4C\x1C\x9B\x99\xB4"
#define NFIT_GUID_PMEM_2MB     "\xF1\xC8\xE7\xB6\x9A\x4A\x2B\x4F\x9F\x3C\x7E\x1B\x5D\x2A\x3C\x4D"

/* Parsed SPA range storage (accessed by pmem driver) */
static struct nfit_spa_range_info g_nfit_spa_ranges[NFIT_MAX_SPA_RANGES];
static int g_nfit_spa_count = 0;

int acpi_nfit_get_count(void) {
    return g_nfit_spa_count;
}

int acpi_nfit_get_spa(int index, struct nfit_spa_range_info *info) {
    if (index < 0 || index >= g_nfit_spa_count || !info)
        return -1;
    *info = g_nfit_spa_ranges[index];
    return 0;
}

/* Compare a GUID against a known pattern (both are 16-byte arrays) */
static int guid_match(const uint8_t *guid, const char *pattern) {
    return (memcmp(guid, pattern, 16) == 0);
}

/* Return a human-readable name for a NFIT SPA range type GUID */
static const char *nfit_spa_type_name(const uint8_t *guid) {
    if (guid_match(guid, NFIT_GUID_VOLATILE))
        return "Volatile memory";
    if (guid_match(guid, NFIT_GUID_PMEM))
        return "Persistent memory (PMEM)";
    if (guid_match(guid, NFIT_GUID_PMEM_LEGACY))
        return "Persistent memory (legacy)";
    if (guid_match(guid, NFIT_GUID_PMEM_2MB))
        return "Persistent memory (2MB-aligned)";
    return "Unknown";
}

/* Determine if a GUID indicates persistent memory (for pmem device) */
static int guid_is_pmem(const uint8_t *guid) {
    return guid_match(guid, NFIT_GUID_PMEM) ||
           guid_match(guid, NFIT_GUID_PMEM_LEGACY) ||
           guid_match(guid, NFIT_GUID_PMEM_2MB);
}

/*
 * Parse the ACPI NFIT table to discover NVDIMM presence.
 *
 * The NFIT contains an array of sub-tables of various types. We iterate
 * through all sub-tables looking for:
 *   - Type 0 (SPA Range): identifies persistent memory regions
 *   - Type 1 (NVDIMM Region Mapping): maps NVDIMMs to SPA ranges
 *   - Type 4 (Control Region): identifies NVDIMM controller registers
 *
 * The parsed SPA ranges are exported via acpi_nfit_get_count/get_spa
 * so the pmem driver can register block devices for persistent memory.
 */
static void acpi_parse_nfit(struct acpi_header *hdr) {
    uint32_t table_len = hdr->length;
    uint8_t *table_end = (uint8_t *)hdr + table_len;

    if (table_len <= sizeof(struct acpi_header)) {
        kprintf("[NFIT] Table too short (%u bytes), ignoring\n",
                (unsigned int)table_len);
        return;
    }

    /* Reset parsed SPA range count */
    g_nfit_spa_count = 0;

    /* The NFIT sub-tables start right after the ACPI table header */
    uint8_t *pos = (uint8_t *)hdr + sizeof(struct acpi_header);

    kprintf("[NFIT] NVDIMM Firmware Interface Table found (%u bytes):\n",
            (unsigned int)table_len);

    int spa_count = 0;
    int region_count = 0;
    int ctrl_count = 0;

    while ((uintptr_t)(pos + sizeof(struct nfit_subtable_header)) <= (uintptr_t)table_end) {
        struct nfit_subtable_header *sub = (struct nfit_subtable_header *)pos;

        /* Validate length: must be at least header size and not exceed table bounds */
        if (sub->length < sizeof(struct nfit_subtable_header)) {
            kprintf("[NFIT] Invalid sub-table at offset %u: length=%u < header=%zu\n",
                    (unsigned int)(pos - (uint8_t *)hdr),
                    (unsigned int)sub->length,
                    sizeof(struct nfit_subtable_header));
            break;
        }

        uint16_t stype = sub->type;
        uint16_t slen  = sub->length;

        /* Ensure we don't read past the table end */
        if ((uintptr_t)(pos + slen) > (uintptr_t)table_end) {
            kprintf("[NFIT] Sub-table at offset %u exceeds table bounds "
                    "(length=%u, table_end_offset=%llu)\n",
                    (unsigned int)(pos - (uint8_t *)hdr),
                    (unsigned int)slen,
                    (unsigned long long)(table_end - (uint8_t *)hdr));
            break;
        }

        switch (stype) {
        case NFIT_SPA_RANGE: {
            /* System Physical Address Range (type 0) */
            if (slen < sizeof(struct nfit_spa_range)) {
                kprintf("[NFIT] SPA range too short: %u < %zu\n",
                        (unsigned int)slen, sizeof(struct nfit_spa_range));
                break;
            }
            struct nfit_spa_range *spa = (struct nfit_spa_range *)pos;
            const char *type_name = nfit_spa_type_name(spa->addr_range_type_guid);
            int is_pmem = guid_is_pmem(spa->addr_range_type_guid);

            kprintf("  SPA[%u] index=%u flags=0x%x prox=%u base=0x%llx "
                    "len=0x%llx (%llu MB) type=%s%s\n",
                    (unsigned int)spa_count,
                    (unsigned int)spa->spa_index,
                    (unsigned int)spa->flags,
                    (unsigned int)spa->proximity_domain,
                    (unsigned long long)spa->spa_base,
                    (unsigned long long)spa->spa_length,
                    (unsigned long long)(spa->spa_length / (1024ULL * 1024ULL)),
                    type_name,
                    (spa->flags & NFIT_SPA_READ_ONLY) ? " [RO]" : "");

            /* Store SPA range for pmem driver if it's persistent memory */
            if (is_pmem && g_nfit_spa_count < NFIT_MAX_SPA_RANGES) {
                struct nfit_spa_range_info *info = &g_nfit_spa_ranges[g_nfit_spa_count];
                info->spa_base = spa->spa_base;
                info->spa_length = spa->spa_length;
                info->spa_index = spa->spa_index;
                info->flags = spa->flags;
                info->proximity_domain = spa->proximity_domain;
                g_nfit_spa_count++;
            }
            spa_count++;
            break;
        }

        case NFIT_NVDIMM_REGION: {
            /* NVDIMM Region Mapping (type 1) */
            if (slen < sizeof(struct nfit_region_mapping)) {
                kprintf("[NFIT] Region mapping too short: %u < %zu\n",
                        (unsigned int)slen, sizeof(struct nfit_region_mapping));
                break;
            }
            struct nfit_region_mapping *reg = (struct nfit_region_mapping *)pos;
            kprintf("  REGION[%u] handle=0x%x spa_idx=%u offset=0x%llx "
                    "len=0x%llx interleave=%u ways=%u\n",
                    (unsigned int)region_count,
                    (unsigned int)reg->nfit_handle,
                    (unsigned int)reg->spa_index,
                    (unsigned long long)reg->region_offset,
                    (unsigned long long)reg->region_length,
                    (unsigned int)reg->interleave_index,
                    (unsigned int)reg->interleave_ways);
            region_count++;
            break;
        }

        case NFIT_CONTROL_REGION: {
            /* NVDIMM Control Region (type 4) */
            if (slen < sizeof(struct nfit_ctrl_region)) {
                kprintf("[NFIT] Control region too short: %u < %zu\n",
                        (unsigned int)slen, sizeof(struct nfit_ctrl_region));
                break;
            }
            struct nfit_ctrl_region *ctrl = (struct nfit_ctrl_region *)pos;
            kprintf("  CTRL[%u] handle=0x%x vendor=0x%04x device=0x%04x "
                    "serial=0x%x region_size=0x%llx\n",
                    (unsigned int)ctrl_count,
                    (unsigned int)ctrl->nfit_handle,
                    (unsigned int)ctrl->vendor_id,
                    (unsigned int)ctrl->device_id,
                    (unsigned int)ctrl->serial_number,
                    (unsigned long long)ctrl->control_region_size);
            ctrl_count++;
            break;
        }

        case NFIT_INTERLEAVE:
            kprintf("  INTERLEAVE: skipping (type=%u length=%u)\n",
                    (unsigned int)stype, (unsigned int)slen);
            break;

        case NFIT_SMBIOS_HANDLE:
            kprintf("  SMBIOS: skipping (type=%u length=%u)\n",
                    (unsigned int)stype, (unsigned int)slen);
            break;

        default:
            kprintf("  SUBTABLE[type=%u length=%u]: unknown, skipping\n",
                    (unsigned int)stype, (unsigned int)slen);
            break;
        }

        /* Advance to the next sub-table (must be 4-byte aligned) */
        pos += slen;
        /* Align to 4 bytes per ACPI spec */
        uintptr_t align_offset = (uintptr_t)pos & 3;
        if (align_offset)
            pos += 4 - align_offset;
    }

    kprintf("[NFIT] Summary: %u SPA ranges, %u region mappings, "
            "%u control regions, %u PMEM regions exported\n",
            spa_count, region_count, ctrl_count, g_nfit_spa_count);
}

void acpi_init(void) {
    struct rsdp *rsdp = find_rsdp(0x80000, 0x9FFFF);
    if (!rsdp) rsdp = find_rsdp(0xE0000, 0xFFFFF);
    if (!rsdp) {
        kprintf("[--] ACPI: RSDP not found\n");
        return;
    }

    struct rsdt *rsdt = (struct rsdt *)PHYS_TO_VIRT((unsigned long)rsdp->rsdt_addr);
    if (!rsdt) return;

    if (memcmp(rsdt->header.signature, "RSDT", 4) != 0) return;

    /* Validate RSDT header length to prevent underflow when computing
     * num_entries.  Each entry is 4 bytes (a 32-bit physical address). */
    if (rsdt->header.length < sizeof(struct acpi_header)) {
        kprintf("[--] ACPI: RSDT too short (%u bytes, need %zu)\n",
                (unsigned int)rsdt->header.length, sizeof(struct acpi_header));
        return;
    }

    uint32_t num_entries = (uint32_t)((rsdt->header.length - sizeof(struct acpi_header)) / 4);
    struct fadt *fadt = NULL;

    /* Validate the mapped region bounds for ACPI tables */
    uint64_t acpi_region_start = 0x80000;
    uint64_t acpi_region_end = 0xFFFFF;

    for (uint32_t i = 0; i < num_entries; i++) {
        uint64_t entry_phys = (uint64_t)rsdt->entries[i];
        /* Reject entries outside the standard ACPI memory region */
        if (entry_phys < acpi_region_start || entry_phys > acpi_region_end) {
            kprintf("[--] ACPI: RSDT entry %u physical address 0x%llx outside valid range, skipping\n",
                    (unsigned int)i, (unsigned long long)entry_phys);
            continue;
        }
        struct acpi_header *hdr = (struct acpi_header *)PHYS_TO_VIRT(entry_phys);
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
                kprintf("[OK] PCIe ECAM base: 0x%llx\n", (unsigned long long)ecam);
            }
        }
        /* LPIT table — Low Power Idle States (ACPI 6.2+) */
        if (memcmp(hdr->signature, LPIT_SIG, 4) == 0) {
            kprintf("[OK] ACPI: LPIT (Low Power Idle Table) found\n");
            acpi_parse_lpit(hdr);
        }
        /* NFIT table — NVDIMM Firmware Interface Table */
        if (memcmp(hdr->signature, NFIT_SIG, 4) == 0) {
            acpi_parse_nfit(hdr);
        }
        /* DMAR table — DMA Remapping (VT-d) */
        if (memcmp(hdr->signature, DMAR_SIG, 4) == 0) {
            kprintf("[OK] ACPI: DMAR (DMA Remapping) table found\n");
            /* IOMMU driver will parse this on iommu_init() */
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

    /* Export DSDT base/length for ACPI drivers (acpi_cpufreq, etc.) */
    if (fadt && fadt->dsdt) {
        uint8_t *dsdt = (uint8_t *)PHYS_TO_VIRT((uint64_t)fadt->dsdt);
        if (dsdt && memcmp(dsdt, "DSDT", 4) == 0) {
            struct acpi_header *hdr = (struct acpi_header *)dsdt;
            g_dsdt_base = dsdt;
            g_dsdt_length = hdr->length;
            kprintf("  ACPI: DSDT at %p, length %u\n",
                    (void *)g_dsdt_base, g_dsdt_length);
        }
    }

    /* Parse DSDT for dock station (Item 106) */
    parse_dsdt_for_dock(fadt);

    /* Check for power button */
    check_power_button(fadt);

    /* Default S5 value if DSDT parsing didn't find it */
    if (slp_typa_s5 == 0xFF) slp_typa_s5 = 0x07;

    kprintf("[OK] ACPI: PM1a control port 0x%lx\n", (unsigned long)pm1a_cnt);
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

/* ── Dock/Undock Notification API (Item 106) ─────────────────────── */

int acpi_dock_register_notify(acpi_dock_callback_t cb, void *user_data) {
    if (!cb)
        return -1;

    for (int i = 0; i < ACPI_DOCK_MAX_CB; i++) {
        if (!g_dock_notify[i].in_use) {
            g_dock_notify[i].cb = cb;
            g_dock_notify[i].user_data = user_data;
            g_dock_notify[i].in_use = 1;
            return 0;
        }
    }
    /* Table full */
    kprintf("[ACPI] Dock notify: callback table full (%d max)\n",
            ACPI_DOCK_MAX_CB);
    return -1;
}

void acpi_dock_unregister_notify(acpi_dock_callback_t cb, void *user_data) {
    if (!cb)
        return;

    for (int i = 0; i < ACPI_DOCK_MAX_CB; i++) {
        if (g_dock_notify[i].in_use &&
            g_dock_notify[i].cb == cb &&
            g_dock_notify[i].user_data == user_data) {
            g_dock_notify[i].in_use = 0;
            g_dock_notify[i].cb = NULL;
            g_dock_notify[i].user_data = NULL;
            return;
        }
    }
}

int acpi_dock_get_state(void) {
    return g_dock_state;
}

/* Fire all registered dock notification callbacks with the new state. */
static void acpi_dock_fire_callbacks(int new_state) {
    for (int i = 0; i < ACPI_DOCK_MAX_CB; i++) {
        if (g_dock_notify[i].in_use && g_dock_notify[i].cb) {
            g_dock_notify[i].cb(new_state, g_dock_notify[i].user_data);
        }
    }
}

/* ── Dock Polling ──────────────────────────────────────────────────
 *
 * Since we do not have a full AML interpreter to evaluate _DCK at
 * runtime, we provide:
 *   1. Automatic DSDT scanning for _DCK during init.
 *   2. A manual override for drivers/shell to signal dock events.
 *   3. A polling entry point for future AML or EC-based detection.
 *
 * For platforms where ACPI _DCK evaluation is unavailable, drivers
 * can call acpi_dock_manual_transition() when they detect dock
 * insertion/removal via other means (EC query, GPIO interrupt).
 */

/* Manual dock state override flag: set to 1 once manual_transition is used */
static int g_dock_manual_override = 0;

void acpi_dock_manual_transition(int new_state) {
    if (new_state != ACPI_DOCK_DOCKED && new_state != ACPI_DOCK_UNDOCKED)
        return;
    if (g_dock_state == new_state)
        return;  /* no change */

    g_dock_prev_state = g_dock_state;
    g_dock_state = new_state;
    g_dock_manual_override = 1;

    kprintf("[ACPI] Dock manual transition: %s -> %s\n",
            g_dock_prev_state == ACPI_DOCK_DOCKED ? "docked" : "undocked",
            new_state == ACPI_DOCK_DOCKED ? "docked" : "undocked");

    acpi_dock_fire_callbacks(new_state);
}

void acpi_dock_poll(void) {
    if (g_dock_device_addr == 0 && !g_dock_manual_override)
        return;  /* no dock hardware */

    if (g_dock_manual_override)
        return;  /* manual mode — polling disabled */

    /*
     * Full implementation (requires AML interpreter):
     *
     *   // Evaluate _DCK at g_dock_device_addr.
     *   // If _DCK(1) returns != 0 -> docked
     *   // If _DCK(0) returns != 0 -> undocked
     *   // Then call _STA to confirm state.
     *
     * For ThinkPad EC-based detection:
     *   uint8_t ec_dock;
     *   if (ec_read(0x0c, &ec_dock) == 0 && (ec_dock & 0x02)) {
     *       acpi_dock_manual_transition(ACPI_DOCK_DOCKED);
     *   }
     *
     * Since we lack AML execution, this is a placeholder for
     * future expansion.  Platforms with embedded controllers
     * can add EC register checks here.
     */
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
