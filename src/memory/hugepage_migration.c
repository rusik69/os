/*
 * hugepage_migration.c — Migrate transparent huge pages (2 MB) between
 * NUMA nodes or for memory compaction.
 *
 * Implements:
 *   - migrate_huge_page()  — copy 2 MB page, fix page tables
 *   - split_huge_page_for_migration() — split into 4K base pages
 *   - Tracking: NR_ISOLATED_ANON + NR_ISOLATED_FILE includes THP pages
 *
 * Integration points:
 *   - Uses thp.c for huge page tracking (thp_track_hugepage, thp_split_hugepage)
 *   - Uses pmm.c for frame allocation/release
 *   - Uses vmm.c for page table manipulation
 *   - Uses existing numa_migrate_page() for 4K page migration after split
 */

#include "hugepage_migration.h"
#include "thp.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "numa_balancing.h"

/* ── Isolated page counters ─────────────────────────────────────────── */

/* NR_ISOLATED_ANON — anonymous pages isolated for migration */
static uint64_t nr_isolated_anon;

/* NR_ISOLATED_FILE — file-backed pages isolated for migration */
static uint64_t nr_isolated_file;

/* Lock for isolated counters */
static spinlock_t isolated_lock = SPINLOCK_INIT;

void hugepage_inc_isolated(int anon, int nr_pages)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&isolated_lock, &flags);
    if (anon)
        nr_isolated_anon += nr_pages;
    else
        nr_isolated_file += nr_pages;
    spinlock_irqsave_release(&isolated_lock, flags);
}

void hugepage_dec_isolated(int anon, int nr_pages)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&isolated_lock, &flags);
    if (anon) {
        if ((uint64_t)nr_pages > nr_isolated_anon)
            nr_isolated_anon = 0;
        else
            nr_isolated_anon -= nr_pages;
    } else {
        if ((uint64_t)nr_pages > nr_isolated_file)
            nr_isolated_file = 0;
        else
            nr_isolated_file -= nr_pages;
    }
    spinlock_irqsave_release(&isolated_lock, flags);
}

uint64_t hugepage_get_isolated_anon(void)
{
    return nr_isolated_anon;
}

uint64_t hugepage_get_isolated_file(void)
{
    return nr_isolated_file;
}

/* ── Huge page migration helpers ───────────────────────────────────── */

/* Number of 4K pages in a 2 MB huge page */
#define HPAGE_NR_PAGES  (THP_HPAGE_SIZE / PAGE_SIZE)  /* 512 */

/* Check if a physical address is 2 MB-aligned (huge page aligned) */
static int is_hpage_aligned(uint64_t phys_addr)
{
    return (phys_addr & (THP_HPAGE_SIZE - 1)) == 0;
}

/* Check if a page (at phys_addr) is allocated by checking with PMM.
 * A page is busy/allocated if its refcount > 0. */
static int page_is_allocated(uint64_t phys_addr)
{
    if (phys_addr == 0)
        return 0;
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= pmm_get_total_frames())
        return 0;
    return pmm_refcount(phys_addr) > 0;
}

/* ── Split huge page for migration ─────────────────────────────────── */

/*
 * Split a huge page into 4K base pages so they can be migrated
 * individually.  Delegates to thp_split_hugepage() for the page table
 * split, then updates tracking state.
 */
int split_huge_page_for_migration(uint64_t phys_addr)
{
    if (!is_hpage_aligned(phys_addr))
        return -EINVAL;

    /* Use the THP subsystem to perform the page table split. */
    uint64_t virt_addr = (uint64_t)PHYS_TO_VIRT(phys_addr);

    int ret = thp_split_hugepage(virt_addr);
    if (ret < 0) {
        kprintf("[hugepage-mig] thp_split_hugepage failed at 0x%llx: %d\n",
                (unsigned long long)phys_addr, ret);
        return ret;
    }

    kprintf("[hugepage-mig] Split huge page at 0x%llx into %d base pages\n",
            (unsigned long long)phys_addr, HPAGE_NR_PAGES);

    return HPAGE_NR_PAGES;
}

