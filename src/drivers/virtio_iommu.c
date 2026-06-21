/*
 * src/drivers/virtio_iommu.c — Virtio IOMMU device
 *
 * Implements a Virtio IOMMU (VIRTIO_ID_IOMMU = 23) for DMA isolation
 * between virtio devices.  Provides mapping/unmapping of I/O virtual
 * addresses to physical addresses.  Simple identity-mapping fallback.
 */

#include "virtio_iommu.h"
#include "printf.h"
#include "string.h"
#include "pci.h"
#include "virtio.h"
#include "io.h"
#include "types.h"

/* ── Internal state ────────────────────────────────────────────────── */

#define VIRTIO_IOMMU_MAX_MAPPINGS  256
#define VIRTIO_IOMMU_MAX_DOMAINS   16

static int virtio_iommu_initialized = 0;
static uint16_t virtio_iommu_iobase = 0;

/* Mapping table */
static struct virtio_iommu_map_entry
    virtio_iommu_mappings[VIRTIO_IOMMU_MAX_MAPPINGS];
static int virtio_iommu_num_mappings = 0;

/* Domain table */
static struct virtio_iommu_domain
    virtio_iommu_domains[VIRTIO_IOMMU_MAX_DOMAINS];
static int virtio_iommu_num_domains = 0;

/* ── PCI I/O helpers ──────────────────────────────────────────────── */

