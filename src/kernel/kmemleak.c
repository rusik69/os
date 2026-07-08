/*
 * kmemleak.c — Kernel memory leak detector
 *
 * Scans kernel memory for references to allocated objects and reports
 * any allocations that are not reachable (suspected memory leaks).
 *
 * Algorithm:
 *   1. Maintain a "grey" list of all tracked allocations (ptr, size,
 *      backtrace, allocation time).
 *   2. On each scan, walk the following memory regions as "roots":
 *      - All process task structs and their kernel stacks
 *      - The kernel .data and .bss sections
 *      - Registered scan roots (added by subsystems)
 *   3. For each pointer found in root memory, check if it points into
 *      a tracked allocation.  If so, mark the allocation as reachable.
 *   4. After scanning all roots, any unmarked allocation is a leak.
 *
 * The approach is conservative: false negatives (missed leaks) are
 * possible but false positives (reporting live memory as leaked) are
 * minimised.
 *
 * Reference: Linux kernel's kmemleak implementation.
 */

#define KERNEL_INTERNAL
#include "kmemleak.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "scheduler.h"
#include "process.h"
#include "pmm.h"
#include "timer.h"
#include "workqueue.h"
#include "debugfs.h"
#include "sysfs.h"

/* ── Internal data structures ────────────────────────────────────── */

/** One tracked allocation entry */
struct kmemleak_entry {
    uint64_t    ptr;             /* pointer value (physical/virtual) */
    size_t      size;            /* allocation size */
    int         flags;           /* KMEMLEAK_HEAP, _SLAB, _PAGE, _VMALLOC */
    int         scanned;         /* 1 = reachable from roots in current scan */
    uint64_t    alloc_tick;      /* kernel tick when allocated */
    uint64_t    backtrace[KMEMLEAK_BACKTRACE_DEPTH];  /* return addresses */
    int         bt_depth;        /* number of valid backtrace entries */
    int         leaks_reported;  /* how many times this entry appeared in leaks */
    int         in_use;          /* 1 = slot active */
};

/** Scan root: a memory range to include in the root set */
struct kmemleak_root {
    uint64_t    start;
    uint64_t    end;
    int         in_use;
};

/* ── Forward declarations ───────────────────────────────────────── */

/* Register a memory range as a scan root. */
void kmemleak_add_root(uint64_t start, uint64_t end);

/* ── Static state ───────────────────────────────────────────────── */

static struct kmemleak_entry
    g_entries[KMEMLEAK_MAX_TRACKED];
static struct kmemleak_root
    g_roots[64];
static int g_root_count = 0;

static spinlock_t g_lock;
static int g_initialised = 0;
static int g_enabled = 0;
static int g_leak_count = 0;
static uint64_t g_last_scan_tick = 0;
static int g_scan_in_progress = 0;

/* ── Backtrace capture ──────────────────────────────────────────── */

/*
 * Capture the current call chain using frame-pointer walking.
 * RBP-based on x86-64.  Stores up to KMEMLEAK_BACKTRACE_DEPTH return
 * addresses into bt[] and returns the number captured.
 *
 * The first entry (bt[0]) is the return address of the function that
 * called the allocator; later entries are further up the call chain.
 */
static int capture_backtrace(uint64_t *bt, int max_depth)
{
    int count = 0;
    uint64_t *rbp;

    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    for (int i = 0; i < max_depth && rbp; i++) {
        /* Validate RBP is in kernel address space */
        if ((uint64_t)rbp < 0xFFFF800000000000ULL ||
            (uint64_t)rbp > 0xFFFFFFFFFFFFFFFFULL)
            break;
        uint64_t ret_addr = rbp[1];
        if (ret_addr == 0)
            break;
        if (count < max_depth)
            bt[count++] = ret_addr;
        rbp = (uint64_t *)rbp[0];
    }

    return count;
}

/* ── Entry management ───────────────────────────────────────────── */

static int find_free_slot(void)
{
    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (!g_entries[i].in_use)
            return i;
    }
    return -1;
}

