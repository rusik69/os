#ifndef ACPI_H
#define ACPI_H

#include "types.h"

/* Forward declaration for AML object (defined in aml_exec.h) */
struct aml_object;

/* ACPI table signatures */
#define RSDP_SIG "RSD PTR "
#define FADT_SIG "FACP"
#define LPIT_SIG "LPIT"

void acpi_init(void);
void acpi_shutdown(void);
void acpi_reboot(void);
int  acpi_find_reset_register(void);

/* Power button */
int  acpi_power_button_read(void);  /* returns 1 if pressed, clears flag */

/* ACPI SCI (System Control Interrupt) IRQ number, read from FADT.
   Returns 0 if ACPI is not yet initialized or FADT not found. */
uint16_t acpi_get_sci_irq(void);

/* ACPI table lookup — returns PHYS_TO_VIRT-mapped pointer or NULL */
void *acpi_get_table(const char *sig);

/* Sleep states */
#define ACPI_S0  0
#define ACPI_S1  1
#define ACPI_S2  2
#define ACPI_S3  3  /* Suspend-to-RAM */
#define ACPI_S4  4
#define ACPI_S5  5

/* Request a sleep state transition.
   Returns 0 if the sleep request was accepted, -1 if unsupported. */
int  acpi_sleep(uint32_t state);

/* Check if a given sleep state is supported. */
int  acpi_sleep_supported(uint32_t state);

/* ── Dock/Undock Notification (Item 106) ─────────────────────────── */

/* Dock station state flags */
#define ACPI_DOCK_NOT_PRESENT  0  /* no dock hardware found */
#define ACPI_DOCK_UNDOCKED     1  /* dock present but undocked */
#define ACPI_DOCK_DOCKED       2  /* device docked */

/* Maximum number of dock notification callbacks */
#define ACPI_DOCK_MAX_CB       8

/* Callback type for dock events: called with dock_state (ACPI_DOCK_DOCKED
 * or ACPI_DOCK_UNDOCKED) and an opaque user pointer. */
typedef void (*acpi_dock_callback_t)(int dock_state, void *user_data);

/* Register a callback for dock state change notifications.
 * Returns 0 on success, -1 if the callback table is full. */
int acpi_dock_register_notify(acpi_dock_callback_t cb, void *user_data);

/* Unregister a previously registered dock notification callback. */
void acpi_dock_unregister_notify(acpi_dock_callback_t cb, void *user_data);

/* Query the current dock station state.
 * Returns ACPI_DOCK_NOT_PRESENT, ACPI_DOCK_UNDOCKED, or ACPI_DOCK_DOCKED. */
int acpi_dock_get_state(void);

/* Poll the dock hardware for state changes.
 * Called periodically (e.g., from the idle loop or a timer) to detect
 * hotplug events when no ACPI notification mechanism is available. */
void acpi_dock_poll(void);

/* ── DSDT / SSDT global information (exported for ACPI drivers) ──── */

/* Virtual address of DSDT base (mapped via PHYS_TO_VIRT).
 * Set during acpi_init() after the FADT is parsed. */
extern uint8_t *g_dsdt_base;
/* Total length of the DSDT table (including ACPI header) in bytes. */
extern uint32_t g_dsdt_length;

/* Pointer to the AML bytecode region within the DSDT (after the header). */
extern uint8_t *g_dsdt_aml_base;
/* Length of the AML bytecode (DSDT length minus header). */
extern uint32_t g_dsdt_aml_length;

/* Maximum number of SSDT tables we can track. */
#define ACPI_MAX_SSDT  16

/* Per-SSDT table information. */
struct acpi_ssdt_info {
    uint8_t  *base;      /* Virtual address of the SSDT table header */
    uint32_t  length;    /* Total length including header */
    uint8_t  *aml_base;  /* Pointer to AML bytecode (after header) */
    uint32_t  aml_length;/* Length of AML bytecode */
};

/* Number of SSDT tables found and loaded. */
extern int g_acpi_ssdt_count;
/* Array of SSDT table information. */
extern struct acpi_ssdt_info g_acpi_ssdt_tables[ACPI_MAX_SSDT];

/* Compute total AML size across DSDT + all SSDTs (for AML interpreter). */
uint32_t acpi_get_total_aml_size(void);

/* ── ACPI table header (common to all ACPI tables) ────────────────── */

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