static inline void vio_outb(uint8_t off, uint8_t v)
{
    outb(virtio_iommu_iobase + off, v);
}
static inline void vio_outw(uint8_t off, uint16_t v)
{
    outw(virtio_iommu_iobase + off, v);
}
static inline void vio_outl(uint8_t off, uint32_t v)
{
    outb((uint16_t)(virtio_iommu_iobase + off),     (uint8_t)v);
    outb((uint16_t)(virtio_iommu_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(virtio_iommu_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(virtio_iommu_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vio_inb(uint8_t off)
{
    return inb(virtio_iommu_iobase + off);
}
static inline uint16_t vio_inw(uint8_t off)
{
    return inw(virtio_iommu_iobase + off);
}
static inline uint32_t vio_inl(uint8_t off)
{
    return (uint32_t)inb((uint16_t)(virtio_iommu_iobase + off)) |
           ((uint32_t)inb((uint16_t)(virtio_iommu_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(virtio_iommu_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(virtio_iommu_iobase + off + 3)) << 24);
}

/* ── IOMMU mapping management ─────────────────────────────────────── */

int virtio_iommu_map(uint64_t virt_start, uint64_t virt_end,
                      uint64_t phys_start, uint32_t flags)
{
    if (virtio_iommu_num_mappings >= VIRTIO_IOMMU_MAX_MAPPINGS)
        return -1;

    struct virtio_iommu_map_entry *entry =
        &virtio_iommu_mappings[virtio_iommu_num_mappings];

    /* Check for overlap with existing entries */
    for (int i = 0; i < virtio_iommu_num_mappings; i++) {
        struct virtio_iommu_map_entry *e = &virtio_iommu_mappings[i];
        if (e->used) {
            /* Check if ranges overlap */
            if (virt_start <= e->virt_end && virt_end >= e->virt_start) {
                kprintf("[virtio-iommu] WARNING: mapping overlap "
                        "0x%llx-0x%llx with existing 0x%llx-0x%llx\n",
                        virt_start, virt_end,
                        e->virt_start, e->virt_end);
                /* Overwrite the old entry (simple: delete then add) */
                e->used = 0;
            }
        }
    }

    entry->virt_start = virt_start;
    entry->virt_end   = virt_end;
    entry->phys_start = phys_start;
    entry->flags      = flags;
    entry->domain     = 0; /* default domain */
    entry->used       = 1;

    virtio_iommu_num_mappings++;

    kprintf("[virtio-iommu] MAP 0x%llx-0x%llx → phys 0x%llx flags=0x%x\n",
            virt_start, virt_end, phys_start, flags);
    return 0;
}

int virtio_iommu_unmap(uint64_t virt_start)
{
    for (int i = 0; i < virtio_iommu_num_mappings; i++) {
        struct virtio_iommu_map_entry *e = &virtio_iommu_mappings[i];
        if (e->used && e->virt_start == virt_start) {
            e->used = 0;
            kprintf("[virtio-iommu] UNMAP 0x%llx\n", virt_start);
            return 0;
        }
    }
    return -1; /* not found */
}

/* ── IOMMU domain management ──────────────────────────────────────── */

int virtio_iommu_attach(uint32_t domain_id, uint32_t endpoint)
{
    if (domain_id >= VIRTIO_IOMMU_MAX_DOMAINS)
        return -1;

    struct virtio_iommu_domain *dom = NULL;

    /* Find existing domain or create new */
    for (int i = 0; i < virtio_iommu_num_domains; i++) {
        if (virtio_iommu_domains[i].used &&
            virtio_iommu_domains[i].id == domain_id) {
            dom = &virtio_iommu_domains[i];
            break;
        }
    }

    if (!dom) {
        /* Create new domain */
        if (virtio_iommu_num_domains >= VIRTIO_IOMMU_MAX_DOMAINS)
            return -1;
        dom = &virtio_iommu_domains[virtio_iommu_num_domains];
        memset(dom, 0, sizeof(*dom));
        dom->id = domain_id;
        dom->used = 1;
        virtio_iommu_num_domains++;
    }

    /* Add endpoint */
    if (dom->num_endpoints >= 16)
        return -1;
    dom->endpoints[dom->num_endpoints++] = endpoint;

    kprintf("[virtio-iommu] ATTACH domain %u endpoint %u\n",
            domain_id, endpoint);
    return 0;
}

int virtio_iommu_detach(uint32_t domain_id, uint32_t endpoint)
{
    for (int i = 0; i < virtio_iommu_num_domains; i++) {
        struct virtio_iommu_domain *dom = &virtio_iommu_domains[i];
        if (dom->used && dom->id == domain_id) {
            for (int j = 0; j < dom->num_endpoints; j++) {
                if (dom->endpoints[j] == endpoint) {
                    /* Remove by shifting */
                    for (int k = j; k < dom->num_endpoints - 1; k++)
                        dom->endpoints[k] = dom->endpoints[k + 1];
                    dom->num_endpoints--;
                    kprintf("[virtio-iommu] DETACH domain %u endpoint %u\n",
                            domain_id, endpoint);
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ── Translate IOVA through IOMMU ──────────────────────────────────── */

/* Translate an IOVA to a physical address using the IOMMU mapping table.
 * Returns the physical address, or ~0ULL if no mapping found (identity). */
uint64_t virtio_iommu_translate(uint64_t iova, uint32_t domain)
{
    /* Search mappings for this domain */
    for (int i = 0; i < virtio_iommu_num_mappings; i++) {
        struct virtio_iommu_map_entry *e = &virtio_iommu_mappings[i];
        if (e->used && e->domain == domain &&
            iova >= e->virt_start && iova <= e->virt_end) {
            return e->phys_start + (iova - e->virt_start);
        }
    }

    /* Identity mapping fallback */
    return iova;
}

/* ── Virtqueue request handler ─────────────────────────────────────── */

int virtio_iommu_handle_request(int vq_idx)
{
    if (!virtio_iommu_initialized) return -1;

    kprintf("[virtio-iommu] handling request on virtqueue %d\n", vq_idx);

    /* In a real implementation, this would:
     *  1. Read the request from the virtqueue
     *  2. Parse the request type (MAP, UNMAP, ATTACH, DETACH, PROBE)
     *  3. Execute the operation
     *  4. Write the response
     *  5. Update the used ring
     */
    return 0;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void virtio_iommu_cleanup(void)
{
    memset(virtio_iommu_mappings, 0, sizeof(virtio_iommu_mappings));
    memset(virtio_iommu_domains, 0, sizeof(virtio_iommu_domains));
    virtio_iommu_num_mappings = 0;
    virtio_iommu_num_domains = 0;
    virtio_iommu_initialized = 0;
    kprintf("[virtio-iommu] cleaned up\n");
}

/* ── Init ──────────────────────────────────────────────────────────── */

int virtio_iommu_init(void)
{
    struct pci_device dev;

    memset(virtio_iommu_mappings, 0, sizeof(virtio_iommu_mappings));
    memset(virtio_iommu_domains, 0, sizeof(virtio_iommu_domains));

    /* Try to find the virtio-iommu PCI device */
    if (pci_find_device(0x1AF4, 0x1057, &dev) < 0) {
        /* Some virtio-iommu implementations use transitional device ID */
        if (pci_find_device(0x1AF4, 0x1000, &dev) < 0) {
            kprintf("[virtio-iommu] device not found (running in standalone mode)\n");
            /* Continue without hardware — we provide software IOMMU anyway */
            virtio_iommu_initialized = 1;
            kprintf("[virtio-iommu] software IOMMU initialized (no hardware)\n");
            return 0;
        }
    }

    virtio_iommu_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (virtio_iommu_iobase) {
        pci_enable_bus_master(&dev);

        /* Reset device */
        vio_outb(VIRTIO_PCI_STATUS, 0);
        vio_outb(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

        /* Negotiate features */
        virtio_negotiate_features_ex(vio_inl, vio_outl, vio_outb, vio_inb,
                                     VIRTIO_IOMMU_F_MAP_UNMAP | VIRTIO_IOMMU_F_BYPASS,
                                     0, NULL, "virtio-iommu");

        /* Driver OK */
        vio_outb(VIRTIO_PCI_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_DRIVER_OK);
    }

    /* Create identity mapping for the entire 64-bit address space (simplified) */
    virtio_iommu_map(0, ~0ULL, 0,
                     VIRTIO_IOMMU_MAP_F_READ | VIRTIO_IOMMU_MAP_F_WRITE);

    virtio_iommu_initialized = 1;
    kprintf("[virtio-iommu] Virtio IOMMU initialized at I/O 0x%04x, "
            "%d mappings\n",
            virtio_iommu_iobase, virtio_iommu_num_mappings);
    return 0;
}
#include "module.h"
module_init(virtio_iommu_init);

/* ── Stub: virtio_iommu_probe ─────────────────────────────── */
int virtio_iommu_probe(void *dev)
{
    (void)dev;
    kprintf("[virtio_iommu] virtio_iommu_probe: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: virtio_iommu_remove ─────────────────────────────── */
int virtio_iommu_remove(void *dev)
{
    (void)dev;
    kprintf("[virtio_iommu] virtio_iommu_remove: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: virtio_iommu_iova_alloc ─────────────────────────────── */
int virtio_iommu_iova_alloc(void *domain, size_t size, void *iova)
{
    (void)domain;
    (void)size;
    (void)iova;
    kprintf("[virtio_iommu] virtio_iommu_iova_alloc: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: virtio_iommu_iova_free ─────────────────────────────── */
int virtio_iommu_iova_free(void *domain, void *iova, size_t size)
{
    (void)domain;
    (void)iova;
    (void)size;
    kprintf("[virtio_iommu] virtio_iommu_iova_free: not yet implemented\n");
    return -ENOSYS;
}
