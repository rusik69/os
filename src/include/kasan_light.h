#ifndef KASAN_LIGHT_H
#define KASAN_LIGHT_H

#include "types.h"

/* Enable/disable KASAN checking at runtime */
#define KASAN_ENABLED 1

/* Shadow memory granule: 1 byte of shadow per 8 bytes of kernel memory */
#define KASAN_GRANULE_SIZE  8
#define KASAN_GRANULE_SHIFT 3

/* Shadow values */
#define KASAN_SHADOW_FREE    0xFF  /* inaccessible (freed or poisoned) */
#define KASAN_SHADOW_ACCESS  0x00  /* accessible (allocated or unpoisoned) */
#define KASAN_SHADOW_REDZONE 0xFE  /* redzone padding (out-of-bounds) */

/* Kernel virtual memory base — shadow covers addresses starting from here.
 * Kernel uses a high-half VMA: physical frames are mapped at PHYS_TO_VIRT(phys).
 * Heap and kernel text/data also reside in this range. */
#define KASAN_VMA_START   KERNEL_VMA_OFFSET  /* 0xFFFF800000000000 */

/* Shadow memory base address (in canonical kernel space, after the VMA region).
 *
 * For any kernel address A (>= KASAN_VMA_START), the shadow byte is at:
 *   shadow = KASAN_SHADOW_BASE + ((A - KASAN_VMA_START) >> KASAN_GRANULE_SHIFT)
 *
 * This is a simple linear offset mapping — no 64-bit overflow issue.
 *
 * 32 MB of shadow covers 256 MB of kernel VMA starting at KASAN_VMA_START.
 * This covers: kernel text/data (~2 MB), heap (up to 64 MB), per-process
 * kernel stacks, and physical memory mappings (~190 MB). */
#define KASAN_SHADOW_BASE    0xFFFFD00000000000ULL
#define KASAN_SHADOW_SIZE    (32 * 1024 * 1024)   /* 32 MB shadow */
#define KASAN_SHADOW_PAGES   (KASAN_SHADOW_SIZE / PAGE_SIZE)  /* 8192 pages */
#define KASAN_SHADOW_COVERAGE  ((uint64_t)KASAN_SHADOW_SIZE << KASAN_GRANULE_SHIFT) /* 256 MB */

/* ─── Stack Redzone Macros ────────────────────────────────────────
 *
 * To detect stack buffer overflows, poison 8 bytes before and after a
 * stack-allocated buffer, then verify they remain intact after use.
 *
 * Usage:
 *   void my_func(void) {
 *       char buf[64];
 *       KASAN_STACK_REDZONE_POISON(buf, sizeof(buf));
 *       // ... use buf ...
 *       KASAN_STACK_REDZONE_VERIFY(buf, sizeof(buf));
 *       KASAN_STACK_REDZONE_UNPOISON(buf, sizeof(buf));
 *   }
 *
 * NOTES:
 *   - The buffer MUST have at least 8 bytes of valid memory below and above it
 *     (i.e., NOT at the very edge of the stack frame).
 *   - VERIFY must be called before UNPOISON (but after any write to buf).
 *   - In functions that return cleanly, UNPOISON is optional — the stack
 *     memory will be reused and re-poisoned on the next call.
 */

/* Stamp redzones around a stack buffer */
#define KASAN_STACK_REDZONE_POISON(buf, size) do {                                \
    kasan_poison((const void *)((uintptr_t)(buf) - KASAN_GRANULE_SIZE),           \
                 KASAN_GRANULE_SIZE);                                              \
    kasan_poison((const void *)((uintptr_t)(buf) + (size)),                        \
                 KASAN_GRANULE_SIZE);                                              \
} while (0)

/* Verify redzones are intact — triggers KASAN diagnostic + stack trace if not */
#define KASAN_STACK_REDZONE_VERIFY(buf, size) do {                                \
    if (kasan_check((const void *)((uintptr_t)(buf) - KASAN_GRANULE_SIZE),        \
                    KASAN_GRANULE_SIZE, 0) != 0) {                                 \
        kprintf("KASAN: stack redzone UNDERFLOW at buf=%p size=%zu func=%s\n",    \
                (void *)(buf), (size_t)(size), __func__);                          \
    }                                                                              \
    if (kasan_check((const void *)((uintptr_t)(buf) + (size)),                     \
                    KASAN_GRANULE_SIZE, 0) != 0) {                                 \
        kprintf("KASAN: stack redzone OVERFLOW at buf=%p size=%zu func=%s\n",     \
                (void *)(buf), (size_t)(size), __func__);                          \
    }                                                                              \
} while (0)

/* Restore accessibility to redzones (call before return) */
#define KASAN_STACK_REDZONE_UNPOISON(buf, size) do {                              \
    kasan_unpoison((const void *)((uintptr_t)(buf) - KASAN_GRANULE_SIZE),         \
                   KASAN_GRANULE_SIZE);                                            \
    kasan_unpoison((const void *)((uintptr_t)(buf) + (size)),                      \
                   KASAN_GRANULE_SIZE);                                            \
} while (0)

/* ─── Convenience Shortcut ────────────────────────────────────────
 * Combines all three steps: poison, (implied use by caller), verify, unpoison.
 * Wrap any code that touches the buffer between POISON and the matching
 * VERIFY/UNPOISON.  For simple functions, use the three-macro sequence above
 * with the actual buffer operations in between. */

/* ── Stack memory management (for kernel stack overflow detection) ─── */

/*
 * Poison the unused portion of a kernel stack, leaving the upper
 * portion accessible.  Detects stack overflow via KASAN.
 *
 * @stack_base: low address (bottom) of the kernel stack.
 * @stack_top:  high address (top).
 * @used_from_top: bytes currently in use (RSP - stack_base).
 */
void kasan_poison_stack(uint64_t stack_base, uint64_t stack_top,
                         uint64_t used_from_top);

/*
 * Mark an entire kernel stack as accessible.
 * Used during process creation before the stack is in use.
 */
void kasan_unpoison_stack(uint64_t stack_base, uint64_t stack_top);

/* ─── Function Declarations ─────────────────────────────────────── */

/* Initialize shadow memory region for the kernel.
 * Must be called after heap_init() and VMM init. */
void kasan_init(void);

/* Mark a memory region as poisoned (inaccessible).  addr and size are
 * automatically aligned to KASAN_GRANULE_SIZE boundaries. */
void kasan_poison(const void *addr, size_t size);

/* Mark a memory region as accessible. */
void kasan_unpoison(const void *addr, size_t size);

/* Check if a memory region is accessible.
 * Returns 0 if OK (all bytes accessible), -EFAULT if poisoned.
 * Prints a diagnostic message and kernel stack trace on violation. */
int kasan_check(const void *addr, size_t size, int is_write);

/* Convenience: unpoison after kmalloc (call from allocator hook). */
void kasan_alloc(const void *addr, size_t size);

/* Convenience: poison after kfree (call from allocator hook). */
void kasan_free(const void *addr, size_t size);

/* Map additional shadow pages to extend coverage for a specific virtual
 * address range.  Both start and end must be in kernel VMA space.
 * Returns 0 on success, negative on error. */
int kasan_extend_coverage(uint64_t start, uint64_t end);

#endif /* KASAN_LIGHT_H */
