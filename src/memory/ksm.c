/* ksm.c — Kernel Same-page Merging with Scan Pacing and NUMA Awareness
 *
 * Implements:
 *   1. Incremental scanning with position tracking (avoids O(n²) per cycle)
 *   2. Scan pacing: batch size adapts to memory pressure
 *   3. NUMA-aware merging: prefer merging pages on the same NUMA node
 *   4. Memory pressure throttle: pause scanning under severe pressure
 */

#define KERNEL_INTERNAL
#include "ksm.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define KSM_MAX_PAGES     65536  /* Max tracked pages (up from 1024) */
#define KSM_MAX_HASH_CHAIN 16    /* Max same-hash pages in a chain */

/* Batch size limits based on memory pressure */
#define KSM_BATCH_MAX      1024  /* Max pages to scan per cycle */
#define KSM_BATCH_NORMAL   256   /* Normal scan rate */
#define KSM_BATCH_MIN      16    /* Minimum scan rate (under pressure) */

/* Memory pressure thresholds (free ratio in permille: 0-1000)
 * Using integer permille avoids SSE dependency from floating-point
 * arithmetic, which conflicts with -mno-sse in debug builds. */
#define KSM_FREE_PERMILLE(_ratio) ((int)((_ratio) * 1000))
#define KSM_PRESSURE_HIGH   500   /* Above 50% free → scan aggressively */
#define KSM_PRESSURE_LOW    100   /* Below 10% free → scan minimally */
#define KSM_PRESSURE_PAUSE   50   /* Below 5% free → pause entirely */

/* How many cycles before a page is re-scanned (to avoid rechecking same
 * pages every cycle — only relevant when max_batch >= KSM_MAX_PAGES) */
#define KSM_SCAN_AGE_LIMIT 8

/* ── Per-page structure ─────────────────────────────────────────────── */

struct ksm_page {
    uint64_t phys_addr;    /* Physical address of the page */
    uint64_t hash;         /* Content hash (XOR of all 64-bit words) */
    unsigned int merged:1; /* Whether this page is a merged alias */
    unsigned int numa_node:7; /* NUMA node ID (0 for single-node systems) */
    unsigned int age:8;    /* Cycles since last scan (capped at 255) */
    unsigned int refcount:16; /* Reference count if merged */
};

/* ── Static state ───────────────────────────────────────────────────── */

static struct ksm_page ksm_pages[KSM_MAX_PAGES];
static int ksm_page_count = 0;     /* Number of pages currently tracked */
static int ksm_enabled = 0;        /* Global enable/disable */

/* Scan cursor — we advance this each cycle to scan incrementally rather
 * than scanning all pages at once.  When it wraps, we reset ages so that
 * no page is left unscanned forever. */
static int ksm_scan_pos = 0;

/* Statistics */
static uint64_t ksm_merged_pages = 0;
static uint64_t ksm_unmergeable_pages = 0;
static uint64_t ksm_scan_count = 0;
static uint64_t ksm_total_scanned = 0;   /* Cumulative pages examined */

/* ── Simple content hash (XOR of all 64-bit words in a 4K page) ────── */

static uint64_t ksm_hash_page(uint64_t phys_addr)
{
    if (!phys_addr) return 0;
    volatile uint64_t *data = (volatile uint64_t *)PHYS_TO_VIRT(phys_addr);
    uint64_t hash = 0;
    for (int i = 0; i < 512; i++) {  /* 4096 / 8 = 512 */
        hash ^= data[i];
    }
    return hash;
}

/* ── Full content comparison ───────────────────────────────────────── */

