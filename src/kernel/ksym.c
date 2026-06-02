/*
 * ksym.c — Kernel symbol export table management
 *
 * Provides binary search over the .ksymtab section for the module
 * loader, plus enumeration and debug-dump helpers.
 *
 * The .ksymtab section is populated by EXPORT_SYMBOL / EXPORT_SYMBOL_GPL
 * macros at compile time and linked into a contiguous array between
 * __ksymtab_start and __ksymtab_end (set up by the linker script).
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

/* ── Public API ────────────────────────────────────────────────────── */

/* Initialise the symbol table: compute count and sort. */
void ksym_init(void)
{
    g_ksym_count = (int)(__ksymtab_end - __ksymtab_start);
    if (g_ksym_count < 0)
        g_ksym_count = 0;

    ksym_sort();

    kprintf("[OK] ksym: %d exported symbols in .ksymtab table\n", g_ksym_count);

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
        kprintf("[ksym] (no exported symbols)\n");
        return;
    }

    kprintf("[ksym] Export table (%d entries):\n", g_ksym_count);
    for (int i = 0; i < g_ksym_count; i++) {
        const struct ksym_entry *e = &__ksymtab_start[i];
        kprintf("  %16s  %p  %s\n",
                e->sym_name, (void *)(uintptr_t)e->addr,
                e->gpl_only ? "(GPL)" : "");
    }
}
