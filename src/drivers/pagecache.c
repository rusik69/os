#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "pagecache.h"
#include "string.h"
#include "spinlock.h"
#define CACHE_PAGES 256
static struct {
    uint64_t block;
    uint8_t data[4096];
    int valid;
} page_cache[CACHE_PAGES];
static spinlock_t pc_lock;
void pagecache_init(void) {
    spinlock_init(&pc_lock);
    memset(page_cache, 0, sizeof(page_cache));
    kprintf("[OK] Page cache initialized (%d slots)\n", CACHE_PAGES);
}
int pagecache_lookup(uint64_t block, uint8_t *buf) {
    if (!buf) return -1;
    spinlock_acquire(&pc_lock);
    for (int i = 0; i < CACHE_PAGES; i++) {
        if (page_cache[i].valid && page_cache[i].block == block) {
            memcpy(buf, page_cache[i].data, 4096);
            spinlock_release(&pc_lock);
            return 0;
        }
    }
    spinlock_release(&pc_lock);
    return -1;
}
void pagecache_store(uint64_t block, const uint8_t *data) {
    if (!data) return;
    spinlock_acquire(&pc_lock);
    int slot = block % CACHE_PAGES;
    page_cache[slot].block = block;
    page_cache[slot].valid = 1;
    memcpy(page_cache[slot].data, data, 4096);
    spinlock_release(&pc_lock);
}