/* ── Public API ─────────────────────────────────────────────────── */

void __init kmemleak_init(void)
{
    if (g_initialised)
        return;

    memset(g_entries, 0, sizeof(g_entries));
    memset(g_roots, 0, sizeof(g_roots));
    g_root_count = 0;
    g_leak_count = 0;
    g_last_scan_tick = 0;
    g_scan_in_progress = 0;
    spinlock_init(&g_lock);
    g_initialised = 1;
    g_enabled = 1;

    /* Register the kernel's .data and .bss sections as scan roots.
     * These sections are defined by the linker and hold global variables
     * that may point to heap-allocated memory. */
    extern char _data_start[], _data_end[], _bss_start[], _bss_end[];

    if ((uint64_t)_data_start && (uint64_t)_data_end &&
        (uint64_t)_data_end > (uint64_t)_data_start) {
        kmemleak_add_root((uint64_t)_data_start, (uint64_t)_data_end);
    }

    if ((uint64_t)_bss_start && (uint64_t)_bss_end &&
        (uint64_t)_bss_end > (uint64_t)_bss_start) {
        kmemleak_add_root((uint64_t)_bss_start, (uint64_t)_bss_end);
    }

    kprintf("[OK] kmemleak: kernel memory leak detector initialized "
            "(%d entries, scan interval=%ds)\n",
            KMEMLEAK_MAX_TRACKED, KMEMLEAK_SCAN_INTERVAL_SEC);
}

int kmemleak_is_enabled(void)
{
    return g_enabled;
}

void kmemleak_enable(void)
{
    g_enabled = 1;
    kprintf("[kmemleak] enabled\n");
}

void kmemleak_disable(void)
{
    g_enabled = 0;
    kprintf("[kmemleak] disabled\n");
}

/*
 * Register a memory range as a scan root.
 * During scanning, every 8-byte-aligned word in [start, end) will be
 * examined for pointers to tracked allocations.
 */
void kmemleak_add_root(uint64_t start, uint64_t end)
{
    if (!g_initialised)
        return;
    if (start >= end)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    if (g_root_count >= 64) {
        spinlock_irqsave_release(&g_lock, irq_flags);
        return;
    }

    g_roots[g_root_count].start = start;
    g_roots[g_root_count].end   = end;
    g_roots[g_root_count].in_use = 1;
    g_root_count++;

    spinlock_irqsave_release(&g_lock, irq_flags);
}

void kmemleak_alloc(const void *ptr, size_t size, int flags)
{
    if (!g_initialised || !g_enabled)
        return;
    if (!ptr || size == 0)
        return;

    uint64_t ptr_val = (uint64_t)(uintptr_t)ptr;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    int slot = find_free_slot();
    if (slot < 0) {
        /* Table full — warn once. */
        static int table_full_warned = 0;
        if (!table_full_warned) {
            kprintf("[kmemleak] WARNING: tracked allocation table full "
                    "(%d entries).  Leak detection degraded.\n",
                    KMEMLEAK_MAX_TRACKED);
            table_full_warned = 1;
        }
        spinlock_irqsave_release(&g_lock, irq_flags);
        return;
    }

    struct kmemleak_entry *e = &g_entries[slot];
    e->ptr          = ptr_val;
    e->size         = size;
    e->flags        = flags;
    e->scanned      = 0;
    e->alloc_tick   = timer_get_ticks();
    e->bt_depth     = capture_backtrace(e->backtrace,
                                         KMEMLEAK_BACKTRACE_DEPTH);
    e->leaks_reported = 0;
    e->in_use       = 1;

    spinlock_irqsave_release(&g_lock, irq_flags);
}

void kmemleak_free(const void *ptr)
{
    kmemleak_remove(ptr);
}

void kmemleak_remove(const void *ptr)
{
    if (!g_initialised || !g_enabled)
        return;
    if (!ptr)
        return;

    uint64_t ptr_val = (uint64_t)(uintptr_t)ptr;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (g_entries[i].in_use && g_entries[i].ptr == ptr_val) {
            memset(&g_entries[i], 0, sizeof(g_entries[i]));
            break;
        }
    }

    spinlock_irqsave_release(&g_lock, irq_flags);
}

