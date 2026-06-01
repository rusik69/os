#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "process.h"
#include "pmm.h"

/* ── Page poisoning sysctl ────────────────────────────────────────────── *
 * Allows runtime control of page poisoning via sysctl.
 * Page poisoning fills freed pages with 0xDC and allocated pages with 0xDEADBEEF.
 * This is already implemented in pmm.c but now exposed via sysctl.
 */

/* Sysctl: get page poison state */
int sysctl_page_poison_get(void) {
    return pmm_poison_enabled;
}

/* Sysctl: set page poison state */
int sysctl_page_poison_set(int val) {
    pmm_set_poison(val ? 1 : 0);
    kprintf("[sysctl] page_poison set to %d\n", val ? 1 : 0);
    return 0;
}

/* ── Memory reclaim watermark ─────────────────────────────────────────── *
 * When free pages fall below watermark, kswapd-like reclaim is triggered.
 */

/* Watermark in pages (default: 64 pages = 256KB) */
static uint64_t reclaim_watermark = 64;

uint64_t pmm_get_reclaim_watermark(void) {
    return reclaim_watermark;
}

void pmm_set_reclaim_watermark(uint64_t pages) {
    reclaim_watermark = pages;
    kprintf("[pmm] reclaim watermark set to %llu pages\n", (unsigned long long)pages);
}

/* Check if memory is below watermark */
int pmm_below_watermark(void) {
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    uint64_t free_pages = total - used;
    return free_pages < reclaim_watermark;
}

/* ── Memory compaction forcing ───────────────────────────────────────── */

/* Force memory compaction — move pages to reduce fragmentation */
void compaction_force(void);

void memory_compaction_force(void) {
    kprintf("[compaction] forcing memory compaction...\n");
    compaction_force();
    kprintf("[compaction] compaction complete\n");
}

/* ── Memory reclaim watermark init ───────────────────────────────────── */

void pmm_watermark_init(void) {
    reclaim_watermark = 64;
    kprintf("[OK] pmm watermark initialized (%llu pages)\n", 
            (unsigned long long)reclaim_watermark);
}
