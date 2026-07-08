#ifndef PCI_H
#define PCI_H

#include "types.h"
#include "idt.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass;
    uint8_t  irq;
    uint32_t bar[6];
};

uint32_t pci_read(int bus, int slot, int func, int offset);
void pci_write(int bus, int slot, int func, int offset, uint32_t val);
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out);
int pci_find_class(uint8_t cls, uint8_t sub, struct pci_device *out);
void pci_enable_bus_master(struct pci_device *dev);
void pci_list(void);
void pci_init(void);

/* PCIe ECAM (Memory-mapped config space) */
void pcie_ecam_set_base(uint64_t base);   /* called from ACPI MCFG parser */
uint32_t pcie_read(int bus, int slot, int func, int offset);
void pcie_write(int bus, int slot, int func, int offset, uint32_t val);
int pcie_is_available(void);

/* PCI Express capability detection */
int pci_find_pcie_cap(int bus, int slot, int func, uint8_t *cap_offset);
int pcie_is_present(void);

/* PCI Express device type */
#define PCIE_DEV_TYPE_ENDPOINT    0
#define PCIE_DEV_TYPE_ROOT_PORT   1
#define PCIE_DEV_TYPE_UPSTREAM    2
#define PCIE_DEV_TYPE_DOWNSTREAM  3
#define PCIE_DEV_TYPE_SWITCH      4
#define PCIE_DEV_TYPE_UNKNOWN     0xFF

int pcie_device_type(int bus, int slot, int func);

/* ── VPD (Vital Product Data) ───────────────────────────────────── */

/* VPD capability ID = 0x03 */
#define PCI_CAP_ID_VPD         0x03
#define PCI_VPD_ADDR           2   /* VPD address register (at cap+2) */
#define PCI_VPD_DATA           4   /* VPD data register  (at cap+4) */
#define PCI_VPD_ADDR_F         0x80000000U  /* Flag bit: 1 = write in progress */

/* VPD field tags (PCI 2.2+ specification) */
#define PCI_VPD_TAG_PN         0x81  /* Part number */
#define PCI_VPD_TAG_SN         0x82  /* Serial number */
#define PCI_VPD_TAG_MN         0x83  /* Manufacturer ID */
#define PCI_VPD_TAG_EC         0x84  /* Engineering change level */
#define PCI_VPD_TAG_MC         0x85  /* Manufacturing location */
#define PCI_VPD_TAG_YA         0x86  /* Asset tag */
#define PCI_VPD_TAG_NA         0x87  /* NIC MAC address list */
#define PCI_VPD_TAG_RV         0x90  /* Read-only VPD end tag */
#define PCI_VPD_TAG_CP         0x91  /* Read/write VPD end tag */
#define PCI_VPD_TAG_END        0x78  /* End tag (0x78) */

/* Find VPD capability on a device. Returns capability offset or -1. */
int pci_vpd_find_cap(struct pci_device *dev);

/* Check if a device has VPD capability. Returns 1 if capable, 0 otherwise. */
int pci_vpd_capable(struct pci_device *dev);

/* Read a 32-bit word from VPD at the given offset.
 * Returns 0 on success, -1 on error. */
int pci_vpd_read(struct pci_device *dev, uint32_t addr, uint32_t *val);

/* Write a 32-bit word to VPD at the given offset.
 * Returns 0 on success, -1 on error. */
int pci_vpd_write(struct pci_device *dev, uint32_t addr, uint32_t val);

/* Read a specific VPD field (e.g., part number, serial number).
 * field_tag: PCI_VPD_TAG_PN, PCI_VPD_TAG_SN, PCI_VPD_TAG_MN, etc.
 * buf:       Output buffer for field data.
 * len:       Size of output buffer.
 * Returns the number of bytes written to buf, or -1 on error/not found. */
int pci_vpd_read_field(struct pci_device *dev, uint8_t field_tag,
                        char *buf, size_t len);

/* ── MSI (Message Signalled Interrupts) ──────────────────────────── */