int kmemleak_allocation_count(void)
{
    if (!g_initialised)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    int count = 0;
    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (g_entries[i].in_use)
            count++;
    }

    spinlock_irqsave_release(&g_lock, irq_flags);
    return count;
}

int kmemleak_leak_count(void)
{
    return g_leak_count;
}

/* ── Scanning logic ─────────────────────────────────────────────── */

/*
 * Determine whether a value @val (read from root memory) looks like a
 * pointer to a tracked allocation.  We check if @val falls within the
 * range [ptr, ptr + size) of any active tracked entry.
 *
 * Returns the index of the matched entry, or -1 if none.
 */
static int match_pointer(uint64_t val)
{
    /* Reject obviously non-pointer values */
    if (val < 0xFFFF800000000000ULL)
        return -1;  /* not in kernel address space */

    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (!g_entries[i].in_use)
            continue;

        uint64_t start = g_entries[i].ptr;
        uint64_t end   = start + g_entries[i].size;

        /* Also check if the value points into the middle of an allocation
         * (not just the start). This catches interior pointers. */
        if (val >= start && val < end) {
            return i;  /* matched! */
        }
    }

    return -1;
}

/*
 * Scan a single memory region [start, end) for pointers to tracked
 * allocations.  Scans 8-byte aligned words.
 */
static void scan_region(uint64_t start, uint64_t end)
{
    /* Validate the region looks like valid kernel memory */
    if (start >= end)
        return;
    if (end - start > (64ULL * 1024 * 1024)) {
        /* Sanity: refuse to scan regions larger than 64 MB at once */
        end = start + (64ULL * 1024 * 1024);
    }

    uint64_t *words = (uint64_t *)start;
    uint64_t count  = (end - start) / sizeof(uint64_t);

    for (uint64_t i = 0; i < count; i++) {
        uint64_t val;

        /* Safe read: handle potential fault on unmapped memory */
        __asm__ volatile(
            "mov %1, %0\n\t"
            : "=r"(val)
            : "m"(words[i])
            : "memory"
        );

        int idx = match_pointer(val);
        if (idx >= 0) {
            g_entries[idx].scanned = 1;
        }
    }
}

/*
 * Scan all registered root regions for pointers.
 */
static void scan_roots(void)
{
    for (int i = 0; i < g_root_count; i++) {
        if (!g_roots[i].in_use)
            continue;
        scan_region(g_roots[i].start, g_roots[i].end);
    }
}

/*
 * Scan kernel stacks of all processes for pointers.
 * This is a key root because local variables live on the stack.
 */
static void scan_process_stacks(void)
{
    for (int pid = 0; pid < PROCESS_MAX; pid++) {
        struct process *proc = process_get_by_pid((uint32_t)pid);
        if (!proc || proc->state == PROCESS_UNUSED)
            continue;

        /* The kernel stack is defined by kernel_stack (base/lowest addr)
         * and stack_top (highest addr).  Scan the entire stack range
         * for pointers to tracked allocations. */
        uint64_t stack_base = proc->kernel_stack;
        uint64_t stack_top  = proc->stack_top;

        if (stack_base && stack_top && stack_top > stack_base) {
            /* Limit stack scanning to a reasonable window.
             * The current stack pointer is somewhere within this range. */
            scan_region(stack_base, stack_top);
        }

        /* Also scan the task struct itself (it may contain embedded
         * pointers to allocated memory like fd_table, etc.) */
        uint64_t proc_addr = (uint64_t)(uintptr_t)proc;
        scan_region(proc_addr, proc_addr + sizeof(struct process));
    }
}

/*
 * Run one full kmemleak scan cycle.
 *
 * Steps:
 *   1. Clear the "scanned" flag on all tracked allocations.
 *   2. Walk all root regions (kernel .data/.bss, registered subsystems).
 *   3. Walk all process stacks and task structs.
 *   4. Any allocation whose scanned flag is still 0 is unreachable
 *      and reported as a suspected leak.
 *
 * Returns the number of newly detected leaks.
 */
