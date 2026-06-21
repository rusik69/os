/*
 * src/drivers/balloon.c — Balloon compaction driver
 *
 * Implements virtio-balloon memory management with compaction support.
 * On memory pressure, inflates balloon to reclaim pages.
 * Hooks into memory compaction for balloon page migration.
 *
 * Provides /sys/kernel/mm/balloon_compaction/ reporting.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "pageblock.h"
#include "compaction.h"
#include "devfs.h"
#include "pci.h"
#include "virtio.h"
#include "io.h"
#include "heap.h"

/* ── Balloon constants ─────────────────────────────────────────────── */

#define BALLOON_PCI_VENDOR      0x1AF4
#define BALLOON_PCI_DEVICE      0x1002  /* virtio-balloon legacy */

/* Max balloon pages (256 MB worth) */
#define BALLOON_MAX_PAGES       65536

/* Number of pages per inflation/deflation batch */
#define BALLOON_BATCH_SIZE      64

/* Virtio-balloon feature bits */
#define VIRTIO_BALLOON_F_MUST_TELL_HOST (1u << 0)
#define VIRTIO_BALLOON_F_STATS_VQ       (1u << 1)
#define VIRTIO_BALLOON_F_DEFLATE_ON_OOM (1u << 2)
#define VIRTIO_BALLOON_F_FREE_PAGE_HINT (1u << 3)
#define VIRTIO_BALLOON_F_PAGE_POISON    (1u << 4)
#define VIRTIO_BALLOON_F_COMPACTION     (1u << 5)  /* our custom feature */

/* Memory statistics tags (virtio-balloon stats) */
#define VIRTIO_BALLOON_S_SWAP_IN        0
#define VIRTIO_BALLOON_S_SWAP_OUT       1
#define VIRTIO_BALLOON_S_MAJFLT         2
#define VIRTIO_BALLOON_S_MINFLT         3
#define VIRTIO_BALLOON_S_MEMFREE        4
#define VIRTIO_BALLOON_S_MEMTOT         5
#define VIRTIO_BALLOON_S_AVAIL          6
#define VIRTIO_BALLOON_S_CACHES         7
#define VIRTIO_BALLOON_S_HTLB_PGALLOC   8
#define VIRTIO_BALLOON_S_HTLB_PGFAIL    9

/* ── Balloon page descriptor ───────────────────────────────────────── */

/* Each balloon page is tracked with its physical address */
struct balloon_page {
    uint64_t phys_addr;     /* physical address of this balloon page */
    int      inflated;      /* 1 = currently inflated (host-held) */
};

/* ── Balloon device state ──────────────────────────────────────────── */

static struct {
    /* PCI / virtio state */
    uint16_t iobase;
    int      present;

    /* Balloon state */
    struct balloon_page pages[BALLOON_MAX_PAGES];
    int      num_pages;          /* total pages tracked */
    int      inflate_count;      /* currently inflated pages */
    int      target_pages;       /* desired balloon size */

    /* Compaction hooks */
    int      compaction_enabled; /* 1 = allow balloon page migration */

    /* Statistics */
    uint64_t stat_inflate;
    uint64_t stat_deflate;
    uint64_t stat_migrate;
    uint64_t stat_fail;
} balloon;

/* ── I/O helpers ───────────────────────────────────────────────────── */