/* MSI capability ID = 0x05 */
/* Message Control register bits (at cap_offset + 2) */
#define PCI_MSI_CTRL_ENABLE    (1U << 0)
#define PCI_MSI_CTRL_MMC_SHIFT 1    /* Multiple Message Capable */
#define PCI_MSI_CTRL_MMC_MASK  (7 << 1)
#define PCI_MSI_CTRL_MME_SHIFT 4    /* Multiple Message Enable */
#define PCI_MSI_CTRL_MME_MASK  (7 << 4)
#define PCI_MSI_CTRL_64BIT     (1U << 7)  /* 64-bit addressing capable */
#define PCI_MSI_CTRL_PERVEC    (1U << 8)  /* Per-vector masking capable */

/* MSI delivery modes (used in data register) */
#define PCI_MSI_DM_FIXED       0x0
#define PCI_MSI_DM_LOWEST_PRI  0x1
#define PCI_MSI_DM_SMI         0x2
#define PCI_MSI_DM_NMI         0x4
#define PCI_MSI_DM_INIT        0x5
#define PCI_MSI_DM_EXTINT      0x7

/* MSI capability structure */
struct msi_info {
    uint16_t cap_offset;       /* MSI capability offset in config space */
    uint8_t  is_64bit;         /* 1 if 64-bit addressing supported */
    uint8_t  has_per_vector;   /* 1 if per-vector masking supported */
    uint16_t mmc;              /* Multiple Message Capable (log2 of count) */
};

/* Find and parse MSI capability. Returns 0 if found, -1 if not. */
int pci_find_msi_cap(uint8_t bus, uint8_t slot, uint8_t func,
                     struct msi_info *info);

/* Enable MSI interrupts for a device.
 * vector:      interrupt vector number
 * apic_id:     destination local APIC ID
 * nvecs:       number of vectors needed (rounded down to power of 2)
 * delivery:    delivery mode (PCI_MSI_DM_FIXED, etc.)
 * Returns the vector base on success, < 0 on error.
 */
int pci_enable_msi(struct pci_device *dev, uint8_t vector,
                   uint32_t apic_id, int nvecs, uint8_t delivery);

/* Disable MSI interrupts. */
void pci_disable_msi(struct pci_device *dev);

/* ── MSI-X (MSI with eXtended capabilities) ──────────────────────── */
#define PCI_MSIX_TBL_BIR       0x7   /* bits 0-2: BAR indicator */
#define PCI_MSIX_TBL_OFFSET    0xFFFFFFF8  /* bits 3-31: offset in BAR */

struct msix_info {
    uint16_t cap_offset;       /* MSI-X capability offset in config space */
    uint8_t  table_bir;        /* BAR index for MSI-X table */
    uint32_t table_offset;     /* offset within that BAR */
    uint32_t table_size;       /* number of MSI-X entries */
    uint8_t  pba_bir;          /* BAR index for PBA */
    uint32_t pba_offset;       /* offset within that BAR */
};

/* Find and parse MSI-X capability. Returns 0 if found, -1 if not. */
int pci_find_msix_cap(uint8_t bus, uint8_t slot, uint8_t func,
                      struct msix_info *info);

/* Enable MSI-X interrupts for a device.
 * info:        parsed MSI-X capability info
 * table_virt:  virtual address of the MSI-X table (must be mapped)
 * vectors:     array of vectors to program (one per table entry)
 * apic_ids:    array of destination APIC IDs (one per entry)
 * n:           number of entries to program (<= table_size)
 * Returns 0 on success, < 0 on error.
 */
int pci_enable_msix(struct pci_device *dev, struct msix_info *info,
                    volatile uint32_t *table_virt,
                    const uint8_t *vectors, const uint32_t *apic_ids,
                    int n);

/* Disable MSI-X interrupts. */
void pci_disable_msix(struct pci_device *dev);

/* ── High-level interrupt setup with fallback ────────────────────── */

