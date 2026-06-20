#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"

/* cmd_iommu — show IOMMU status, DMAR tables, per-device mappings */
void cmd_iommu(void) {
    kprintf("IOMMU status:\n");
    /* Use the IOMMU driver's public API */
    extern int iommu_is_active(void);
    int active = iommu_is_active();

    if (active) {
        kprintf("  State:       active\n");
        kprintf("  Remapping:   DMA remapping active\n");

        extern int iommu_unit_count(void);
        extern int iommu_device_count(void);
        int units = iommu_unit_count();
        int devs = iommu_device_count();

        kprintf("  DRHD units:  %d\n", units);
        kprintf("  Devices:     %d\n", devs);
        kprintf("  Translation: %s\n",
                devs > 0 ? "DMA remapping with page tables" : "none");
    } else {
        kprintf("  State:       inactive (no VT-d hardware detected)\n");
    }
    kprintf("  DMAR table:  %s\n",
            active ? "parsed and active" : "not found or disabled");
}
