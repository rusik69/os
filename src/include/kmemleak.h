#ifndef KMEMLEAK_H
#define KMEMLEAK_H

/*
 * kmemleak.h — Kernel memory leak detector
 *
 * Tracks all heap and slab allocations and periodically scans
 * kernel memory to find unreferenced objects (potential leaks).
 *
 * Hooks:
 *   kmemleak_alloc(ptr, size, flags)  — call after each successful alloc
 *   kmemleak_free(ptr)                 — call before each free
 *
 * Control:
 *   kmemleak_init()     — initialise the tracker
 *   kmemleak_scan()     — run one scan cycle
 *   kmemleak_scan_work  — periodic workqueue function
 *
 * kmemleak is designed to be lightweight when disabled and provides
 * actionable leak reports with call-site backtraces.
 */

#include "types.h"

/* ── Public API ─────────────────────────────────────────────────── */

/* Initialise the kmemleak tracker.  Safe to call early in boot. */
void kmemleak_init(void);

/* Notify kmemleak that @ptr (size @size) was just allocated.
 * @flags may include KMEMLEAK_SLAB for slab-allocated objects.
 * Safe to call from any context. */
void kmemleak_alloc(const void *ptr, size_t size, int flags);

/* Notify kmemleak that @ptr is about to be freed.
 * Must be called before the memory is returned to the allocator. */
void kmemleak_free(const void *ptr);

/* Free a tracked allocation by pointer.  Called from kfree / kmem_cache_free. */
void kmemleak_remove(const void *ptr);

/* Scan kernel memory for references to tracked allocations.
 * Unreferenced allocations are reported as suspected leaks.
 * Returns the number of newly detected leaks. */
int kmemleak_scan(void);

/* Enable / disable scanning at runtime. */
void kmemleak_enable(void);
void kmemleak_disable(void);

/* Return 1 if kmemleak is active, 0 otherwise. */
int kmemleak_is_enabled(void);

/* Return the total number of tracked allocations. */
int kmemleak_allocation_count(void);

/* Return the number of allocations currently suspected as leaks. */
int kmemleak_leak_count(void);

/* Print all current scan results (leaks found so far). */
void kmemleak_print_leaks(void);

/* ── Allocation flags ────────────────────────────────────────────── */

#define KMEMLEAK_HEAP    0  /* allocated via kmalloc / heap.c */
#define KMEMLEAK_SLAB    1  /* allocated via kmem_cache_alloc */
#define KMEMLEAK_PAGE    2  /* allocated via pmm_alloc_frame / alloc_pages */
#define KMEMLEAK_VMALLOC 3  /* allocated via vmalloc */

/* ── Configuration ──────────────────────────────────────────────── */

#ifndef KMEMLEAK_MAX_TRACKED
#define KMEMLEAK_MAX_TRACKED  4096   /* maximum tracked allocations */
#endif

#ifndef KMEMLEAK_BACKTRACE_DEPTH
#define KMEMLEAK_BACKTRACE_DEPTH 12  /* stack frames captured per alloc */
#endif

#ifndef KMEMLEAK_SCAN_INTERVAL_SEC
#define KMEMLEAK_SCAN_INTERVAL_SEC 120  /* seconds between automatic scans */
#endif

#endif /* KMEMLEAK_H */