/* Result of pci_setup_interrupts() */
struct pci_interrupt_config {
    int  type;                 /* 2 = MSI-X, 1 = MSI, 0 = INTx */
    int  vector;               /* base vector number */
    int  n_vectors;            /* number of vectors allocated */
};

/* Best-effort interrupt setup: tries MSI-X, then MSI, then INTx.
 * On success the handler at 'handler' will be registered for the vector(s).
 * Returns 0 on success, < 0 on total failure.
 */
int pci_setup_interrupts(struct pci_device *dev,
                         struct pci_interrupt_config *cfg,
                         isr_handler_t handler);

/* ── PCIe Advanced Error Reporting (AER) ───────────────────────── */

/* AER is a PCIe Extended Capability (ID = 0x0001) found in the
 * extended config space (offset >= 0x100).  The AER capability
 * allows reporting and logging of correctable and uncorrectable
 * PCIe errors.
 */

/* AER capability register offsets (relative to capability base) */
#define PCI_AER_UNCOR_STATUS      0x04  /* Uncorrectable Error Status */
#define PCI_AER_UNCOR_MASK        0x08  /* Uncorrectable Error Mask */
#define PCI_AER_UNCOR_SEVERITY    0x0C  /* Uncorrectable Error Severity */
#define PCI_AER_COR_STATUS        0x10  /* Correctable Error Status */
#define PCI_AER_COR_MASK          0x14  /* Correctable Error Mask */
#define PCI_AER_CAP_CONTROL       0x18  /* Advanced Error Cap & Control */
#define PCI_AER_HEADER_LOG        0x1C  /* Header Log (16 bytes) */
#define PCI_AER_ROOT_ERR_CMD      0x2C  /* Root Error Command (RC only) */
#define PCI_AER_ROOT_ERR_STATUS   0x30  /* Root Error Status (RC only) */
#define PCI_AER_COR_SRC_ID        0x34  /* Error Source ID (correctable) */
#define PCI_AER_UNCOR_SRC_ID      0x36  /* Error Source ID (uncorrectable) */

/* AER Uncorrectable Error Status bits */
#define PCI_AER_UNCOR_DL_PROTO     (1U << 4)  /* Data Link Protocol Error */
#define PCI_AER_UNCOR_SURPRISE_DN  (1U << 5)  /* Surprise Down */
#define PCI_AER_UNCOR_POISON_TLP   (1U << 12) /* Poisoned TLP */
#define PCI_AER_UNCOR_FC_PROTO     (1U << 13) /* Flow Control Protocol */
#define PCI_AER_UNCOR_COMP_TIME    (1U << 14) /* Completion Timeout */
#define PCI_AER_UNCOR_COMP_ABORT   (1U << 15) /* Completer Abort */
#define PCI_AER_UNCOR_UNEXP_CMP    (1U << 16) /* Unexpected Completion */
#define PCI_AER_UNCOR_RCV_OVFLOW   (1U << 17) /* Receiver Overflow */
#define PCI_AER_UNCOR_MALFORMED    (1U << 18) /* Malformed TLP */
#define PCI_AER_UNCOR_ECRC_ERR     (1U << 19) /* ECRC Error */
#define PCI_AER_UNCOR_UNSUP_REQ    (1U << 20) /* Unsupported Request */
#define PCI_AER_UNCOR_ACS_VIOL     (1U << 21) /* ACS Violation */
#define PCI_AER_UNCOR_INTERNAL     (1U << 22) /* Internal Error */
#define PCI_AER_UNCOR_ATOMIC_EGR   (1U << 23) /* AtomicOp Egress Blocked */
#define PCI_AER_UNCOR_TLP_PREFIX   (1U << 24) /* TLP Prefix Blocked */
#define PCI_AER_UNCOR_POISON_SKIP  (1U << 25) /* Poisoned TLP (skip) */

