#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "kasan_light.h"
#include "vmm.h"
#include "pmm.h"
#include "errno.h"
#include "kernel.h"

/* Shadow memory mapping.
 * For every 8 bytes of kernel heap, we use 1 byte of shadow.
 * The shadow base is placed in kernel VMA space after the heap. */

/* We use a fixed shadow base address — far enough in the kernel VMA
 * that it doesn't collide with the heap or other mappings. */
#define KASAN_SHADOW_BASE  0xFFFFC00000000000ULL

/* Size of shadow region: covers the first 128MB of kernel heap */
#define KASAN_SHADOW_SIZE  (128 * 1024 * 1024)  /* covers 1GB of heap */
#define KASAN_SHADOW_PAGES (KASAN_SHADOW_SIZE / PAGE_SIZE)

static uint8_t *kasan_shadow = NULL;
static int kasan_enabled = 0;

/* Convert an address to its shadow address */
static inline uint8_t *kasan_addr_to_shadow(const void *addr)
{
    uint64_t a = (uint64_t)addr;
    /* Shift right by KASAN_GRANULE_SHIFT (3) and add shadow base */
    return (uint8_t *)(KASAN_SHADOW_BASE + (a >> 3));
}

void kasan_init(void)
{
    /* Allocate shadow memory pages */
    uint64_t shadow_phys = (uint64_t)pmm_alloc_frames(KASAN_SHADOW_PAGES);
    if (!shadow_phys) {
        kprintf("[!!] kasan: failed to allocate shadow memory\n");
        return;
    }

    /* Map shadow memory */
    for (uint64_t i = 0; i < KASAN_SHADOW_PAGES; i++) {
        uint64_t vaddr = KASAN_SHADOW_BASE + i * PAGE_SIZE;
        uint64_t phys = shadow_phys + i * PAGE_SIZE;
        if (vmm_map_page(vaddr, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE) != 0) {
            kprintf("[!!] kasan: failed to map shadow page %llu\n", (unsigned long long)i);
            return;
        }
    }

    kasan_shadow = (uint8_t *)KASAN_SHADOW_BASE;

    /* Initially poison everything */
    memset(kasan_shadow, KASAN_SHADOW_FREE, KASAN_SHADOW_SIZE);

    kasan_enabled = 1;
    kprintf("[OK] kasan_light: shadow memory at 0x%llx, %llu KB\n",
            (unsigned long long)KASAN_SHADOW_BASE,
            (unsigned long long)(KASAN_SHADOW_SIZE / 1024));
}

void kasan_poison(const void *addr, size_t size)
{
    if (!kasan_enabled)
        return;

    uint64_t a = (uint64_t)addr;
    uint64_t end = a + size;

    /* Align to KASAN_GRANULE_SIZE */
    a = a & ~(KASAN_GRANULE_SIZE - 1);
    end = (end + KASAN_GRANULE_SIZE - 1) & ~(KASAN_GRANULE_SIZE - 1);

    for (; a < end; a += KASAN_GRANULE_SIZE) {
        uint8_t *shadow = kasan_addr_to_shadow((const void *)a);
        *shadow = KASAN_SHADOW_FREE;
    }
    __asm__ volatile("mfence" : : : "memory");
}

void kasan_unpoison(const void *addr, size_t size)
{
    if (!kasan_enabled)
        return;

    uint64_t a = (uint64_t)addr;
    uint64_t end = a + size;

    a = a & ~(KASAN_GRANULE_SIZE - 1);
    end = (end + KASAN_GRANULE_SIZE - 1) & ~(KASAN_GRANULE_SIZE - 1);

    for (; a < end; a += KASAN_GRANULE_SIZE) {
        uint8_t *shadow = kasan_addr_to_shadow((const void *)a);
        *shadow = KASAN_SHADOW_ACCESS;
    }
    __asm__ volatile("mfence" : : : "memory");
}

int kasan_check(const void *addr, size_t size, int is_write)
{
    (void)is_write; /* We don't distinguish read vs write for now */

    if (!kasan_enabled)
        return 0;

    uint64_t a = (uint64_t)addr;
    uint64_t end = a + size;

    /* Check each granule */
    for (; a < end; a += KASAN_GRANULE_SIZE) {
        uint8_t *shadow = kasan_addr_to_shadow((const void *)a);
        if (*shadow != KASAN_SHADOW_ACCESS) {
            /* Report the violation */
            kprintf("[!!] KASAN: invalid access at 0x%llx (shadow=0x%02x, %s)\n",
                    (unsigned long long)a, *shadow,
                    is_write ? "write" : "read");
            return -EFAULT;
        }
    }
    return 0;
}

void kasan_alloc(const void *addr, size_t size)
{
    kasan_unpoison(addr, size);
}

void kasan_free(const void *addr, size_t size)
{
    kasan_poison(addr, size);
}
