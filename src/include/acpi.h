#ifndef ACPI_H
#define ACPI_H

#include "types.h"

void acpi_init(void);
void acpi_shutdown(void);
void acpi_reboot(void);
int  acpi_find_reset_register(void);

/* Power button */
int  acpi_power_button_read(void);  /* returns 1 if pressed, clears flag */

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

/* ── DSDT global information (exported for ACPI drivers) ──────────── */

/* Virtual address of DSDT base (mapped via PHYS_TO_VIRT).
 * Set during acpi_init() after the FADT is parsed. */
extern uint8_t *g_dsdt_base;
/* Total length of the DSDT table (including ACPI header) in bytes. */
extern uint32_t g_dsdt_length;

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

#endif
