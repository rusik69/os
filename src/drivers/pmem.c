/*
 * pmem.c — Persistent Memory (NVDIMM) block device driver (Item 193)
 *
 * Exposes ACPI NFIT-detected persistent memory (PMEM) SPA ranges as
 * block devices.  Each persistent memory range becomes a /dev/pmem*
 * device that can be formatted with a filesystem and mounted.
 *
 * The NVDIMM regions are identified by parsing the ACPI NFIT table
 * during boot (see acpi.c).  This driver reads the parsed SPA ranges
 * via acpi_nfit_get_count()/acpi_nfit_get_spa() and registers a block
 * device for each PMEM region.
 *
 * PMEM devices are memory-mapped: reads and writes go directly to the
 * physical memory backing the NVDIMM.  For simplicity and correctness
 * on emulated platforms (QEMU NVDIMM via -object memory-backend-file,
 * -device nvdimm), we use direct memcpy to/from the physical address
 * range (mapped via PHYS_TO_VIRT).
 *
 * Reference:
 *   - ACPI NFIT spec: http://uefi.org/specifications
 *   - QEMU NVDIMM: qemu -object memory-backend-file,... -device nvdimm
 */

#include "acpi.h"
#include "blockdev.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"        /* PHYS_TO_VIRT */

/* ── Constants ───────────────────────────────────────────────────── */

/* Maximum number of PMEM devices we can register */
#define PMEM_MAX_DEVICES NFIT_MAX_SPA_RANGES

/* Device name base */
#define PMEM_DEV_NAME_BASE "pmem"

/* Each PMEM device has its own sector count */
static uint64_t g_pmem_sectors[PMEM_MAX_DEVICES];
static int      g_pmem_devices = 0;          /* Number of registered PMEM devices */
static int      g_pmem_initialized = 0;

/* ── Block device driver implementation ──────────────────────────── */

/*
 * Submit callback for PMEM block devices.
 *
 * Since NVDIMM is directly addressable memory, we can fulfil read/write
 * requests with simple memcpy operations to/from the physical address.
 *
 * The physical base address for the device is determined by looking up
 * the device ID (which corresponds to the SPA range index).
 */
static int pmem_submit(struct blk_request *req) {
    if (!req)
        return -1;

    int dev_id = (int)req->dev_id;

    /* Calculate the device index within our PMEM array.
     * Block device IDs are assigned sequentially starting from
     * a well-known base (BLOCKDEV_PMEM0). */
    int pmem_idx = dev_id - BLOCKDEV_PMEM0;
    if (pmem_idx < 0 || pmem_idx >= g_pmem_devices) {
        kprintf("[pmem] ERROR: invalid device id %d (pmem_idx=%d)\n",
                dev_id, pmem_idx);
        req->result = -1;
        return -1;
    }

    /* Get the SPA range info for this device */
    struct nfit_spa_range_info spa;
    if (acpi_nfit_get_spa(pmem_idx, &spa) != 0) {
        kprintf("[pmem] ERROR: cannot get SPA info for index %d\n",
                pmem_idx);
        req->result = -1;
        return -1;
    }

    /* Calculate the physical address for this I/O */
    uint64_t lba = req->lba;
    uint32_t count = req->count;
    uint64_t offset = lba * 512ULL;  /* 512-byte sectors */
    uint64_t length = (uint64_t)count * 512ULL;

    /* Bounds check: ensure the request fits within the SPA range */
    if (offset + length > spa.spa_length) {
        kprintf("[pmem] ERROR: request beyond device boundary "
                "(lba=%llu count=%u offset=0x%llx length=0x%llx "
                "spa_length=0x%llx)\n",
                (unsigned long long)lba, count,
                (unsigned long long)offset,
                (unsigned long long)length,
                (unsigned long long)spa.spa_length);
        req->result = -1;
        return -1;
    }

    /* Compute the virtual address of the target memory */
    uint64_t phys_addr = spa.spa_base + offset;
    void *virt_addr = (void *)(uintptr_t)(phys_addr + 0xFFFF800000000000ULL);
    void *buf = req->buf;

    if (!buf) {
        req->result = -1;
        return -1;
    }

    /* Perform the data transfer */
    if (req->flags & BLK_REQ_READ) {
        memcpy(buf, virt_addr, (size_t)length);
    } else if (req->flags & BLK_REQ_WRITE) {
        memcpy(virt_addr, buf, (size_t)length);
    } else {
        /* Unsupported request type (e.g., FLUSH, DISCARD) */
        req->result = -1;
        return -1;
    }

    req->result = 0;
    return 0;
}

/* ── Initialization ──────────────────────────────────────────────── */

void pmem_init(void) {
    if (g_pmem_initialized)
        return;

    /* Query the number of PMEM SPA ranges found during NFIT parsing */
    int count = acpi_nfit_get_count();
    if (count <= 0) {
        kprintf("[pmem] No NVDIMM regions found (NFIT not present or "
                "no PMEM ranges)\n");
        g_pmem_initialized = 1;
        return;
    }

    if (count > PMEM_MAX_DEVICES) {
        kprintf("[pmem] WARNING: %u PMEM regions found, but only "
                "%d supported; ignoring extras\n",
                count, PMEM_MAX_DEVICES);
        count = PMEM_MAX_DEVICES;
    }

    g_pmem_devices = count;

    kprintf("[pmem] Registering %d persistent memory device(s):\n",
            count);

    for (int i = 0; i < count; i++) {
        struct nfit_spa_range_info spa;
        if (acpi_nfit_get_spa(i, &spa) != 0) {
            kprintf("[pmem] ERROR: cannot get SPA info for index %d, "
                    "skipping\n", i);
            continue;
        }

        /* Calculate sector count from SPA length */
        uint64_t sector_count = spa.spa_length / 512ULL;
        g_pmem_sectors[i] = sector_count;

        /* Build device name: pmem0, pmem1, ... */
        char dev_name[16];
        int n = snprintf(dev_name, sizeof(dev_name), "%s%d",
                         PMEM_DEV_NAME_BASE, i);
        if (n < 0 || n >= (int)sizeof(dev_name)) {
            /* Truncation — fall back to generic name */
            strncpy(dev_name, PMEM_DEV_NAME_BASE, sizeof(dev_name) - 1);
            dev_name[sizeof(dev_name) - 1] = '\0';
        }

        /* Register the block device */
        int dev_id = BLOCKDEV_PMEM0 + i;
        int ret = blockdev_register(dev_id, dev_name,
                                    pmem_submit, NULL,
                                    sector_count, 0);
        if (ret != 0) {
            kprintf("[pmem] ERROR: blockdev_register(%d, %s) failed "
                    "with %d\n", dev_id, dev_name, ret);
            continue;
        }

        kprintf("  %s (id=%d) base=0x%llx len=0x%llx (%llu MB) "
                "%llu sectors%s\n",
                dev_name, dev_id,
                (unsigned long long)spa.spa_base,
                (unsigned long long)spa.spa_length,
                (unsigned long long)(spa.spa_length / (1024ULL * 1024ULL)),
                (unsigned long long)sector_count,
                (spa.flags & NFIT_SPA_READ_ONLY) ? " [RO]" : "");
    }

    g_pmem_initialized = 1;
    kprintf("[OK] pmem: %d persistent memory device(s) registered\n",
            count);
}

/* Return the number of registered PMEM devices */
int pmem_get_device_count(void) {
    return g_pmem_devices;
}