/* RSDT / RSDP root system description pointers */
struct rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct rsdt {
    struct acpi_header header;
    uint32_t entries[1];  /* variable-length */
} __attribute__((packed));

/* ── NFIT: NVDIMM Firmware Interface Table (Item 193) ─────────────── */

/* NFIT table signature */
#define NFIT_SIG "NFIT"

/* NFIT sub-table type values */
#define NFIT_SPA_RANGE       0  /* System Physical Address Range */
#define NFIT_NVDIMM_REGION   1  /* NVDIMM Region Mapping */
#define NFIT_INTERLEAVE      2  /* Interleave Structure */
#define NFIT_SMBIOS_HANDLE   3  /* SMBIOS Management Information */
#define NFIT_CONTROL_REGION  4  /* NVDIMM Control Region */
#define NFIT_DATA_ADDR       5  /* NVDIMM Block Data Window Region */
#define NFIT_FLUSH_HINT      6  /* Flush Hint Address Structure */
#define NFIT_CAPABILITIES    7  /* NVDIMM Capabilities Structure */
#define NFIT_PHYS_DEVICE     8  /* NVDIMM Physical Device ID */

/* NFIT System Physical Address Range flags */
#define NFIT_SPA_READ_ONLY   (1ULL << 0)
#define NFIT_SPA_ONLINE      (1ULL << 1)

/* NFIT System Physical Address Range type GUIDs (16 bytes each) */
/* Volatile memory range */
#define NFIT_SPA_TYPE_VOLATILE   0x01234567  /* placeholder — real GUIDs are 16 bytes */

/* NFIT sub-table header (all sub-tables share this) */
struct nfit_subtable_header {
    uint16_t type;
    uint16_t length;
} __attribute__((packed));

/* NFIT System Physical Address Range structure (type 0) */
struct nfit_spa_range {
    struct nfit_subtable_header hdr;
    uint16_t     spa_index;
    uint16_t     flags;
    uint32_t     reserved;
    uint32_t     proximity_domain;
    uint8_t      addr_range_type_guid[16];
    uint64_t     spa_base;
    uint64_t     spa_length;
    uint64_t     memory_mapping_offset;
    uint64_t     spa_range_attribute;
} __attribute__((packed));

/* NFIT NVDIMM Region Mapping structure (type 1) */
struct nfit_region_mapping {
    struct nfit_subtable_header hdr;
    uint32_t     nfit_handle;
    uint16_t     nvdimm_phys_id;
    uint16_t     region_id;
    uint16_t     spa_index;
    uint16_t     reserved;
    uint64_t     region_offset;
    uint64_t     region_length;
    uint64_t     region_blk_addr_offset;
    uint64_t     region_blk_data_len;
    uint8_t      interleave_index;
    uint8_t      interleave_ways;
    uint16_t     reserved2;
    uint32_t     flags;
} __attribute__((packed));

/* NFIT Control Region structure (type 4) */
struct nfit_ctrl_region {
    struct nfit_subtable_header hdr;
    uint32_t     nfit_handle;
    uint16_t     vendor_id;
    uint16_t     device_id;
    uint16_t     revision_id;
    uint16_t     subsystem_vendor_id;
    uint16_t     subsystem_device_id;
    uint16_t     subsystem_revision_id;
    uint8_t      reserved[6];
    uint32_t     serial_number;
    uint16_t     region_format_interface_code;
    uint16_t     num_control_regions;
    uint64_t     control_region_size;
    uint64_t     control_region_offset;
    uint8_t      control_region_bus_addr[8];
    uint8_t      region_format_interface_code2;
    uint8_t      reserved2[3];
    uint64_t     data_size;
    uint64_t     data_offset;
} __attribute__((packed));

/* ── Exported NFIT info for pmem driver ──────────────────────────── */

/* Maximum number of SPA ranges we can track */
#define NFIT_MAX_SPA_RANGES 8

/* Parsed SPA range summary */
struct nfit_spa_range_info {
    uint64_t  spa_base;
    uint64_t  spa_length;
    uint16_t  spa_index;
    uint16_t  flags;
    uint32_t  proximity_domain;
};

/* Returns number of SPA ranges found (0 if no NFIT). */
int acpi_nfit_get_count(void);
/* Copy a specific SPA range entry. Returns 0 on success. */
int acpi_nfit_get_spa(int index, struct nfit_spa_range_info *info);