/* AER Correctable Error Status bits */
#define PCI_AER_COR_RCV_ERR        (1U << 0)  /* Receiver Error */
#define PCI_AER_COR_BAD_TLP        (1U << 6)  /* Bad TLP */
#define PCI_AER_COR_BAD_DLLP       (1U << 7)  /* Bad DLLP */
#define PCI_AER_COR_REPLAY_ROLL    (1U << 8)  /* REPLAY_NUM Rollover */
#define PCI_AER_COR_REPLAY_TIMEOUT (1U << 12) /* Replay Timer Timeout */
#define PCI_AER_COR_ADVISORY_NF    (1U << 13) /* Advisory Non-Fatal Error */

/* Find the AER extended capability on a PCIe device.
 * Returns the extended capability offset (>= 0x100) on success, or < 0
 * if the device does not support AER.
 */
int pci_find_aer_cap(uint8_t bus, uint8_t slot, uint8_t func);

/* Check and log AER errors for a single PCIe device.
 * Returns a bitmask of error types found (bit 0 = correctable, bit 1 = uncorrectable),
 * or 0 if no errors were found.
 */
int pci_aer_check_device(uint8_t bus, uint8_t slot, uint8_t func);

/* Check AER errors for all PCI devices.  Called periodically. */
void pci_aer_check_all(void);

/* ── Generic extended capability finder ──────────────────────────── */

/* Walk the PCIe extended config space (offsets 0x100..0xFFF) looking for
 * the given extended capability ID.  Returns offset (>= 0x100) on success,
 * or < 0 if not found.  Requires ECAM access. */
int pci_find_ext_cap(int bus, int slot, int func, uint16_t cap_id);

/* ── Access Control Services (ACS, ext cap ID 0x000D) ──────────────
 *
 * ACS provides fine-grained control over peer-to-peer transactions
 * between PCIe functions for security and IOMMU grouping.
 * Defined in PCIe Base Spec r4.0, Section 6.12.
 */

/* ACS capability register offsets (relative to capability base) */
#define PCI_ACS_CAP           0x04  /* ACS Capability Register */
#define PCI_ACS_CTRL          0x06  /* ACS Control Register */
#define PCI_ACS_EGRESS_CTRL   0x08  /* Egress Control Vector (if cap[2]=1) */

/* ACS Capability bits */
#define PCI_ACS_CAP_SV        (1U << 0)  /* Source Validation */
#define PCI_ACS_CAP_TB        (1U << 1)  /* Translation Blocking */
#define PCI_ACS_CAP_RR        (1U << 2)  /* P2P Request Redirect */
#define PCI_ACS_CAP_CR        (1U << 3)  /* P2P Completion Redirect */
#define PCI_ACS_CAP_UF        (1U << 4)  /* Upstream Forwarding */
#define PCI_ACS_CAP_EC        (1U << 5)  /* P2P Egress Control */
#define PCI_ACS_CAP_DT        (1U << 6)  /* Direct Translated P2P */

/* ACS Control bits (same bit positions, writable) */
#define PCI_ACS_CTRL_SV       PCI_ACS_CAP_SV
#define PCI_ACS_CTRL_TB       PCI_ACS_CAP_TB
#define PCI_ACS_CTRL_RR       PCI_ACS_CAP_RR
#define PCI_ACS_CTRL_CR       PCI_ACS_CAP_CR
#define PCI_ACS_CTRL_UF       PCI_ACS_CAP_UF
#define PCI_ACS_CTRL_EC       PCI_ACS_CAP_EC
#define PCI_ACS_CTRL_DT       PCI_ACS_CAP_DT

/* Find the ACS extended capability.
 * Returns offset (>= 0x100) on success, or < 0 if not found. */
int pci_find_acs_cap(uint8_t bus, uint8_t slot, uint8_t func);

/* Log ACS capability details for a device. */
void pci_log_acs_cap(uint8_t bus, uint8_t slot, uint8_t func);

/* ── Latency Tolerance Reporting (LTR, ext cap ID 0x0018) ──────────
 *
 * LTR allows endpoints to report their latency tolerance so the
 * platform can optimise power states.  Defined in PCIe Base Spec r3.0.
 */

