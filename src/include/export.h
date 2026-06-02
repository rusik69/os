#ifndef EXPORT_H
#define EXPORT_H

#include "types.h"

/*
 * Kernel symbol export table — used by the module loader to resolve
 * references from loadable kernel modules (.ko files) to symbols
 * exported by the core kernel.
 *
 * Each exported symbol is an entry in the .ksymtab ELF section.
 * The section is sorted by symbol name at link time (by convention,
 * source files containing EXPORT_SYMBOL are compiled in sorted order,
 * or we sort at boot).  A binary search over the sorted table enables
 * O(log n) symbol resolution during module loading.
 *
 * Usage:
 *   EXPORT_SYMBOL(kmalloc);        // public export
 *   EXPORT_SYMBOL_GPL(some_gpl_fn); // GPL-only export
 */

/* ── Kernel symbol table entry ─────────────────────────────────────── */

struct ksym_entry {
    uint64_t    addr;       /* Address of the symbol (value) */
    const char *sym_name;   /* Pointer to symbol name string */
    int         gpl_only;   /* 1 = GPL-only export, 0 = unrestricted */
};

/* The linker collects all exported symbols into contiguous arrays.
 * These externs are defined by the linker script. */
extern struct ksym_entry __ksymtab_start[];
extern struct ksym_entry __ksymtab_end[];

/* ── EXPORT_SYMBOL / EXPORT_SYMBOL_GPL macros ────────────────────────
 *
 * Each use of EXPORT_SYMBOL(name) creates a struct ksym_entry initialised
 * with { &name, "name", 0 } and places it in the .ksymtab section.
 * The linker script collects all such entries into a contiguous array
 * between __ksymtab_start and __ksymtab_end.
 */

#define __EXPORT_SYMBOL(name, gpl)                                          \
    static const struct ksym_entry                                          \
    __attribute__((section(".ksymtab"), used, aligned(16)))                  \
    __ksym_##name = {                                                       \
        .addr     = (uint64_t)(uintptr_t)(name),                            \
        .sym_name = #name,                                                  \
        .gpl_only = (gpl),                                                  \
    }

/* Public export — available to any module regardless of license */
#define EXPORT_SYMBOL(name)    __EXPORT_SYMBOL(name, 0)

/* GPL-only export — only available to modules with a GPL-compatible license */
#define EXPORT_SYMBOL_GPL(name) __EXPORT_SYMBOL(name, 1)

/* ── Symbol lookup API ─────────────────────────────────────────────── */

/* Initialise the kernel symbol export table (sort entries by name). */
void ksym_init(void);

/* Find an exported symbol by name.
 * Returns the address (value) if found, or 0 if not found.
 * If gpl_ok is 0, GPL-only symbols are skipped.
 */
uint64_t find_ksym(const char *name, int gpl_ok);

/* Return the total number of exported symbols. */
int ksym_count(void);

/* Return the i-th exported symbol entry (for enumeration / lsmod). */
const struct ksym_entry *ksym_get_entry(int i);

/* Dump all exported symbols via kprintf (for boot-time debugging). */
void ksym_dump_all(void);

#endif /* EXPORT_H */