/* ── DMAR: DMA Remapping Reporting (VT-d) ─────────────────────────── */

#define DMAR_SIG "DMAR"

/* DMAR table header (extends acpi_header with flags) */
struct dmar_table {
    struct acpi_header header;
    uint8_t  host_addr_width;  /* DMA address width, minus 1 (e.g. 47 = 48-bit) */
    uint8_t  flags;
    uint8_t  reserved[10];
} __attribute__((packed));

/* DMAR sub-table types */
#define DMAR_TYPE_DRHD   0  /* DMA Remapping Hardware Unit Definition */
#define DMAR_TYPE_RMRR   1  /* Reserved Memory Region Reporting */
#define DMAR_TYPE_ATSR   2  /* Root Port ATS Capability */
#define DMAR_TYPE_RHSA   3  /* Remapping Hardware Status Affinity */

/* DMAR sub-table header */
struct dmar_sub_header {
    uint16_t type;
    uint16_t length;
} __attribute__((packed));

/* DRHD — DMA Remapping Hardware Unit Definition */
#define DMAR_DRHD_FLAG_INCLUDE_PCI_ALL (1U << 0)

struct dmar_drhd {
    struct dmar_sub_header hdr;
    uint8_t  flags;
    uint8_t  reserved;
    uint16_t segment;
    uint64_t base_addr;  /* Register base of remapping hardware */
    /* Followed by variable-length array of device scope entries */
} __attribute__((packed));

/* RMRR — Reserved Memory Region Reporting */
struct dmar_rmrr {
    struct dmar_sub_header hdr;
    uint16_t segment;
    uint8_t  reserved[2];
    uint64_t base_addr;   /* Region base (must be 4K aligned) */
    uint64_t end_addr;    /* Region end (inclusive) */
    /* Followed by variable-length array of device scope entries */
} __attribute__((packed));

/* Device scope entry structure */
struct dmar_device_scope {
    uint8_t  type;
    uint8_t  length;
    uint16_t reserved;
    uint8_t  enumeration_id;
    uint8_t  start_bus_number;
    /* Followed by variable-length array of path entries (dev, func pairs) */
} __attribute__((packed));

/* ── AML Namespace Structures ───────────────────────────────────── */

/* AML namespace node types */
#define AML_NS_ROOT          0
#define AML_NS_SCOPE         1
#define AML_NS_DEVICE        2
#define AML_NS_PROCESSOR     3
#define AML_NS_POWERRESOURCE 4
#define AML_NS_THERMAL_ZONE  5
#define AML_NS_NAME          6
#define AML_NS_METHOD        7

/* Maximum number of namespace nodes */
#define ACPI_NS_MAX_NODES    2048

/* AML namespace node — each represents a named object in the ACPI namespace */
struct aml_ns_node {
	char     name[4];            /* 4-byte NameSeg (e.g. "_SB_", "PCI0") */
	uint8_t  type;                /* AML_NS_* type */
	uint16_t parent;              /* Index of parent (0xFFFF = root) */
	uint16_t first_child;         /* Index of first child (0xFFFF = none) */
	uint16_t next_sibling;        /* Index of next sibling (0xFFFF = none) */
	uint8_t  *aml_start;          /* Pointer to start of AML for this node */
	uint32_t aml_length;          /* Length of AML bytecode for this node */
	uint8_t  from_ssdt;           /* Source SSDT index (0 = DSDT) */
	struct aml_object *value;     /* Evaluated value (set during method exec) */
};

/* ── AML Namespace Construction API ─────────────────────────────── */

/* Build the ACPI namespace by walking DSDT + SSDT AML bytecode.
 * Called after acpi_init() has loaded DSDT/SSDT tables.
 * Returns 0 on success, negative on error. */
int aml_build_namespace(void);

/* Dump the AML namespace tree to the kernel log (debugging).
 * Logs each node with its type, name, parent, and AML range. */
void aml_dump_namespace(void);

/* Look up a namespace node by its full path (e.g. "\\_SB_.PCI0").
 * Returns pointer to the node, or NULL if not found.
 * Path format: absolute paths start with '\\', segments separated by '.'. */
struct aml_ns_node *aml_ns_lookup(const char *path);

/* Get the number of namespace nodes. */
int aml_ns_get_count(void);

/* Get a namespace node by index. Returns NULL if index out of range. */
struct aml_ns_node *aml_ns_get_node(int index);

#endif
