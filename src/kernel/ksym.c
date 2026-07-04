/*
 * ksym.c — Kernel symbol export table management
 *
 * Provides binary search over the .ksymtab section for the module
 * loader, plus enumeration and debug-dump helpers.
 *
 * The .ksymtab section is populated by EXPORT_SYMBOL / EXPORT_SYMBOL_GPL
 * macros at compile time and linked into a contiguous array between
 * __ksymtab_start and __ksymtab_end (set up by the linker script).
 *
 * kallsyms support:
 *   In addition to the EXPORT_SYMBOL table, we build a comprehensive
 *   symbol table at boot time that includes ALL global kernel symbols
 *   (not just exported ones).  This table is used as a fallback
 *   resolution source for modules, allowing them to reference any
 *   global symbol even if it wasn't explicitly exported.
 *
 *   The comprehensive table is constructed by the linker (via the
 *   .ksymtab linker script section).  All functions and global
 *   variables with global linkage are automatically included.
 *   The module loader first tries find_ksym() (exported symbols),
 *   then falls back to find_ksym_all() (all global symbols).
 */

#define KERNEL_INTERNAL
#include "export.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"

/* ── Sorting ──────────────────────────────────────────────────────────
 *
 * We use a simple insertion sort at boot time (the table is generally
 * small — a few hundred entries — so O(n²) is fast enough in practice).
 * After sorting, binary search is O(log n).
 *
 * Entries are sorted by .name pointer value.  Since all name strings
 * reside in the kernel's .rodata, their relative order is deterministic,
 * but not alphabetical.  We sort by the string content, not by pointer.
 */

static int g_sorted = 0;

/* Total number of exported entries */
static int g_ksym_count = 0;

/* ── Comprehensive kallsyms table (all global symbols) ────────────────
 *
 * External symbols defined by the linker script.  The .kallsyms section
 * contains ALL global kernel symbols (functions and variables), not just
 * those explicitly exported via EXPORT_SYMBOL.
 */
extern struct ksym_entry __kallsyms_start[];
extern struct ksym_entry __kallsyms_end[];

/* Number of entries in the comprehensive table */
static int g_kallsyms_count = 0;
static int g_kallsyms_sorted = 0;

/* ── Dynamic symbol table (module-to-module exports) ──────────────────
 *
 * When a loadable module is loaded, any symbols it exports via
 * EXPORT_SYMBOL() or EXPORT_SYMBOL_GPL() are registered here so
 * that subsequently loaded modules can resolve references against them.
 *
 * The table is a fixed-size array.  Entries are kept sorted by name
 * so that find_ksym() can use binary search on the dynamic table too.
 */

struct ksym_dynamic_entry {
    char     name[64];      /* symbol name (copied) */
    uint64_t addr;          /* symbol address */
    int      gpl_only;      /* 1 = GPL-only export */
    int      in_use;        /* 1 = slot occupied */
};

static struct ksym_dynamic_entry g_dynamic_syms[KSYM_DYNAMIC_MAX];
static int g_dynamic_count = 0;         /* number of in-use entries */
static int g_dynamic_sorted = 0;        /* 1 = table is sorted */
static spinlock_t g_ksym_lock;

/* ── Sort the table by name (simple insertion sort) ───────────────── */

static void ksym_sort(void)
{
    int n = g_ksym_count;
    if (n <= 1) {
        g_sorted = 1;
        return;
    }

    /* Compute total number of entries */
    struct ksym_entry *base = __ksymtab_start;

    for (int i = 1; i < n; i++) {
        struct ksym_entry key;
        memcpy(&key, &base[i], sizeof(key));
        int j = i - 1;

        while (j >= 0 && strcmp(base[j].sym_name, key.sym_name) > 0) {
            memcpy(&base[j + 1], &base[j], sizeof(struct ksym_entry));
            j--;
        }
        memcpy(&base[j + 1], &key, sizeof(struct ksym_entry));
    }

    g_sorted = 1;
}