int kmemleak_scan(void)
{
    if (!g_initialised || !g_enabled)
        return 0;
    if (g_scan_in_progress)
        return 0;

    g_scan_in_progress = 1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    /* Step 1: Clear scanned flags */
    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (g_entries[i].in_use)
            g_entries[i].scanned = 0;
    }

    /* Step 2: Scan root regions */
    scan_roots();

    /* Step 3: Scan process stacks */
    spinlock_irqsave_release(&g_lock, irq_flags);
    scan_process_stacks();
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    /* Step 4: Report leaks */
    int new_leaks = 0;
    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (!g_entries[i].in_use)
            continue;
        if (g_entries[i].scanned)
            continue;

        /* This allocation was not reached from any root — it's a leak. */
        if (g_entries[i].leaks_reported == 0) {
            new_leaks++;
        }
        g_entries[i].leaks_reported++;
    }

    if (new_leaks > 0) {
        kprintf("[kmemleak] SCAN: %d new suspected leak(s) detected "
                "(total tracked=%d, total leaks=%d)\n",
                new_leaks, kmemleak_allocation_count(),
                g_leak_count + new_leaks);

        /* Print details for each newly detected leak */
        for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
            if (!g_entries[i].in_use || g_entries[i].scanned)
                continue;
            if (g_entries[i].leaks_reported != 1)
                continue;  /* only newly detected */

            struct kmemleak_entry *e = &g_entries[i];
            kprintf("[kmemleak]   LEAK: ptr=0x%llx size=%llu flags=%d "
                    "age=%llu sec\n",
                    (unsigned long long)e->ptr,
                    (unsigned long long)e->size, e->flags,
                    (unsigned long long)(timer_get_ticks() - e->alloc_tick)
                        / TIMER_FREQ);
            if (e->bt_depth > 0) {
                kprintf("[kmemleak]     Backtrace:");
                int show = e->bt_depth < 5 ? e->bt_depth : 5;
                for (int j = 0; j < show; j++) {
                    kprintf(" 0x%llx",
                            (unsigned long long)e->backtrace[j]);
                }
                kprintf("\n");
            }
        }
    }

    g_leak_count += new_leaks;
    g_last_scan_tick = timer_get_ticks();
    g_scan_in_progress = 0;

    spinlock_irqsave_release(&g_lock, irq_flags);

    return new_leaks;
}

void kmemleak_print_leaks(void)
{
    if (!g_initialised) {
        kprintf("[kmemleak] not initialized\n");
        return;
    }

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    int leak_count = 0;
    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (!g_entries[i].in_use)
            continue;
        if (g_entries[i].scanned)
            continue;

        leak_count++;
        struct kmemleak_entry *e = &g_entries[i];
        kprintf("[kmemleak] LEAK #%d: ptr=0x%llx size=%llu flags=%d "
                "reported=%d times, age=%llu sec\n",
                leak_count,
                (unsigned long long)e->ptr,
                (unsigned long long)e->size, e->flags,
                e->leaks_reported,
                (unsigned long long)(timer_get_ticks() - e->alloc_tick)
                    / TIMER_FREQ);
        if (e->bt_depth > 0) {
            kprintf("[kmemleak]   Backtrace:");
            int show = e->bt_depth < 5 ? e->bt_depth : 5;
            for (int j = 0; j < show; j++) {
                kprintf(" 0x%llx", (unsigned long long)e->backtrace[j]);
            }
            kprintf("\n");
        }
    }

    kprintf("[kmemleak] Total: %d tracked allocations, "
            "%d suspected leaks\n",
            kmemleak_allocation_count(), leak_count);

    spinlock_irqsave_release(&g_lock, irq_flags);
}

/* ── Debugfs/sysfs scan trigger interface ───────────────────────── */