/* LTR capability register offsets */
#define PCI_LTR_MAX_SNOOP       0x04  /* Max Snoop Latency Register */
#define PCI_LTR_MAX_NOSNOOP     0x06  /* Max No-Snoop Latency Register */

/* LTR max latency encoding:
 *   [11:0] = value (in ns = value * scale_factor)
 *   [14:12] = scale
 *   [15]    = Require LTR (read-only)
 * scale: 0=1ns, 1=32ns, 2=1024ns, 3=32768ns, 4=1048576ns, 5=33554432ns
 *        6-7 reserved
 */
#define PCI_LTR_VALUE_MASK      0x0FFF
#define PCI_LTR_SCALE_SHIFT      12
#define PCI_LTR_SCALE_MASK      (7 << 12)
#define PCI_LTR_REQUIRE         (1U << 15)

/* Convert LTR encoded latency to nanoseconds (returns 0 on invalid scale) */
uint64_t pci_ltr_to_ns(uint16_t ltr_reg);

/* Find the LTR extended capability.
 * Returns offset (>= 0x100) on success, or < 0 if not found. */
int pci_find_ltr_cap(uint8_t bus, uint8_t slot, uint8_t func);

/* Log LTR capability details for a device. */
void pci_log_ltr_cap(uint8_t bus, uint8_t slot, uint8_t func);

/* ── L1 PM Substates (ext cap ID 0x001E) ──────────────────────────
 *
 * Defines L1.1 and L1.2 low-power substates for PCIe links,
 * reducing power further than baseline L1.
 * Defined in PCIe Base Spec r4.0, Section 6.11.1.
 */

/* L1 PM Substates capability register offsets */
#define PCI_L1PM_CAP            0x04  /* L1 PM Substates Capabilities */
#define PCI_L1PM_CTRL1          0x08  /* L1 PM Substates Control 1 */
#define PCI_L1PM_CTRL2          0x0C  /* L1 PM Substates Control 2 */

/* L1 PM Substates Capability bits */
#define PCI_L1PM_CAP_PCIPM_L12  (1U << 0)  /* PCI-PM L1.2 Supported */
#define PCI_L1PM_CAP_PCIPM_L11  (1U << 1)  /* PCI-PM L1.1 Supported */
#define PCI_L1PM_CAP_ASPM_L12   (1U << 2)  /* ASPM L1.2 Supported */
#define PCI_L1PM_CAP_ASPM_L11   (1U << 3)  /* ASPM L1.1 Supported */
#define PCI_L1PM_CAP_L1SS       (1U << 4)  /* L1 SubState Supported */
#define PCI_L1PM_CAP_CM_REST    (1U << 5)  /* CommonMode Restore Time Supported */
#define PCI_L1PM_CAP_PWR_ON     (1U << 6)  /* Power On Time Supported */
#define PCI_L1PM_CAP_LTR_BLOCK  (1U << 7)  /* LTR Blocking Supported */

/* L1 PM Substates Control 1 bits */
#define PCI_L1PM_CTRL1_CM_REST_TIME_MASK  0xFF  /* CommonModeRestoreTime (bits 7:0) */
#define PCI_L1PM_CTRL1_PWR_ON_TIME_MASK   0xFF00 /* PowerOnTime (bits 15:8) */
#define PCI_L1PM_CTRL1_PWR_ON_SHIFT       8
#define PCI_L1PM_CTRL1_LTR_BLOCK_OVR      (1U << 30) /* LTR Block Override */
#define PCI_L1PM_CTRL1_L12_EN             (1U << 31) /* L1.2 Enable (both PCI-PM & ASPM) */

/* Find the L1 PM Substates extended capability.
 * Returns offset (>= 0x100) on success, or < 0 if not found. */
int pci_find_l1pm_cap(uint8_t bus, uint8_t slot, uint8_t func);

/* Log L1 PM Substates capability details for a device. */
void pci_log_l1pm_cap(uint8_t bus, uint8_t slot, uint8_t func);

#endif
