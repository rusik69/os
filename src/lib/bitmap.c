#include "bitmap.h"
#include "string.h"
void bitmap_zero(unsigned long *dst, int nbits) { memset(dst, 0, (nbits + 7) / 8); }
void bitmap_set(unsigned long *map, int start, int nr) {
    for (int i = start; i < start + nr; i++) map[i / (8*sizeof(long))] |= (1UL << (i % (8*sizeof(long))));
}
void bitmap_clear(unsigned long *map, int start, int nr) {
    for (int i = start; i < start + nr; i++) map[i / (8*sizeof(long))] &= ~(1UL << (i % (8*sizeof(long))));
}
int bitmap_find_next_zero_area(unsigned long *map, int size, int start, int nr) {
    for (int i = start; i < size - nr + 1; i++) {
        int found = 1;
        for (int j = 0; j < nr; j++) if (map[(i+j) / (8*sizeof(long))] & (1UL << ((i+j) % (8*sizeof(long))))) { found = 0; break; }
        if (found) return i;
    }
    return -1;
}

/* ── Stub: bitmap_alloc ─────────────────────────────── */
void* bitmap_alloc(int nbits)
{
    (void)nbits;
    kprintf("[bitmap] bitmap_alloc: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: bitmap_free ─────────────────────────────── */
int bitmap_free(void *bitmap)
{
    (void)bitmap;
    kprintf("[bitmap] bitmap_free: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: bitmap_parselist ─────────────────────────────── */
int bitmap_parselist(const char *buf, void *bitmap, int nbits)
{
    (void)buf;
    (void)bitmap;
    (void)nbits;
    kprintf("[bitmap] bitmap_parselist: not yet implemented\n");
    return -ENOSYS;
}