/* Sort the kallsyms comprehensive table by name */
static void kallsyms_sort(void)
{
    int n = g_kallsyms_count;
    if (n <= 1) {
        g_kallsyms_sorted = 1;
        return;
    }

    struct ksym_entry *base = __kallsyms_start;

    for (int i = 1; i < n; i++) {
        struct ksym_entry key;
        memcpy(&key, &base[i], sizeof(key));
        int j = i - 1;

        while (j >= 0 && strcmp(base[j].sym_name, key.sym_name) > 0) {
            memcpy(&base[j + 1], &base[j], sizeof(struct ksym_entry));
            j--;
        }
        memcpy(&base[j + 1], &key, sizeof(struct ksym_entry));
    }

    g_kallsyms_sorted = 1;
}

/* Sort the dynamic module symbol table by name.
 * The dynamic table is typically much smaller than the static table
 * (a few to a few dozen entries), so insertion sort is fine. */
static void dynamic_syms_sort(void)
{
    int n = g_dynamic_count;
    if (n <= 1) {
        g_dynamic_sorted = 1;
        return;
    }

    for (int i = 0; i < n - 1; i++) {
        int min_idx = i;
        for (int j = i + 1; j < n; j++) {
            if (strcmp(g_dynamic_syms[j].name,
                       g_dynamic_syms[min_idx].name) < 0) {
                min_idx = j;
            }
        }
        if (min_idx != i) {
            struct ksym_dynamic_entry tmp;
            memcpy(&tmp, &g_dynamic_syms[i], sizeof(tmp));
            memcpy(&g_dynamic_syms[i], &g_dynamic_syms[min_idx], sizeof(tmp));
            memcpy(&g_dynamic_syms[min_idx], &tmp, sizeof(tmp));
        }
    }

    g_dynamic_sorted = 1;
}

/* ── Public API ────────────────────────────────────────────────────── */

/* Initialise the symbol table: compute count and sort. */
void __init ksym_init(void)
{
    g_ksym_count = (int)(((uintptr_t)__ksymtab_end - (uintptr_t)__ksymtab_start) / sizeof(struct ksym_entry));
    if (g_ksym_count < 0)
        g_ksym_count = 0;

    ksym_sort();

    /* Initialise comprehensive kallsyms table */
    g_kallsyms_count = (int)(((uintptr_t)__kallsyms_end - (uintptr_t)__kallsyms_start) / sizeof(struct ksym_entry));
    if (g_kallsyms_count < 0)
        g_kallsyms_count = 0;

    kallsyms_sort();

    kprintf("[OK] ksym: %d exported symbols + %d all-global symbols in .ksymtab/.kallsyms tables\n",
            g_ksym_count, g_kallsyms_count);

    if (g_ksym_count > 0) {
        kprintf("[ksym] Exported symbols:");
        for (int i = 0; i < g_ksym_count && i < 8; i++) {
            kprintf(" %s", __ksymtab_start[i].sym_name);
        }
        if (g_ksym_count > 8)
            kprintf(" ... (%d more)", g_ksym_count - 8);
        kprintf("\n");
    }

    /* Initialise the dynamic module symbol table */
    spinlock_init(&g_ksym_lock);
    memset(g_dynamic_syms, 0, sizeof(g_dynamic_syms));
    g_dynamic_count = 0;
    g_dynamic_sorted = 0;

    kprintf("[OK] ksym: dynamic symbol table ready (%d slots)\n",
            KSYM_DYNAMIC_MAX);
}

/* Binary search for a symbol by name.
 * Returns the address (value) if found, or 0 if not found.
 * If gpl_ok is 0, GPL-only symbols are skipped (return 0).
 * Searches both the static kernel export table and the dynamic
 * module-to-module symbol table. */