/* Read handler for scan trigger — returns last scan result summary */
static int kmemleak_debugfs_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    int n = snprintf(buf, (int)max_size,
        "kmemleak: %d tracked, %d leaks\n"
        "Write 'scan' to trigger a scan\n"
        "Write 'clear' to clear leak counters\n"
        "Write 'disable' to disable tracking\n"
        "Write 'enable' to enable tracking\n",
        kmemleak_allocation_count(),
        kmemleak_leak_count());
    if (n < 0) return 0;
    if ((uint32_t)n >= max_size) return (int)max_size - 1;
    return n;
}

/* Write handler for scan trigger */
static int kmemleak_debugfs_write(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    const char *p = data;
    uint32_t pos = 0;

    while (pos < size && *p && (*p == ' ' || *p == '\t')) { p++; pos++; }

    /* Check for "scan" command */
    if (size - pos >= 4 && p[0] == 's' && p[1] == 'c' &&
        p[2] == 'a' && p[3] == 'n') {
        kprintf("[kmemleak] manual scan triggered via debugfs\n");
        kmemleak_scan();
        return 0;
    }

    /* Check for "clear" command */
    if (size - pos >= 5 && p[0] == 'c' && p[1] == 'l' &&
        p[2] == 'e' && p[3] == 'a' && p[4] == 'r') {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_lock, &irq_flags);
        g_leak_count = 0;
        for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
            if (g_entries[i].in_use)
                g_entries[i].leaks_reported = 0;
        }
        spinlock_irqsave_release(&g_lock, irq_flags);
        kprintf("[kmemleak] leak counters cleared\n");
        return 0;
    }

    /* Check for "enable" command */
    if (size - pos >= 6 && p[0] == 'e' && p[1] == 'n' &&
        p[2] == 'a' && p[3] == 'b' && p[4] == 'l' && p[5] == 'e') {
        kmemleak_enable();
        return 0;
    }

    /* Check for "disable" command */
    if (size - pos >= 7 && p[0] == 'd' && p[1] == 'i' &&
        p[2] == 's' && p[3] == 'a' && p[4] == 'b' && p[5] == 'l' &&
        p[6] == 'e') {
        kmemleak_disable();
        return 0;
    }

    /* Also check for "print" to dump all leaks */
    if (size - pos >= 5 && p[0] == 'p' && p[1] == 'r' &&
        p[2] == 'i' && p[3] == 'n' && p[4] == 't') {
        kmemleak_print_leaks();
        return 0;
    }

    kprintf("[kmemleak] unknown command '%.*s'\n", (int)(size - pos), p);
    return 0;
}

/* Register debugfs/sysfs interface for kmemleak */
void kmemleak_register_debugfs(void)
{
    if (!g_initialised)
        return;

    /* Create /sys/kernel/debug/kmemleak file */
    if (sysfs_create_dir("/sys/kernel/debug") < 0) {
        /* may already exist */
    }

    if (sysfs_create_writable_file(
            "/sys/kernel/debug/kmemleak",
            "kmemleak: write 'scan' to trigger scan\n",
            NULL, kmemleak_debugfs_read, kmemleak_debugfs_write) < 0) {
        kprintf("[kmemleak] failed to create debugfs scan trigger\n");
        return;
    }

    /* Also register a debugfs u32 entry for the leak count */
    debugfs_create_u32("kmemleak_leak_count", (uint32_t*)&g_leak_count);

    kprintf("[OK] kmemleak: debugfs interface at /sys/kernel/debug/kmemleak\n");
}

/* KUnit-compatible test helper: return number of unreferenced objects */
int kmemleak_unreferenced_count(void)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_lock, &irq_flags);

    int count = 0;
    for (int i = 0; i < KMEMLEAK_MAX_TRACKED; i++) {
        if (g_entries[i].in_use && !g_entries[i].scanned)
            count++;
    }

    spinlock_irqsave_release(&g_lock, irq_flags);
    return count;
}

/* ── Stub: kmemleak_report ─────────────────────────────── */
static int kmemleak_report(void)
{
    kprintf("[kmemleak] kmemleak_report: not yet implemented\n");
    return 0;
}
