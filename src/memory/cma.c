/* cma.c — Contiguous Memory Allocator */

#include "cma.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"

static struct cma_area cma_areas[CMA_MAX_AREAS];
static int cma_area_count = 0;
static int cma_initialized = 0;

void cma_init(void) {
    memset(cma_areas, 0, sizeof(cma_areas));
    cma_area_count = 0;
    cma_initialized = 1;
    kprintf("[mem] CMA (Contiguous Memory Allocator) initialized\n");
}

int cma_create_area(uint64_t base_pfn, uint64_t count, const char *name) {
    if (!cma_initialized) return -1;
    if (cma_area_count >= CMA_MAX_AREAS) return -1;

    struct cma_area *area = &cma_areas[cma_area_count];
    area->base_pfn = base_pfn;
    area->count = count;

    /* Allocate bitmap: one bit per page */
    area->bitmap_size = (count + 7) / 8;
    area->bitmap = (uint64_t *)pmm_alloc_frames((area->bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!area->bitmap) return -1;

    memset(area->bitmap, 0, area->bitmap_size);
    strncpy(area->name, name, sizeof(area->name) - 1);
    area->name[sizeof(area->name) - 1] = '\0';
    area->initialized = 1;

    cma_area_count++;
    kprintf("[mem] CMA area '%s': base_pfn=0x%lx, count=%lu pages (%lu MB)\n",
            name, (unsigned long)base_pfn, (unsigned long)count,
            (unsigned long)(count * 4 / 1024));
    return 0;
}

static int cma_find_area(const char *name) {
    for (int i = 0; i < cma_area_count; i++) {
        if (strcmp(cma_areas[i].name, name) == 0)
            return i;
    }
    return -1;
}

static inline void cma_bitmap_set(struct cma_area *area, uint64_t idx) {
    area->bitmap[idx / 64] |= (1ULL << (idx % 64));
}

static inline void cma_bitmap_clear(struct cma_area *area, uint64_t idx) {
    area->bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

static inline int cma_bitmap_test(struct cma_area *area, uint64_t idx) {
    return (area->bitmap[idx / 64] >> (idx % 64)) & 1;
}

uint64_t cma_alloc(const char *name, size_t count, size_t align) {
    int idx = cma_find_area(name);
    if (idx < 0) return 0;

    struct cma_area *area = &cma_areas[idx];
    size_t i = 0;
    size_t found = 0;
    uint64_t start = 0;

    /* Simple linear scan for contiguous free region */
    while (i < area->count) {
        if (!cma_bitmap_test(area, i)) {
            if (found == 0) {
                /* Check alignment */
                if (align > 1 && ((area->base_pfn + i) % align != 0)) {
                    i++;
                    continue;
                }
                start = i;
            }
            found++;
            if (found == count) {
                /* Allocate */
                for (size_t j = start; j < start + count; j++) {
                    cma_bitmap_set(area, j);
                }
                uint64_t pfn = area->base_pfn + start;
                kprintf("[mem] CMA alloc '%s': %lu pages at PFN 0x%lx\n",
                        name, (unsigned long)count, (unsigned long)pfn);
                return pfn;
            }
        } else {
            found = 0;
        }
        i++;
    }
    return 0;
}

void cma_free(uint64_t pfn, size_t count) {
    for (int i = 0; i < cma_area_count; i++) {
        struct cma_area *area = &cma_areas[i];
        if (pfn >= area->base_pfn && pfn < area->base_pfn + area->count) {
            size_t offset = pfn - area->base_pfn;
            for (size_t j = offset; j < offset + count && j < area->count; j++) {
                cma_bitmap_clear(area, j);
            }
            kprintf("[mem] CMA free '%s': %lu pages at PFN 0x%lx\n",
                    area->name, (unsigned long)count, (unsigned long)pfn);
            return;
        }
    }
}

uint64_t cma_get_total_pages(const char *name) {
    int idx = cma_find_area(name);
    if (idx < 0) return 0;
    return cma_areas[idx].count;
}

uint64_t cma_get_free_pages(const char *name) {
    int idx = cma_find_area(name);
    if (idx < 0) return 0;
    struct cma_area *area = &cma_areas[idx];
    uint64_t free = 0;
    for (size_t i = 0; i < area->count; i++) {
        if (!cma_bitmap_test(area, i)) free++;
    }
    return free;
}

void cma_reserve_default(void) {
    /* Reserve 32 MB for CMA at a high physical address.
     * In a real kernel this would come from the command line. */
    uint64_t total = pmm_get_total_frames();
    uint64_t reserve_size = (32ULL * 1024 * 1024) / PAGE_SIZE; /* 32 MB */
    if (reserve_size > total / 4) reserve_size = total / 4;

    uint64_t base_pfn = total - reserve_size;
    cma_create_area(base_pfn, reserve_size, "default");
}

/* ── cma_release ──────────────────────────────────────── */
int cma_release(uint64_t pfn, size_t count)
{
    if (count == 0) return -EINVAL;
    /* Return CMA-allocated pages back to CMA pool */
    cma_free(pfn, count);
    kprintf("[cma] cma_release: released %zu pages at PFN 0x%llx\n",
            count, (unsigned long long)pfn);
    return 0;
}

/* ── cma_isolate_page ─────────────────────────────────── */
int cma_isolate_page(uint64_t pfn)
{
    /* Isolate a page from CMA area for migration.
     * In a real kernel, this marks the page as MIGRATE_ISOLATE. */
    kprintf("[cma] cma_isolate_page: isolating PFN 0x%llx\n",
            (unsigned long long)pfn);
    /* Check if this PFN falls within any CMA area */
    for (int i = 0; i < cma_area_count; i++) {
        struct cma_area *area = &cma_areas[i];
        if (pfn >= area->base_pfn && pfn < area->base_pfn + area->count) {
            size_t offset = pfn - area->base_pfn;
            if (!cma_bitmap_test(area, offset)) {
                kprintf("[cma] cma_isolate_page: PFN 0x%llx already free\n",
                        (unsigned long long)pfn);
                return -EINVAL;
            }
            return 0;
        }
    }
    kprintf("[cma] cma_isolate_page: PFN 0x%llx not in any CMA area\n",
            (unsigned long long)pfn);
    return -ENOENT;
}

/* ── cma_free_page — Free a single CMA page ──────────────── */
void cma_free_page(uint64_t pfn)
{
    /* Free a single page back to its CMA area */
    for (int i = 0; i < cma_area_count; i++) {
        struct cma_area *area = &cma_areas[i];
        if (pfn >= area->base_pfn && pfn < area->base_pfn + area->count) {
            size_t offset = pfn - area->base_pfn;
            if (cma_bitmap_test(area, offset)) {
                cma_bitmap_clear(area, offset);
                kprintf("[cma] cma_free_page: freed PFN 0x%llx in area '%s'\n",
                        (unsigned long long)pfn, area->name);
            }
            return;
        }
    }
    kprintf("[cma] cma_free_page: PFN 0x%llx not found in any CMA area\n",
            (unsigned long long)pfn);
}

/* ── cma_debug_show_areas — Display all CMA areas ────────── */
void cma_debug_show_areas(void)
{
    kprintf("[cma] CMA areas: %d total\n", cma_area_count);
    for (int i = 0; i < cma_area_count; i++) {
        struct cma_area *area = &cma_areas[i];
        uint64_t free_pages = 0;
        for (size_t j = 0; j < area->count; j++) {
            if (!cma_bitmap_test(area, j))
                free_pages++;
        }
        uint64_t used_pages = area->count - free_pages;
        kprintf("  [%d] '%s': base=0x%llx pages=%llu used=%llu free=%llu\n",
                i, area->name,
                (unsigned long long)area->base_pfn,
                (unsigned long long)area->count,
                (unsigned long long)used_pages,
                (unsigned long long)free_pages);
    }
}