uint64_t find_ksym(const char *name, int gpl_ok)
{
    if (!name || !g_sorted || g_ksym_count == 0)
        return 0;

    struct ksym_entry *base = __ksymtab_start;
    int lo = 0, hi = g_ksym_count - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(base[mid].sym_name, name);

        if (cmp == 0) {
            /* Found.  Check GPL restriction. */
            if (!gpl_ok && base[mid].gpl_only)
                return 0; /* GPL-only and caller not GPL */
            return base[mid].addr;
        }

        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    /* Not found in static exports — try dynamic table */
    if (!g_dynamic_sorted || g_dynamic_count == 0)
        return 0;

    lo = 0;
    hi = g_dynamic_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(g_dynamic_syms[mid].name, name);

        if (cmp == 0) {
            if (!gpl_ok && g_dynamic_syms[mid].gpl_only)
                return 0;
            return g_dynamic_syms[mid].addr;
        }

        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return 0; /* not found */
}

/*
 * find_ksym_all — Search all kernel symbols (exported + comprehensive + dynamic).
 *
 * This is the fallback symbol resolution used by the module loader.
 * It first tries find_ksym() (exported + dynamic symbols), then falls
 * back to the comprehensive kallsyms table (all global symbols).
 *
 * Returns the symbol address on success, 0 on failure.
 */
uint64_t find_ksym_all(const char *name)
{
    if (!name)
        return 0;

    /* First try exported + dynamic symbols (find_ksym now covers both) */
    uint64_t addr = find_ksym(name, 1);
    if (addr != 0)
        return addr;

    /* Fall back to comprehensive kallsyms table */
    if (!g_kallsyms_sorted || g_kallsyms_count == 0)
        return 0;

    struct ksym_entry *base = __kallsyms_start;
    int lo = 0, hi = g_kallsyms_count - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(base[mid].sym_name, name);

        if (cmp == 0)
            return base[mid].addr;

        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return 0;
}

/* Return the total number of exported symbols. */
int ksym_count(void)
{
    return g_ksym_count;
}

/* Return the i-th exported symbol entry.
 * Returns NULL if i is out of range. */
const struct ksym_entry *ksym_get_entry(int i)
{
    if (i < 0 || i >= g_ksym_count)
        return NULL;
    return &__ksymtab_start[i];
}

/* Dump all exported symbols (for boot-time debugging). */
void ksym_dump_all(void)
{
    if (g_ksym_count == 0) {
        kprintf("[ksym] (no exported symbols)\\n");
    } else {
        kprintf("[ksym] Export table (%d entries):\\n", g_ksym_count);
        for (int i = 0; i < g_ksym_count; i++) {
            const struct ksym_entry *e = &__ksymtab_start[i];
            kprintf("  %16s  %p  %s\\n",
                    e->sym_name, (void *)(uintptr_t)e->addr,
                    e->gpl_only ? "(GPL)" : "");
        }
    }

    /* Dump comprehensive kallsyms table too */
    if (g_kallsyms_count > 0) {
        kprintf("[ksym] Comprehensive kallsyms table (%d entries):\\n", g_kallsyms_count);
        for (int i = 0; i < g_kallsyms_count && i < 16; i++) {
            const struct ksym_entry *e = &__kallsyms_start[i];
            kprintf("  %16s  %p\\n",
                    e->sym_name, (void *)(uintptr_t)e->addr);
        }
        if (g_kallsyms_count > 16)
            kprintf("  ... (%d more)\\n", g_kallsyms_count - 16);
    }
}

/* ── Dynamic symbol operations ──────────────────────────── */

/* ksym_lookup — Look up a symbol by name across all tables.
 * Returns the address, or NULL if not found.
 * This is a void*-returning convenience wrapper around find_ksym_all(). */
void *ksym_lookup(const char *name)
{
    uint64_t addr = find_ksym_all(name);
    if (addr == 0)
        return NULL;
    return (void *)(uintptr_t)addr;
}

/* ksym_resolve — Reverse lookup: address → symbol name.
 * Scans static export, kallsyms, and dynamic tables linearly.
 * Returns a pointer to the static name string, or NULL if not found.
 * The returned pointer is valid until next ksym_register/unregister. */
