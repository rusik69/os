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
    uint32_t    __pad[3];   /* Padding to 32 bytes (aligned(16) compat) */
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

/*
 * __KALLSYMS — Add a symbol to the comprehensive symbol table.
 *
 * This is used to register ALL global kernel symbols (not just exported
 * ones) so that the module loader can resolve them as a fallback.
 *
 * Usage: __KALLSYMS(my_function);
 *
 * This creates an entry in the .kallsyms section equivalent to a
 * non-GPL export, allowing any module to reference the symbol even
 * if not explicitly exported via EXPORT_SYMBOL().
 */
#define __KALLSYMS(name)                                                \
    static const struct ksym_entry                                      \
    __attribute__((section(".kallsyms"), used, aligned(16)))             \
    __kallsym_##name = {                                                \
        .addr     = (uint64_t)(uintptr_t)(name),                        \
        .sym_name = #name,                                              \
        .gpl_only = 0,                                                  \
    }

/* ── Symbol lookup API ─────────────────────────────────────────────── */

/* Initialise the kernel symbol export table (sort entries by name). */
void ksym_init(void);

/* Find an exported symbol by name.
 * Returns the address (value) if found, or 0 if not found.
 * If gpl_ok is 0, GPL-only symbols are skipped.
 */
uint64_t find_ksym(const char *name, int gpl_ok);

/*
 * find_ksym_all — Search all kernel symbols (exported + comprehensive).
 * This is the fallback resolution source for modules.
 * Returns the address (value) if found, or 0 if not found.
 * Unlike find_ksym(), this searches ALL global kernel symbols,
 * not just those explicitly exported via EXPORT_SYMBOL().
 */
uint64_t find_ksym_all(const char *name);

/* Return the total number of exported symbols. */
int ksym_count(void);

/* Return the i-th exported symbol entry (for enumeration / lsmod). */
const struct ksym_entry *ksym_get_entry(int i);

/* Dump all exported symbols via kprintf (for boot-time debugging). */
void ksym_dump_all(void);

#endif /* EXPORT_H */
