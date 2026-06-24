/*
 * idr.c — Integer ID management
 *
 * Allocates and releases integer IDs from a bitmap-based allocator.
 * Uses __builtin_ctzll for O(1) allocation.
 */

#include "idr.h"
#include "heap.h"
#include "string.h"
#include "printf.h"

int idr_init(struct idr *idr, int max) {
    if (!idr || max <= 0) return -1;

    int nwords = (max + 63) / 64;
    idr->bitmap = (uint64_t *)kmalloc((size_t)nwords * sizeof(uint64_t));
    if (!idr->bitmap) return -1;

    memset(idr->bitmap, 0, (size_t)nwords * sizeof(uint64_t));
    idr->max = max;
    idr->nwords = nwords;
    return 0;
}

int idr_alloc(struct idr *idr) {
    if (!idr || !idr->bitmap) return -1;

    for (int w = 0; w < idr->nwords; w++) {
        if (idr->bitmap[w] == ~0ULL) continue;
        int bit = __builtin_ctzll(~idr->bitmap[w]);
        int id = w * 64 + bit;
        if (id >= idr->max) break;
        idr->bitmap[w] |= (1ULL << bit);
        return id;
    }
    return -1; /* no free IDs */
}

void idr_remove(struct idr *idr, int id) {
    if (!idr || !idr->bitmap || id < 0 || id >= idr->max) return;
    int w = id / 64;
    int bit = id % 64;
    idr->bitmap[w] &= ~(1ULL << bit);
}

int idr_find(struct idr *idr, int id) {
    if (!idr || !idr->bitmap || id < 0 || id >= idr->max) return 0;
    int w = id / 64;
    int bit = id % 64;
    return idr->bitmap[w] & (1ULL << bit);
}

/* ── Stub: idr_destroy ─────────────────────────────── */
int idr_destroy(void *idr)
{
    (void)idr;
    kprintf("[idr] idr_destroy: not yet implemented\n");
    return 0;
}
