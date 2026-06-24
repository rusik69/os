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

/* ── Public API ────────────────────────────────────────────────────── */

/* Initialise the symbol table: compute count and sort. */
void ksym_init(void)
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
}

/* Binary search for a symbol by name.
 * Returns the address (value) if found, or 0 if not found.
 * If gpl_ok is 0, GPL-only symbols are skipped (return 0). */
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

    return 0; /* not found */
}

/*
 * find_ksym_all — Search all kernel symbols (exported + comprehensive).
 *
 * This is the fallback symbol resolution used by the module loader.
 * It first tries find_ksym() (exported symbols), then falls back to
 * the comprehensive kallsyms table (all global symbols).
 *
 * Returns the symbol address on success, 0 on failure.
 */
uint64_t find_ksym_all(const char *name)
{
    if (!name)
        return 0;

    /* First try exported symbols */
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

/* ── Stub: ksym_lookup ─────────────────────────────── */
static void* ksym_lookup(const char *name)
{
    (void)name;
    kprintf("[ksym] ksym_lookup: not yet implemented\n");
    return 0;
}
/* ── Stub: ksym_resolve ─────────────────────────────── */
static const char* ksym_resolve(void *addr)
{
    (void)addr;
    kprintf("[ksym] ksym_resolve: not yet implemented\n");
    return 0;
}
/* ── Stub: ksym_register ─────────────────────────────── */
static int ksym_register(const char *name, void *addr)
{
    (void)name;
    (void)addr;
    kprintf("[ksym] ksym_register: not yet implemented\n");
    return 0;
}
/* ── Stub: ksym_unregister ─────────────────────────────── */
static int ksym_unregister(const char *name)
{
    (void)name;
    kprintf("[ksym] ksym_unregister: not yet implemented\n");
    return 0;
}
