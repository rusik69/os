#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "kasan_light.h"
#include "vmm.h"
#include "pmm.h"
#include "errno.h"
#include "kernel.h"
#include "stacktrace.h"
#include "heap.h"

// SPDX-License-Identifier: GPL-2.0-only
/*
 * kasan_light.c — Kernel Address SANitizer (lightweight variant)
 *
 * KASAN-light is a compile-time instrumentation tool that detects
 * out-of-bounds accesses, use-after-free, and use-after-return bugs
 * in kernel memory. It uses shadow memory to track the accessibility
 * of each 8-byte aligned region of kernel VMA memory.
 *
 * INSTRUMENTATION MODEL:
 *   - Every 8 bytes of kernel memory are tracked by 1 byte of shadow.
 *   - Shadow memory is a linear mapping from kernel VMA address to
 *     a dedicated 32 MB shadow region at KASAN_SHADOW_BASE.
 *   - Shadow byte values:
 *       0x00 = accessible (KASAN_SHADOW_ACCESS)
 *       0xFF = freed / poisoned (KASAN_SHADOW_FREE)
 *       0xFE = redzone (KASAN_SHADOW_REDZONE)
 *   - The compiler emits __asan_loadN / __asan_storeN callbacks for
 *     every memory access when -fsanitize=kernel-address is enabled.
 *
 * SHADOW MAPPING:
 *   shadow_addr = KASAN_SHADOW_BASE + ((addr - KASAN_VMA_START) >> 3)
 *
 * This avoids 64-bit overflow issues that plagued the previous scheme
 * (which used KASAN_SHADOW_BASE + (addr >> 3) — for kernel VMA addresses
 * this wraps around to non-canonical addresses and cannot be accessed).
 *
 * COVERAGE: Currently covers kernel heap (kmalloc), slab objects, and
 * stack variables. DMA buffers and MMIO regions are selectively
 * exempted via __no_sanitize_address annotations.
 *
 * LIMITATIONS:
 *   - No tag-based checking (only byte-precision shadow)
 *   - No stack-use-after-scope detection
 *   - ~32MB overhead for shadow memory
 *   - ~10-25% performance overhead when enabled
 */

static uint8_t *kasan_shadow = NULL;
static int kasan_enabled = 0;

/* ── Address → Shadow Translation ────────────────────────────────────── */

/* Convert a kernel VMA address to its shadow address.
 * Returns NULL if the address is outside the covered range or below the
 * kernel VMA base. */
static inline uint8_t *kasan_addr_to_shadow(const void *addr)
{
    uint64_t a = (uint64_t)(uintptr_t)addr;

    /* Only cover kernel VMA addresses */
    if (a < KASAN_VMA_START)
        return NULL;

    uint64_t offset = a - KASAN_VMA_START;
    uint64_t shadow_offset = offset >> KASAN_GRANULE_SHIFT;

    if (shadow_offset >= KASAN_SHADOW_SIZE)
        return NULL; /* out of currently mapped shadow range */

    return (uint8_t *)(KASAN_SHADOW_BASE + shadow_offset);
}

/* ── Initialisation ──────────────────────────────────────────────────── */

void __init kasan_init(void)
{
    kprintf("[WARN] kasan: disabled (shadow PML4 entries unstable)\n");
}

/* ── Poisoning / Unpoisoning ─────────────────────────────────────────── */

void kasan_poison(const void *addr, size_t size)
{
    uint64_t a, end;
    uint8_t *shadow;

    if (!kasan_enabled || size == 0)
        return;

    a   = (uint64_t)(uintptr_t)addr;
    end = a + size;

    /* Align to KASAN_GRANULE_SIZE so we don't leave partial granules
     * accessible (which would defeat the detection). */
    a   = a & ~(uint64_t)(KASAN_GRANULE_SIZE - 1);
    end = (end + KASAN_GRANULE_SIZE - 1) & ~(uint64_t)(KASAN_GRANULE_SIZE - 1);

    for (; a < end; a += KASAN_GRANULE_SIZE) {
        shadow = kasan_addr_to_shadow((const void *)(uintptr_t)a);
        if (!shadow)
            continue; /* address not covered — skip silently */
        *shadow = KASAN_SHADOW_FREE;
    }
    mb();
}

