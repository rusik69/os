#include "bitmap.h"
#include "string.h"
#include "heap.h"
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

/* ── bitmap_alloc ─────────────────────────────── */
void* bitmap_alloc(int nbits)
{
    size_t bytes = (nbits + 7) / 8;
    void *p = kmalloc(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}
/* ── bitmap_free ─────────────────────────────── */
int bitmap_free(void *bitmap)
{
    if (bitmap) kfree(bitmap);
    return 0;
}
/* ── bitmap_parselist ─────────────────────────────── */
int bitmap_parselist(const char *buf, void *bitmap, int nbits)
{
    unsigned long *map = (unsigned long *)bitmap;
    bitmap_zero(map, nbits);
    if (!buf || !*buf) return 0;
    const char *p = buf;
    while (*p) {
        if (*p == ' ' || *p == ',') { p++; continue; }
        int start = 0, end = 0;
        while (*p >= '0' && *p <= '9') { start = start * 10 + (*p - '0'); p++; }
        if (*p == '-') {
            p++;
            while (*p >= '0' && *p <= '9') { end = end * 10 + (*p - '0'); p++; }
        } else {
            end = start;
        }
        if (start >= nbits) start = nbits - 1;
        if (end >= nbits) end = nbits - 1;
        for (int i = start; i <= end; i++)
            map[i / (8*sizeof(long))] |= (1UL << (i % (8*sizeof(long))));
    }
    return 0;
}