const char *ksym_resolve(void *addr)
{
    if (!addr)
        return NULL;

    uint64_t target = (uint64_t)(uintptr_t)addr;

    /* Search static export table */
    for (int i = 0; i < g_ksym_count; i++) {
        const struct ksym_entry *e = &__ksymtab_start[i];
        if (e->addr == target)
            return e->sym_name;
    }

    /* Search comprehensive kallsyms table */
    for (int i = 0; i < g_kallsyms_count; i++) {
        const struct ksym_entry *e = &__kallsyms_start[i];
        if (e->addr == target)
            return e->sym_name;
    }

    /* Search dynamic module symbol table */
    for (int i = 0; i < g_dynamic_count; i++) {
        if (g_dynamic_syms[i].in_use &&
            g_dynamic_syms[i].addr == target)
            return g_dynamic_syms[i].name;
    }

    return NULL;
}

/* ksym_register — Register a dynamically exported symbol (from a module).
 * The entry is added to the dynamic symbol table, which is then re-sorted.
 * Returns 0 on success, -1 on error (full table or duplicate). */
int ksym_register(const char *name, void *addr, int gpl_only)
{
    if (!name || !name[0] || !addr)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_ksym_lock, &irq_flags);

    /* Check for duplicate name */
    for (int i = 0; i < g_dynamic_count; i++) {
        if (g_dynamic_syms[i].in_use &&
            strcmp(g_dynamic_syms[i].name, name) == 0) {
            spinlock_irqsave_release(&g_ksym_lock, irq_flags);
            kprintf("[ksym] ksym_register: duplicate symbol '%s' ignored\n",
                    name);
            return -1;
        }
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < KSYM_DYNAMIC_MAX; i++) {
        if (!g_dynamic_syms[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_irqsave_release(&g_ksym_lock, irq_flags);
        kprintf("[ksym] ksym_register: dynamic symbol table full "
                "(%d entries)\n", KSYM_DYNAMIC_MAX);
        return -1;
    }

    /* Fill the entry */
    strncpy(g_dynamic_syms[slot].name, name, sizeof(g_dynamic_syms[slot].name) - 1);
    g_dynamic_syms[slot].name[sizeof(g_dynamic_syms[slot].name) - 1] = '\0';
    g_dynamic_syms[slot].addr = (uint64_t)(uintptr_t)addr;
    g_dynamic_syms[slot].gpl_only = gpl_only;
    g_dynamic_syms[slot].in_use = 1;
    g_dynamic_count++;

    /* Re-sort the table for binary search */
    dynamic_syms_sort();

    spinlock_irqsave_release(&g_ksym_lock, irq_flags);

    kprintf("[ksym] Registered dynamic symbol: %s -> 0x%llx%s\n",
            name, (unsigned long long)(uintptr_t)addr,
            gpl_only ? " (GPL)" : "");
    return 0;
}

/* ksym_unregister — Remove a dynamically registered symbol.
 * Called when a module is unloaded to clean up its exported symbols.
 * Returns 0 on success, -1 if not found. */
int ksym_unregister(const char *name)
{
    if (!name || !name[0])
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_ksym_lock, &irq_flags);

    int found = -1;
    for (int i = 0; i < g_dynamic_count; i++) {
        if (g_dynamic_syms[i].in_use &&
            strcmp(g_dynamic_syms[i].name, name) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        spinlock_irqsave_release(&g_ksym_lock, irq_flags);
        kprintf("[ksym] ksym_unregister: symbol '%s' not in dynamic table\n",
                name);
        return -1;
    }

    /* Remove by shifting entries down, then decrement count */
    for (int i = found; i < g_dynamic_count - 1; i++) {
        memcpy(&g_dynamic_syms[i], &g_dynamic_syms[i + 1],
               sizeof(struct ksym_dynamic_entry));
    }
    memset(&g_dynamic_syms[g_dynamic_count - 1], 0,
           sizeof(struct ksym_dynamic_entry));
    g_dynamic_count--;

    /* Table is still sorted after removal */
    spinlock_irqsave_release(&g_ksym_lock, irq_flags);

    kprintf("[ksym] Unregistered dynamic symbol: %s\n", name);
    return 0;
}