static inline void bal_outb(uint8_t off, uint8_t v)  { outb(balloon.iobase + off, v); }
static inline void bal_outw(uint8_t off, uint16_t v) { outw(balloon.iobase + off, v); }
static inline void bal_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(balloon.iobase + off),     (uint8_t)v);
    outb((uint16_t)(balloon.iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(balloon.iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(balloon.iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  bal_inb(uint8_t off)  { return inb(balloon.iobase + off); }
static inline uint16_t bal_inw(uint8_t off)  { return inw(balloon.iobase + off); }
static inline uint32_t bal_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(balloon.iobase + off)) |
           ((uint32_t)inb((uint16_t)(balloon.iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(balloon.iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(balloon.iobase + off + 3)) << 24);
}

/* ── Core balloon operations ───────────────────────────────────────── */

/* Inflate: "take" a page from the guest (mark as balloon-owned).
 * The host holds onto this page for potential re-use. */
static int balloon_inflate_one(void)
{
    if (balloon.inflate_count >= balloon.num_pages)
        return -1; /* no more tracked pages */

    struct balloon_page *bp = &balloon.pages[balloon.inflate_count];
    if (bp->inflated)
        return -1; /* already inflated */

    /* Allocate a physical page */
    uint64_t phys = pmm_alloc_frame();
    if (phys == 0)
        return -1; /* OOM */

    bp->phys_addr = phys;
    bp->inflated = 1;
    balloon.inflate_count++;
    balloon.stat_inflate++;
    return 0;
}

/* Deflate: "release" a page back to the guest. */
static int balloon_deflate_one(void)
{
    if (balloon.inflate_count <= 0)
        return -1;

    balloon.inflate_count--;
    struct balloon_page *bp = &balloon.pages[balloon.inflate_count];
    if (!bp->inflated)
        return -1;

    /* Free the page back to the system */
    pmm_free_frame(bp->phys_addr);
    bp->phys_addr = 0;
    bp->inflated = 0;
    balloon.stat_deflate++;
    return 0;
}

/* Adjust balloon to reach target_pages */
static void balloon_adjust(void)
{
    int diff = balloon.target_pages - balloon.inflate_count;

    if (diff > BALLOON_BATCH_SIZE)
        diff = BALLOON_BATCH_SIZE;
    else if (diff < -BALLOON_BATCH_SIZE)
        diff = -BALLOON_BATCH_SIZE;

    if (diff > 0) {
        for (int i = 0; i < diff; i++) {
            if (balloon_inflate_one() < 0)
                break;
        }
    } else if (diff < 0) {
        for (int i = 0; i < -diff; i++) {
            if (balloon_deflate_one() < 0)
                break;
        }
    }
}

/* ── Balloon compaction hooks ──────────────────────────────────────── */

/* Isolate a balloon page for migration.
 * Called during compaction scan to identify balloon-owned pages
 * that can be moved. */
int balloon_page_isolate(uint64_t phys_addr)
{
    if (!balloon.compaction_enabled)
        return 0;

    /* Check if this phys_addr belongs to the balloon */
    for (int i = 0; i < balloon.inflate_count; i++) {
        struct balloon_page *bp = &balloon.pages[i];
        if (bp->inflated && bp->phys_addr == phys_addr) {
            /* Mark as isolate (MIGRATE_ISOLATE) — pageblock will do this */
            return 1; /* yes, this is a balloon page */
        }
    }
    return 0;
}

/* Migrate a balloon page: copy contents from old_phys to new_phys,
 * then update balloon tracking and re-inflate. */
int balloon_page_migrate(uint64_t old_phys, uint64_t new_phys)
{
    if (!balloon.compaction_enabled)
        return -1;
    if (old_phys == 0 || new_phys == 0)
        return -1;

    /* Find the balloon page entry */
    for (int i = 0; i < balloon.inflate_count; i++) {
        struct balloon_page *bp = &balloon.pages[i];
        if (bp->inflated && bp->phys_addr == old_phys) {
            /* Copy data from old page to new page */
            uint64_t *old_virt = (uint64_t *)PHYS_TO_VIRT(old_phys);
            uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT(new_phys);
            for (int j = 0; j < (int)(PAGE_SIZE / 8); j++)
                new_virt[j] = old_virt[j];

            /* Free the old page */
            pmm_free_frame(old_phys);

            /* Update tracking to point to new page */
            bp->phys_addr = new_phys;

            /* Re-inflate: the new page is now balloon-owned */
            balloon.stat_migrate++;
            return 0;
        }
    }
    return -1; /* not found */
}

/* Dequeue a balloon page for migration (called during compaction scan).
 * Returns 1 if the page was a balloon page and is now isolated. */
int balloon_page_dequeue(uint64_t phys_addr)
{
    if (!balloon.compaction_enabled)
        return 0;

    return balloon_page_isolate(phys_addr);
}

/* ── Sysfs reporting handler ───────────────────────────────────────── */

static int balloon_sysfs_read(void *priv, void *buf,
                               uint32_t max_size, uint32_t *out_size)
{
    char tmp[256];
    int n = snprintf(tmp, sizeof(tmp),
                     "balloon_compaction:\n"
                     "  inflate_count:  %d\n"
                     "  target_pages:   %d\n"
                     "  max_pages:      %d\n"
                     "  compaction:     %s\n"
                     "  stat_inflate:   %llu\n"
                     "  stat_deflate:   %llu\n"
                     "  stat_migrate:   %llu\n"
                     "  stat_fail:      %llu\n"
                     "  fragmentation:  %llu%%\n",
                     balloon.inflate_count,
                     balloon.target_pages,
                     balloon.num_pages,
                     balloon.compaction_enabled ? "enabled" : "disabled",
                     balloon.stat_inflate,
                     balloon.stat_deflate,
                     balloon.stat_migrate,
                     balloon.stat_fail,
                     (unsigned long long)compaction_fragmentation_pct());

    uint32_t len = (max_size < (uint32_t)n) ? max_size : (uint32_t)n;
    memcpy(buf, tmp, len);
    *out_size = len;
    return 0;
}

/* ── Memory pressure callback ──────────────────────────────────────── */

/* Called when the system detects memory pressure.
 * Inflates the balloon to reclaim pages and trigger compaction. */
void balloon_memory_pressure(void)
{
    if (!balloon.present) return;

    /* Increase target on pressure */
    balloon.target_pages += BALLOON_BATCH_SIZE;
    if (balloon.target_pages > balloon.num_pages)
        balloon.target_pages = balloon.num_pages;

    balloon_adjust();

    /* Run compaction to defragment */
    compaction_run();

    kprintf("[balloon] memory pressure: target=%d inflated=%d "
            "compaction=%llu%%\n",
            balloon.target_pages, balloon.inflate_count,
            (unsigned long long)compaction_fragmentation_pct());
}

/* ── Init / Cleanup ────────────────────────────────────────────────── */

void balloon_init(void)
{
    struct pci_device dev;

    memset(&balloon, 0, sizeof(balloon));

    /* Try to find virtio-balloon PCI device */
    if (pci_find_device(BALLOON_PCI_VENDOR, BALLOON_PCI_DEVICE, &dev) < 0) {
        /* No hardware balloon — create software balloon anyway */
        kprintf("[balloon] virtio-balloon device not found, "
                "running software balloon\n");
    } else {
        balloon.iobase = (uint16_t)(dev.bar[0] & ~0x3u);
        if (balloon.iobase) {
            pci_enable_bus_master(&dev);

            /* Reset and init device */
            bal_outb(VIRTIO_PCI_STATUS, 0);
            bal_outb(VIRTIO_PCI_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

            virtio_negotiate_features_ex(bal_inl, bal_outl, bal_outb, bal_inb,
                                         VIRTIO_BALLOON_F_MUST_TELL_HOST |
                                         VIRTIO_BALLOON_F_DEFLATE_ON_OOM |
                                         VIRTIO_BALLOON_F_COMPACTION,
                                         0, NULL, "balloon");

            bal_outb(VIRTIO_PCI_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                     VIRTIO_STATUS_DRIVER_OK);

            balloon.present = 1;
        }
    }

    /* Initialize page tracking array */
    balloon.num_pages = BALLOON_MAX_PAGES;
    for (int i = 0; i < balloon.num_pages; i++) {
        balloon.pages[i].phys_addr = 0;
        balloon.pages[i].inflated = 0;
    }

    balloon.compaction_enabled = 1;

    /* Register /sys/kernel/mm/balloon_compaction/ reporting */
    if (devfs_register_device("balloon_compaction", NULL,
                              balloon_sysfs_read, NULL) < 0) {
        kprintf("[balloon] Warning: failed to register sysfs node\n");
    }

    /* Set a reasonable initial target */
    balloon.target_pages = balloon.num_pages / 4;

    kprintf("[balloon] Balloon compaction initialized: "
            "max=%d pages, target=%d, compaction=%s\n",
            balloon.num_pages, balloon.target_pages,
            balloon.compaction_enabled ? "enabled" : "disabled");
}
#include "module.h"
module_init(balloon_init);

/* ── Stub: balloon_inflate ─────────────────────────────── */
int balloon_inflate(size_t count)
{
    (void)count;
    kprintf("[balloon] balloon_inflate: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: balloon_deflate ─────────────────────────────── */
int balloon_deflate(size_t count)
{
    (void)count;
    kprintf("[balloon] balloon_deflate: not yet implemented\n");
    return -ENOSYS;
}