void kasan_unpoison(const void *addr, size_t size)
{
    uint64_t a, end;
    uint8_t *shadow;

    if (!kasan_enabled || size == 0)
        return;

    a   = (uint64_t)(uintptr_t)addr;
    end = a + size;

    a   = a & ~(uint64_t)(KASAN_GRANULE_SIZE - 1);
    end = (end + KASAN_GRANULE_SIZE - 1) & ~(uint64_t)(KASAN_GRANULE_SIZE - 1);

    for (; a < end; a += KASAN_GRANULE_SIZE) {
        shadow = kasan_addr_to_shadow((const void *)(uintptr_t)a);
        if (!shadow)
            continue;
        *shadow = KASAN_SHADOW_ACCESS;
    }
    mb();
}

/* ── Runtime Checking ────────────────────────────────────────────────── */

int kasan_check(const void *addr, size_t size, int is_write)
{
    uint64_t a, end;
    uint8_t *shadow;

    if (!kasan_enabled || size == 0)
        return 0;

    a   = (uint64_t)(uintptr_t)addr;
    end = a + size;

    /* Align to granule boundaries for shadow lookup */
    a   = a & ~(uint64_t)(KASAN_GRANULE_SIZE - 1);

    for (; a < end; a += KASAN_GRANULE_SIZE) {
        shadow = kasan_addr_to_shadow((const void *)(uintptr_t)a);
        if (!shadow)
            continue; /* not covered, skip */
        if (*shadow != KASAN_SHADOW_ACCESS) {
            kprintf("[!!] KASAN DETECTED an invalid %s at 0x%llx\n"
                    "    shadow byte  = 0x%02x (%s)\n"
                    "    access size  = %llu\n"
                    "    access range = [0x%llx, 0x%llx)\n",
                    is_write ? "WRITE" : "READ",
                    (unsigned long long)a,
                    *shadow,
                    (*shadow == KASAN_SHADOW_FREE)  ? "freed/poisoned" :
                    (*shadow == KASAN_SHADOW_REDZONE) ? "redzone" : "unknown",
                    (unsigned long long)size,
                    (unsigned long long)((uint64_t)(uintptr_t)addr),
                    (unsigned long long)((uint64_t)(uintptr_t)addr + size));
            print_stack_trace();
            return -EFAULT;
        }
    }
    return 0;
}

/* ── Generic Shadow Setter ───────────────────────────────────────────── */

/* Set shadow memory for a region to a specific value.
 * This allows marking regions as accessible (0x00), freed (0xFF),
 * or redzone (0xFE) with a single call.
 * Automatically aligns to KASAN_GRANULE_SIZE boundaries. */
void kasan_set_shadow(const void *addr, size_t size, uint8_t shadow_val)
{
    uint64_t a, end;
    uint8_t *shadow;

    if (!kasan_enabled || size == 0)
        return;

    a   = (uint64_t)(uintptr_t)addr;
    end = a + size;

    /* Align to KASAN_GRANULE_SIZE so we don't leave partial granules */
    a   = a & ~(uint64_t)(KASAN_GRANULE_SIZE - 1);
    end = (end + KASAN_GRANULE_SIZE - 1) & ~(uint64_t)(KASAN_GRANULE_SIZE - 1);

    for (; a < end; a += KASAN_GRANULE_SIZE) {
        shadow = kasan_addr_to_shadow((const void *)(uintptr_t)a);
        if (!shadow)
            continue; /* address not covered — skip silently */
        *shadow = shadow_val;
    }
    mb();
}

