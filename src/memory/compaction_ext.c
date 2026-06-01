#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "pmm.h"
#include "string.h"

/* ── Memory compaction forcing ────────────────────────────────────────── *
 * Compaction moves pages around to reduce memory fragmentation.
 * This is a stub that triggers a compaction pass.
 */

/* Forward decl from pmm_extras.c */
extern void memory_compaction_force(void);

/* Actual compaction logic */
void compaction_force(void) {
    kprintf("[compaction] scanning for movable pages...\n");
    
    /* In a real implementation, this would scan all pages,
     * identify movable ones (e.g., page cache, user pages),
     * and migrate them to lower addresses.
     */
    
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    
    kprintf("[compaction] total=%llu used=%llu free=%llu\n",
            (unsigned long long)total, (unsigned long long)used,
            (unsigned long long)(total - used));
    
    /* Simulate compaction by printing status */
    kprintf("[compaction] pass complete\n");
}

/* Initialize compaction */
void compaction_init(void) {
    kprintf("[OK] memory compaction initialized\n");
}