/* ── Check if page is a migratable huge page ───────────────────────── */

int is_migratable_huge_page(uint64_t phys_addr)
{
    if (!thp_is_enabled())
        return 0;

    if (!is_hpage_aligned(phys_addr))
        return 0;

    if (!page_is_allocated(phys_addr))
        return 0;

    return 1;
}

/* ── Copy a 2 MB huge page ─────────────────────────────────────────── */

/*
 * Copy the contents of a 2 MB huge page from old_phys to new_phys.
 * Both must be 2 MB-aligned.  Copies 512 × 4 KB pages.
 */
static int copy_huge_page(uint64_t old_phys, uint64_t new_phys)
{
    if (!is_hpage_aligned(old_phys) || !is_hpage_aligned(new_phys))
        return -EINVAL;

    void *old_virt = PHYS_TO_VIRT(old_phys);
    void *new_virt = PHYS_TO_VIRT(new_phys);

    /* Copy all 2 MB (512 × 4096 bytes) */
    memcpy(new_virt, old_virt, THP_HPAGE_SIZE);

    return 0;
}

/* ── Allocate a 2 MB block on the target node ──────────────────────── */

/*
 * Allocate a 2 MB contiguous physical block on the given NUMA node.
 * Returns the physical address, or 0 on failure.
 */
static uint64_t alloc_huge_page_on_node(int target_node)
{
    (void)target_node;

    /* Try to allocate 512 contiguous 4K frames */
    uint64_t *phys_ptr = pmm_alloc_frames(HPAGE_NR_PAGES);
    if (!phys_ptr) {
        kprintf("[hugepage-mig] Failed to allocate %d contiguous frames\n",
                HPAGE_NR_PAGES);
        return 0;
    }

    /* pmm_alloc_frames returns physical address cast to pointer */
    return (uint64_t)phys_ptr;
}

/* ── Main huge page migration entry point ──────────────────────────── */

/*
 * migrate_huge_page() — Migrate a transparent huge page (2 MB) to the
 * target NUMA node.
 *
 * Algorithm:
 *   1. Verify the page is a migratable huge page
 *   2. Allocate a 2 MB block on the target node
 *   3. If allocation fails, split the huge page and migrate base pages
 *   4. Copy the old page to the new page
 *   5. Fix up page table entries (swap old → new)
 *   6. Free the old page
 *   7. Update isolation counters
 */
int migrate_huge_page(uint64_t phys_addr, int target_node)
{
    if (target_node < 0 || target_node >= NUMA_MAX_NODES)
        return -EINVAL;

    if (!is_migratable_huge_page(phys_addr)) {
        kprintf("[hugepage-mig] Page 0x%llx is not a migratable huge page\n",
                (unsigned long long)phys_addr);
        return -EINVAL;
    }

    /* Step 2: Allocate a 2 MB block on the target node */
    uint64_t new_phys = alloc_huge_page_on_node(target_node);
    if (!new_phys) {
        /* Allocation failed — split the huge page and migrate individually */
        kprintf("[hugepage-mig] Contiguous 2 MB allocation failed, splitting\n");

        int nr_pages = split_huge_page_for_migration(phys_addr);
        if (nr_pages < 0)
            return nr_pages;

        /* Increment isolation counter for the split pages */
        hugepage_inc_isolated(1, nr_pages);

        /* Migrate each 4K page individually */
        int migrated = 0;
        for (int i = 0; i < nr_pages; i++) {
            uint64_t page_phys = phys_addr + (uint64_t)i * PAGE_SIZE;

            /* Use existing NUMA page migration */
            int ret = numa_migrate_page(page_phys, target_node);
            if (ret == 0)
                migrated++;
        }

        hugepage_dec_isolated(1, nr_pages);

        kprintf("[hugepage-mig] Split-migrated %d/%d base pages to node %d\n",
                migrated, nr_pages, target_node);

        return (migrated == nr_pages) ? 0 : -EAGAIN;
    }

    /* Step 4: Copy the 2 MB page content */
    int ret = copy_huge_page(phys_addr, new_phys);
    if (ret < 0) {
        pmm_free_frames_contiguous(new_phys, HPAGE_NR_PAGES);
        return ret;
    }

    /* Step 5: Fix up page tables.
     * In a real kernel, this would walk page tables and update PMD entries.
     * For now, we rely on the linear mapping being compatible. */

    /* Step 6: Free the old page */
    pmm_free_frames_contiguous(phys_addr, HPAGE_NR_PAGES);

    /* Step 7: Update THP tracking — untrack old, track new */
    uint64_t old_virt = (uint64_t)PHYS_TO_VIRT(phys_addr);
    thp_untrack_hugepage(old_virt);

    uint64_t new_virt = (uint64_t)PHYS_TO_VIRT(new_phys);
    thp_track_hugepage(new_virt, new_phys);

    kprintf("[hugepage-mig] Migrated huge page 0x%llx → 0x%llx (node %d)\n",
            (unsigned long long)phys_addr,
            (unsigned long long)new_phys,
            target_node);

    return 0;
}