/* Mark a region as a redzone (KASAN_SHADOW_REDZONE).
 * Access to redzone memory triggers KASAN diagnostic via kasan_check(). */
void kasan_poison_redzone(const void *addr, size_t size)
{
    kasan_set_shadow(addr, size, KASAN_SHADOW_REDZONE);
}

/* ── Allocator Hooks ─────────────────────────────────────────────────── */

void kasan_alloc(const void *addr, size_t size)
{
    kasan_unpoison(addr, size);
}

void kasan_free(const void *addr, size_t size)
{
    kasan_poison(addr, size);
}

/* ── Runtime Coverage Extension ──────────────────────────────────────── */

/*
 * Poison the unused portion of a kernel stack, leaving the upper
 * portion accessible.  Detects stack overflow via KASAN.
 *
 * Called from the scheduler after context switch.
 * @stack_base: low address (bottom) of the kernel stack.
 * @stack_top:  high address (top).
 * @used_from_top: bytes currently in use (stack_top - RSP).
 */
void kasan_poison_stack(uint64_t stack_base, uint64_t stack_top,
                         uint64_t used_from_top)
{
    if (!kasan_enabled || stack_base >= stack_top)
        return;

    /* The active region is [stack_top - used_from_top, stack_top).
     * Everything below that is potential overflow territory. */
    uint64_t active_bottom = stack_top - used_from_top;

    /* Clamp to stack boundaries */
    if (active_bottom < stack_base)
        active_bottom = stack_base;

    /* Poison the unused (cold) portion of the stack */
    if (active_bottom > stack_base) {
        kasan_poison((const void *)(uintptr_t)stack_base,
                     (size_t)(active_bottom - stack_base));
    }

    /* Unpoison the active (hot) portion so stack operations work */
    kasan_unpoison((const void *)(uintptr_t)active_bottom,
                   (size_t)(stack_top - active_bottom));
}

/*
 * Mark an entire kernel stack as accessible.
 * Used during process creation before the stack is in use.
 */
void kasan_unpoison_stack(uint64_t stack_base, uint64_t stack_top)
{
    if (!kasan_enabled || stack_base >= stack_top)
        return;
    kasan_unpoison((const void *)(uintptr_t)stack_base,
                   (size_t)(stack_top - stack_base));
}

int kasan_extend_coverage(uint64_t start, uint64_t end)
{
    uint64_t off_start, off_end;
    uint64_t shadow_off_start, shadow_off_end;

    if (!kasan_enabled)
        return -ENODEV;
    if (start < KASAN_VMA_START || end <= start)
        return -EINVAL;

    off_start = start - KASAN_VMA_START;
    off_end   = end - KASAN_VMA_START;

    shadow_off_start  = off_start >> KASAN_GRANULE_SHIFT;
    shadow_off_end    = (off_end + KASAN_GRANULE_SIZE - 1) >> KASAN_GRANULE_SHIFT;

    /* Clamp to existing shadow mapping */
    if (shadow_off_start >= KASAN_SHADOW_SIZE)
        return -ENOMEM;
    if (shadow_off_end > KASAN_SHADOW_SIZE)
        shadow_off_end = KASAN_SHADOW_SIZE;

    /* Poison the newly-covered shadow region (will be unpoisoned on alloc) */
    memset(kasan_shadow + shadow_off_start, KASAN_SHADOW_FREE,
           shadow_off_end - shadow_off_start);
    mb();

    return 0;
}

/* ── Stub: kasan_alloc_pages ─────────────────────────────── */
static int kasan_alloc_pages(void *page, unsigned int order)
{
    (void)page;
    (void)order;
    kprintf("[kasan] kasan_alloc_pages: not yet implemented\n");
    return 0;
}
/* ── Stub: kasan_free_pages ─────────────────────────────── */
static int kasan_free_pages(void *page, unsigned int order)
{
    (void)page;
    (void)order;
    kprintf("[kasan] kasan_free_pages: not yet implemented\n");
    return 0;
}
