/*
 * src/lib/bitmap.c — Bitmap manipulation API
 *
 * Bitmaps are represented as arrays of unsigned long, providing efficient
 * bit-level operations for resource tracking (memory pages, inode slots,
 * file descriptors, etc.). Each unsigned long holds 8*sizeof(long) bits,
 * indexed linearly from bit 0 (LSB of word 0) to bit nbits-1.
 *
 * Memory order: bits are stored in native word order.  Bit 0 is the
 * least-significant bit of word 0; bit 8*sizeof(long) is the
 * least-significant bit of word 1.
 *
 * All functions operate on pre-allocated bitmap storage.  The bitmap_alloc
 * and bitmap_free helpers manage dynamic allocation.
 */

#include "bitmap.h"
#include "string.h"
#include "heap.h"

/*
 * bitmap_zero — Clear all bits in a bitmap
 * @dst:    pointer to the bitmap array
 * @nbits:  number of valid bits
 *
 * Fills the underlying storage with zero bytes.  Only the first ceil(nbits/8)
 * bytes are written regardless of array length.
 */
void bitmap_zero(unsigned long *dst, int nbits) { memset(dst, 0, (nbits + 7) / 8); }

/*
 * bitmap_set — Set a contiguous range of bits
 * @map:   pointer to the bitmap array
 * @start: first bit index (inclusive)
 * @nr:    number of bits to set
 *
 * Sets bits [start, start+nr) in the bitmap.  No bounds checking is
 * performed — the caller must ensure start+nr <= available bits.
 */
void bitmap_set(unsigned long *map, int start, int nr) {
    for (int i = start; i < start + nr; i++) map[i / (8*sizeof(long))] |= (1UL << (i % (8*sizeof(long))));
}

/*
 * bitmap_clear — Clear a contiguous range of bits
 * @map:   pointer to the bitmap array
 * @start: first bit index (inclusive)
 * @nr:    number of bits to clear
 *
 * Clears bits [start, start+nr) in the bitmap.  No bounds checking is
 * performed — the caller must ensure start+nr <= available bits.
 */
void bitmap_clear(unsigned long *map, int start, int nr) {
    for (int i = start; i < start + nr; i++) map[i / (8*sizeof(long))] &= ~(1UL << (i % (8*sizeof(long))));
}

/*
 * bitmap_find_next_zero_area — Find a contiguous region of zero bits
 * @map:   pointer to the bitmap array
 * @size:  total number of bits in the bitmap
 * @start: bit index to begin searching
 * @nr:    number of contiguous zero bits required
 *
 * Scans forward from @start looking for @nr consecutive zero bits.
 * Returns the starting index of the first such region, or -1 if no
 * suitable area exists.  This is an O(n * nr) scan suitable for
 * infrequent allocation; use a more sophisticated allocator for
 * hot paths.
 */
int bitmap_find_next_zero_area(unsigned long *map, int size, int start, int nr) {
    for (int i = start; i < size - nr + 1; i++) {
        int found = 1;
        for (int j = 0; j < nr; j++) if (map[(i+j) / (8*sizeof(long))] & (1UL << ((i+j) % (8*sizeof(long))))) { found = 0; break; }
        if (found) return i;
    }
    return -1;
}

/*
 * bitmap_alloc — Allocate and zero-initialize a bitmap
 * @nbits:  number of bits the bitmap must hold
 *
 * Returns a pointer to zeroed storage sized to hold @nbits bits,
 * or NULL on allocation failure.  The caller should cast the
 * returned void pointer to unsigned long *.
 *
 * The allocated bitmap is word-aligned via kmalloc.
 */
void* bitmap_alloc(int nbits)
{
    size_t bytes = (nbits + 7) / 8;
    void *p = kmalloc(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

/*
 * bitmap_free — Release a bitmap allocated with bitmap_alloc
 * @bitmap:  pointer returned by bitmap_alloc (may be NULL)
 *
 * Frees the bitmap storage.  Safe to call with a NULL pointer.
 * The caller must ensure no concurrent access during the free.
 */
int bitmap_free(void *bitmap)
{
    if (bitmap) kfree(bitmap);
    return 0;
}

/*
 * bitmap_parselist — Parse a human-readable bitmap list
 * @buf:    NUL-terminated string (e.g. "0-3,7,10-15")
 * @bitmap: destination bitmap (will be zeroed before parsing)
 * @nbits:  number of valid bits
 *
 * Parses a comma/space-separated list of individual bit numbers
 * and ranges (start-end).  Out-of-range indices are clamped to
 * [0, nbits-1].  Returns 0 on success.
 *
 * Example input:  "0-2,5,8-10"  sets bits 0,1,2,5,8,9,10.
 */
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
