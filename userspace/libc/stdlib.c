/* Minimal stdlib: malloc (sbrk-style), atoi, free (no-op) */

#include "stdlib.h"
#include "unistd.h"
#include "string.h"

/* Heap grows upward from a base address obtained via brk().
 * We use a simple bump allocator with a free list for reuse. */

#define HEAP_MIN     (4 * 4096)    /* 16 KB initial */
#define HEAP_CHUNK   (4 * 4096)    /* 4 KB growth chunks */

/* Minimum allocation size for free-list bookkeeping */
#define MALLOC_ALIGN 16
#define MALLOC_MIN   32

typedef struct free_hdr {
    unsigned long size;        /* total size including header */
    struct free_hdr *next;
} free_hdr_t;

static free_hdr_t *free_list = NULL;
static unsigned long heap_start = 0;
static unsigned long heap_end = 0;
static unsigned long heap_max = 0;

/* Align size up to MALLOC_ALIGN */
static unsigned long align_up(unsigned long sz) {
    return (sz + MALLOC_ALIGN - 1) & ~(MALLOC_ALIGN - 1);
}

/* Ensure we have at least 'need' bytes above heap_end */
static int grow_heap(unsigned long need) {
    if (heap_start == 0) {
        /* First call: get initial heap base */
        heap_start = (unsigned long)brk((void *)0);
        if (heap_start == (unsigned long)-1)
            return -1;
        heap_end = heap_start;
        heap_max = heap_start;
    }

    /* Round up need */
    unsigned long want = align_up(need);
    if (want < HEAP_CHUNK) want = HEAP_CHUNK;

    /* Try to extend */
    unsigned long new_end = heap_end + want;
    if (brk((void *)new_end) < 0) {
        /* brk failed — try exact amount */
        new_end = heap_end + align_up(need);
        if (brk((void *)new_end) < 0)
            return -1;
    }

    heap_end = new_end;
    if (heap_end > heap_max)
        heap_max = heap_end;
    return 0;
}

void *malloc(unsigned long size) {
    if (size == 0) size = 1;

    unsigned long total = align_up(size + sizeof(free_hdr_t));
    if (total < MALLOC_MIN) total = MALLOC_MIN;

    /* Search free list for a suitable block */
    free_hdr_t **prev = &free_list;
    free_hdr_t *curr = free_list;
    while (curr) {
        if (curr->size >= total) {
            /* Found a block — unlink */
            *prev = curr->next;
            /* Mark as allocated (size stays the same) */
            return (void *)(curr + 1);
        }
        prev = &curr->next;
        curr = curr->next;
    }

    /* No suitable free block — allocate from heap */
    if (heap_end - heap_start < total) {
        if (grow_heap(total) < 0)
            return NULL;
    }

    free_hdr_t *hdr = (free_hdr_t *)heap_end;
    hdr->size = total;
    heap_end += total;
    if (heap_end > heap_max)
        heap_max = heap_end;

    return (void *)(hdr + 1);
}

void free(void *ptr) {
    if (!ptr) return;

    free_hdr_t *hdr = (free_hdr_t *)ptr - 1;
    if (hdr->size == 0) return; /* double-free guard */

    /* Add to free list (sorted by address to aid coalescing) */
    free_hdr_t **prev = &free_list;
    free_hdr_t *curr = free_list;
    while (curr && (void *)curr < (void *)hdr) {
        prev = &curr->next;
        curr = curr->next;
    }
    hdr->next = curr;
    *prev = hdr;

    /* Simple coalesce with next if adjacent */
    if (curr && (void *)curr == (void *)hdr + hdr->size) {
        hdr->size += curr->size;
        hdr->next = curr->next;
    }
}

void *calloc(unsigned long nmemb, unsigned long size) {
    unsigned long total = nmemb * size;
    if (total == 0) return NULL;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, unsigned long size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    free_hdr_t *hdr = (free_hdr_t *)ptr - 1;
    unsigned long old_size = hdr->size - sizeof(free_hdr_t);

    if (align_up(size + sizeof(free_hdr_t)) <= hdr->size)
        return ptr; /* fits in current block */

    void *new_ptr = malloc(size);
    if (new_ptr) {
        unsigned long copy = old_size < size ? old_size : size;
        memcpy(new_ptr, ptr, copy);
        free(ptr);
    }
    return new_ptr;
}

int atoi(const char *s) {
    int val = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

void abort(void) {
    /* Force exit with error */
    for (;;) { write(STDERR_FILENO, "Abort\n", 6); exit(1); }
}