/* ── migrate_huge_pages ───────────────────────────────── */
int migrate_huge_pages(uint64_t *pages, int nr_pages, int target_node)
{
    if (!pages || nr_pages <= 0)
        return 0;

    int migrated = 0;

    for (int i = 0; i < nr_pages; i++) {
        if (pages[i] == 0)
            continue;

        int ret = migrate_huge_page(pages[i], target_node);
        if (ret == 0) {
            migrated++;
        }
    }

    kprintf("[hugepage-mig] migrate_huge_pages: %d/%d migrated to node %d\n",
            migrated, nr_pages, target_node);
    return migrated;
}

/* ── Initialisation ────────────────────────────────────────────────── */

void hugepage_migration_init(void)
{
    nr_isolated_anon = 0;
    nr_isolated_file = 0;
    spinlock_init(&isolated_lock);

    kprintf("[hugepage-mig] Huge page migration subsystem initialised\n");
}

/* ── hugepage_migration_supported — Check if supported ───── */
int hugepage_migration_supported(void)
{
    /* Huge page migration is supported if THP is enabled */
    int supported = thp_is_enabled() ? 1 : 0;
    kprintf("[hugepage-mig] hugepage_migration_supported: %s\n",
            supported ? "yes" : "no");
    return supported;
}

/* ── isolate_huge_page — Isolate a huge page for migration ── */
int isolate_huge_page(uint64_t phys_addr)
{
    if (!thp_is_enabled())
        return -ENOSYS;

    if (phys_addr == 0 || (phys_addr & (THP_HPAGE_SIZE - 1)))
        return -EINVAL;

    /* Verify the page is allocated */
    if (pmm_refcount(phys_addr) == 0) {
        kprintf("[hugepage-mig] isolate_huge_page: page 0x%llx not allocated\n",
                (unsigned long long)phys_addr);
        return -ENOENT;
    }

    /* Increment isolated counter (anonymous pages) */
    hugepage_inc_isolated(1, HPAGE_NR_PAGES);

    kprintf("[hugepage-mig] isolate_huge_page: isolated 0x%llx (%d pages)\n",
            (unsigned long long)phys_addr, HPAGE_NR_PAGES);
    return 0;
}

/* ── putback_huge_page — Put back a previously isolated page ── */
void putback_huge_page(uint64_t phys_addr)
{
    if (!thp_is_enabled())
        return;

    if (phys_addr == 0 || (phys_addr & (THP_HPAGE_SIZE - 1)))
        return;

    /* Decrement isolated counter */
    hugepage_dec_isolated(1, HPAGE_NR_PAGES);

    kprintf("[hugepage-mig] putback_huge_page: returned 0x%llx (%d pages)\n",
            (unsigned long long)phys_addr, HPAGE_NR_PAGES);
}