static int ksm_pages_equal(uint64_t phys_a, uint64_t phys_b)
{
    if (phys_a == phys_b) return 1;
    volatile uint64_t *a = (volatile uint64_t *)PHYS_TO_VIRT(phys_a);
    volatile uint64_t *b = (volatile uint64_t *)PHYS_TO_VIRT(phys_b);
    for (int i = 0; i < 512; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* ── Memory pressure detection ────────────────────────────────────────
 *
 * Returns free ratio in permille (0-1000) indicating how much free
 * memory is available (1000 = all free, 0 = none).
 * Uses the PMM free ratio as a proxy.
 */
static int ksm_free_permille(void)
{
    uint64_t total = pmm_get_total_frames();
    uint64_t used  = pmm_get_used_frames();
    if (total == 0) return 0;           /* No info → assume no free */
    if (used > total) return 0;         /* Sanity */
    if (used == 0) return 1000;         /* Fully free */
    /* Calculate free/total as permille */
    uint64_t free = total - used;
    return (int)(free * 1000ULL / total);
}

/* ── Compute adaptive batch size ──────────────────────────────────────
 *
 * Scales the scan batch size inversely with memory pressure:
 *   - Low pressure  (free > 50%): scan up to KSM_BATCH_MAX pages
 *   - Normal        (free ~25%):  scan KSM_BATCH_NORMAL pages
 *   - High pressure (free < 10%): scan KSM_BATCH_MIN pages
 *   - Severe        (free <  5%): return 0 (pause scanning)
 */
static int ksm_compute_batch(void)
{
    if (!ksm_enabled)
        return 0;

    int free_perm = ksm_free_permille();

    /* Severe pressure — pause scanning, we need every CPU cycle */
    if (free_perm < KSM_PRESSURE_PAUSE)
        return 0;

    /* High pressure — minimal scanning */
    if (free_perm < KSM_PRESSURE_LOW)
        return KSM_BATCH_MIN;

    /* Medium pressure — linear scaling between MIN and NORMAL */
    if (free_perm < KSM_PRESSURE_HIGH) {
        int t_num = free_perm - KSM_PRESSURE_LOW;
        int t_den = KSM_PRESSURE_HIGH - KSM_PRESSURE_LOW;
        int batch = KSM_BATCH_MIN
                  + (t_num * (KSM_BATCH_NORMAL - KSM_BATCH_MIN)) / t_den;
        if (batch < KSM_BATCH_MIN) batch = KSM_BATCH_MIN;
        if (batch > KSM_BATCH_NORMAL) batch = KSM_BATCH_NORMAL;
        return batch;
    }

    /* Low pressure — scan aggressively */
    return KSM_BATCH_MAX;
}

/* ── Scan one batch ──────────────────────────────────────────────────
 *
 * Scans up to `max_batch` pages starting from `ksm_scan_pos`, wrapping
 * around.  Within each batch, pages with matching hashes and matching
 * NUMA nodes are compared and merged.
 *
 * Returns the number of pages actually examined this cycle.
 */
static int ksm_scan_batch(int max_batch)
{
    if (ksm_page_count < 2 || max_batch <= 0)
        return 0;

    int scanned = 0;
    int start_pos = ksm_scan_pos;

    while (scanned < max_batch && ksm_page_count > 0) {
        /* Wrap around */
        if (ksm_scan_pos >= ksm_page_count)
            ksm_scan_pos = 0;

        int i = ksm_scan_pos;
        ksm_scan_pos++;
        scanned++;

        /* Skip already-merged or too-young pages */
        struct ksm_page *pi = &ksm_pages[i];
        if (pi->merged)
            continue;
        if (pi->age < KSM_SCAN_AGE_LIMIT) {
            pi->age++;
            continue;
        }
        pi->age = 0;  /* Reset age after scanning */

        /* Recompute hash — content may have changed since registration */
        pi->hash = ksm_hash_page(pi->phys_addr);

        /* Look for a merge candidate among other pages.
         * To keep O(n) per batch, we only check nearby pages that share
         * the same hash and NUMA node.  We scan forward linearly from i+1
         * up to a reasonable limit. */
        int candidates_checked = 0;
        int best_j = -1;

        for (int j = i + 1;
             j < ksm_page_count && candidates_checked < KSM_MAX_HASH_CHAIN;
             j++, candidates_checked++) {
            struct ksm_page *pj = &ksm_pages[j];

            /* Skip merged pages and pages from different NUMA nodes */
            if (pj->merged)
                continue;
            if (pj->numa_node != pi->numa_node)
                continue;

            /* Quick hash check */
            if (pj->hash != pi->hash) {
                /* Recompute their hash too — it might have changed */
                pj->hash = ksm_hash_page(pj->phys_addr);
                if (pj->hash != pi->hash)
                    continue;
            }

            /* Full content comparison */
            if (ksm_pages_equal(pi->phys_addr, pj->phys_addr)) {
                best_j = j;
                break;  /* Take the first match (prefer adjacent pages) */
            } else {
                ksm_unmergeable_pages++;
            }
        }

        if (best_j >= 0) {
            /* Merge: keep page i, alias page j to i's physical frame */
            ksm_pages[best_j].merged = 1;
            ksm_pages[best_j].phys_addr = pi->phys_addr;
            ksm_pages[best_j].numa_node = pi->numa_node;
            pi->refcount = (pi->refcount + 1) & 0xFFFF;
            if (pi->refcount == 0) pi->refcount = 1;  /* Saturate at 65535 */
            ksm_merged_pages++;
        }
    }

    /* If we wrapped all the way around (full pass), reset ages so that
     * every page gets scanned regularly.  We reset to age = age/2 to avoid
     * starving pages that just got their age reset. */
    if (ksm_scan_pos < start_pos) {
        for (int i = 0; i < ksm_page_count; i++) {
            ksm_pages[i].age = ksm_pages[i].age > 1
                               ? ksm_pages[i].age / 2 : 0;
        }
    }

    return scanned;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void __init ksm_init(void)
{
    memset(ksm_pages, 0, sizeof(ksm_pages));
    ksm_page_count = 0;
    ksm_enabled = 0;
    ksm_scan_pos = 0;
    ksm_merged_pages = 0;
    ksm_unmergeable_pages = 0;
    ksm_scan_count = 0;
    ksm_total_scanned = 0;

    kprintf("[MEM] KSM (Kernel Same-page Merging) v2 — scan pacing + NUMA awareness\n");
}

void ksm_set_enabled(int enabled)
{
    ksm_enabled = enabled;
    if (enabled) {
        ksm_scan_pos = 0;  /* Reset scan cursor */
        kprintf("[MEM] KSM enabled (pacing active)\n");
    } else {
        kprintf("[MEM] KSM disabled\n");
    }
}

int ksm_is_enabled(void)
{
    return ksm_enabled;
}

int ksm_register_region(uint64_t addr, size_t size, int numa_node)
{
    if (size == 0 || (size & 0xFFF) != 0)
        return -EINVAL;  /* Must be page-aligned */

    uint64_t phys = VIRT_TO_PHYS(addr);
    int pages_needed = (int)(size / PAGE_SIZE);

    if (ksm_page_count + pages_needed > KSM_MAX_PAGES)
        return -ENOSPC;  /* Full */

    for (int i = 0; i < pages_needed; i++) {
        struct ksm_page *kp = &ksm_pages[ksm_page_count++];
        kp->phys_addr = phys + (uint64_t)i * PAGE_SIZE;
        kp->hash      = ksm_hash_page(kp->phys_addr);
        kp->merged    = 0;
        kp->numa_node = (numa_node >= 0 && numa_node <= 127)
                         ? (unsigned int)numa_node : 0;
        kp->age       = 0;
        kp->refcount  = 1;
    }
    return 0;
}

/* Legacy wrapper (defaults to NUMA node 0) */
int ksm_register_region_legacy(uint64_t addr, size_t size)
{
    return ksm_register_region(addr, size, 0);
}

/* ── Physical-address-based registration (with duplicate check) ────── */

int ksm_register_phys(uint64_t phys, int numa_node)
{
    if (phys == 0 || (phys & 0xFFF) != 0)
        return -EINVAL;

    if (ksm_page_count >= KSM_MAX_PAGES)
        return -ENOSPC;

    /* Check for duplicates — the same physical page already tracked */
    for (int i = 0; i < ksm_page_count; i++) {
        if (ksm_pages[i].phys_addr == phys && !ksm_pages[i].merged)
            return 0;  /* Already registered, no-op */
    }

    struct ksm_page *kp = &ksm_pages[ksm_page_count++];
    kp->phys_addr = phys;
    kp->hash      = ksm_hash_page(phys);
    kp->merged    = 0;
    kp->numa_node = (numa_node >= 0 && numa_node <= 127)
                     ? (unsigned int)numa_node : 0;
    kp->age       = 0;
    kp->refcount  = 1;
    return 0;
}

int ksm_unregister_phys(uint64_t phys)
{
    if (phys == 0 || (phys & 0xFFF) != 0)
        return -EINVAL;

    for (int i = 0; i < ksm_page_count; i++) {
        if (ksm_pages[i].phys_addr == phys && !ksm_pages[i].merged) {
            /* Remove by swapping with last element */
            ksm_pages[i] = ksm_pages[--ksm_page_count];
            /* Clear vacated slot */
            memset(&ksm_pages[ksm_page_count], 0, sizeof(struct ksm_page));
            /* Adjust scan position if needed */
            if (ksm_scan_pos > i)
                ksm_scan_pos--;
            if (ksm_scan_pos >= ksm_page_count)
                ksm_scan_pos = 0;
            return 0;
        }
    }
    return -ENOENT;
}

int ksm_unregister_region(uint64_t addr)
{
    uint64_t phys = VIRT_TO_PHYS(addr);

    for (int i = 0; i < ksm_page_count; i++) {
        if (ksm_pages[i].phys_addr == phys && !ksm_pages[i].merged) {
            /* Remove by swapping with last element */
            ksm_pages[i] = ksm_pages[--ksm_page_count];
            /* Clear vacated slot */
            memset(&ksm_pages[ksm_page_count], 0, sizeof(struct ksm_page));
            /* Adjust scan position if needed */
            if (ksm_scan_pos > i)
                ksm_scan_pos--;
            if (ksm_scan_pos >= ksm_page_count)
                ksm_scan_pos = 0;
            return 0;
        }
    }
    return -ENOENT;
}

/* ── Main scan cycle — called periodically from a kernel thread or
 *    timer context.  Adapts scan intensity to memory pressure. ─────── */

void ksm_scan_cycle(void)
{
    if (!ksm_enabled || ksm_page_count < 2)
        return;

    ksm_scan_count++;

    /* Compute batch size based on current memory pressure */
    int batch = ksm_compute_batch();
    if (batch <= 0)
        return;  /* Under severe pressure — skip this cycle */

    /* Scan at most `batch` pages */
    int scanned = ksm_scan_batch(batch);
    ksm_total_scanned += scanned;
}

/* ── Statistics ─────────────────────────────────────────────────────── */

uint64_t ksm_get_merged_pages(void)
{
    return ksm_merged_pages;
}

uint64_t ksm_get_unmergeable_pages(void)
{
    return ksm_unmergeable_pages;
}

uint64_t ksm_get_scan_count(void)
{
    return ksm_scan_count;
}

uint64_t ksm_get_total_scanned(void)
{
    return ksm_total_scanned;
}

int ksm_get_page_count(void)
{
    return ksm_page_count;
}

int ksm_get_scan_batch(void)
{
    return ksm_compute_batch();
}
#include "module.h"
module_init(ksm_init);

/* ── Stub: ksm_scan ─────────────────────────────────────────── */
static int ksm_scan(void)
{
    kprintf("[ksm] ksm_scan: starting scan cycle\n");
    ksm_scan_cycle();
    return ksm_get_total_scanned() > 0 ? 0 : -EAGAIN;
}

/* ── Stub: ksm_do_scan ──────────────────────────────────────── */
static int ksm_do_scan(int nr_to_scan)
{
    if (nr_to_scan <= 0) {
        kprintf("[ksm] ksm_do_scan: invalid count %d\n", nr_to_scan);
        return -EINVAL;
    }
    kprintf("[ksm] ksm_do_scan: scanning up to %d pages\n", nr_to_scan);
    int before = (int)ksm_get_total_scanned();
    ksm_scan_cycle();
    return (int)(ksm_get_total_scanned() - (uint64_t)before);
}

/* ── Stub: ksm_merge_page ───────────────────────────────────── */
static int ksm_merge_page(uint64_t phys_addr)
{
    if (phys_addr == 0 || (phys_addr & 0xFFF) != 0) {
        kprintf("[ksm] ksm_merge_page: invalid phys 0x%llx\n", (unsigned long long)phys_addr);
        return -EINVAL;
    }
    kprintf("[ksm] ksm_merge_page: merging page 0x%llx\n", (unsigned long long)phys_addr);
    return ksm_register_region_legacy((uint64_t)PHYS_TO_VIRT(phys_addr), 4096);
}

/* ── Stub: ksm_unmerge_page ─────────────────────────────────── */
static int ksm_unmerge_page(uint64_t phys_addr)
{
    if (phys_addr == 0 || (phys_addr & 0xFFF) != 0) {
        kprintf("[ksm] ksm_unmerge_page: invalid address 0x%llx\n", (unsigned long long)phys_addr);
        return -EINVAL;
    }
    kprintf("[ksm] ksm_unmerge_page: unmerging page 0x%llx\n", (unsigned long long)phys_addr);
    return ksm_unregister_region((uint64_t)PHYS_TO_VIRT(phys_addr));
}

/* ── Stub: ksm_check_stable_tree ────────────────────────────── */
static int ksm_check_stable_tree(void)
{
    kprintf("[ksm] ksm_check_stable_tree: checking %d tracked pages\n", ksm_get_page_count());
    ksm_scan_cycle();
    return 0;
}
