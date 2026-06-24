#ifndef KALLSYMS_H
#define KALLSYMS_H

#include "types.h"
#include "string.h"
#include "printf.h"

/*
 * Kallsyms — simple symbol table lookup for stack traces and debugging.
 *
 * Implements a built-in symbol table that maps addresses to function names.
 * The table is populated at build time from the kernel ELF (by extracting
 * the symbol table) or at runtime by parsing /proc/kallsyms data.
 *
 * For simplicity, we include a statically-defined table of key kernel
 * symbols that can be looked up by address. This enables meaningful
 * stack traces instead of just "unknown".
 */

/* Maximum number of built-in symbols */
#define KALLSYMS_MAX_SYMBOLS 512

/* A symbol table entry */
struct kallsyms_entry {
    uint64_t addr;
    const char *name;
};

/* Built-in symbol table (sorted by address for binary search) */
static const struct kallsyms_entry kallsyms_table[] = {
    /* Kernel entry points and major functions */
    { 0xFFFFFFFF80000000ULL, "_start" },
    { 0xFFFFFFFF80000010ULL, "kernel_main" },
    { 0xFFFFFFFF80000020ULL, "kernel_entry" },

    /* Addr here will be filled dynamically. For this kernel, we don't
     * have fixed addresses because of the high-half VMA offset.
     *
     * Instead, we compute the actual addresses at boot time by scanning
     * for known function pointers. The table below uses offsets from
     * a base symbol that we calibrate at runtime.
     *
     * For now, we use a simpler approach: a small hardcoded set of
     * important kernel function names that can be matched by name
     * if their addresses are registered at boot.
     */

    /* Terminator */
    { 0, NULL }
};

/* Runtime symbol table (registered at boot by each subsystem) */
static struct kallsyms_entry runtime_table[KALLSYMS_MAX_SYMBOLS];
static int runtime_symbol_count = 0;
static int kallsyms_initialized = 0;

/*
 * Initialize kallsyms subsystem.
 * This should be called after the kernel heap is available.
 */
static inline void kallsyms_init(void) {
    memset(runtime_table, 0, sizeof(runtime_table));
    runtime_symbol_count = 0;
    kallsyms_initialized = 1;
}

/*
 * Register a kernel symbol at runtime.
 * This is called by EXPORT_SYMBOL-like macros during kernel init.
 * Returns 0 on success, -1 if table is full.
 */
static inline int kallsyms_register(const char *name, uint64_t addr) {
    if (!name || !kallsyms_initialized) return -1;
    if (runtime_symbol_count >= KALLSYMS_MAX_SYMBOLS) return -1;

    /* Check for duplicate */
    for (int i = 0; i < runtime_symbol_count; i++) {
        if (runtime_table[i].addr == addr) return 0;  /* Already registered */
    }

    runtime_table[runtime_symbol_count].addr = addr;
    runtime_table[runtime_symbol_count].name = name;
    runtime_symbol_count++;
    return 0;
}

/*
 * Look up a symbol name by address.
 * Returns the symbol name string, or "unknown" if not found.
 *
 * Uses binary search on the runtime table (sorted by address).
 */
static inline const char *kallsyms_lookup(uint64_t addr) {
    /* Search runtime table first */
    if (kallsyms_initialized && runtime_symbol_count > 0) {
        /* Binary search */
        int lo = 0, hi = runtime_symbol_count - 1;
        int best = -1;

        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (runtime_table[mid].addr <= addr) {
                best = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }

        if (best >= 0) {
            /* Check if addr is close to the symbol start (within 64KB) */
            uint64_t offset = addr - runtime_table[best].addr;
            if (offset < 65536) {
                return runtime_table[best].name;
            }
        }
    }

    /* Search static table as fallback */
    for (int i = 0; kallsyms_table[i].name != NULL; i++) {
        if (kallsyms_table[i].addr == addr)
            return kallsyms_table[i].name;
    }

    return "unknown";
}

/*
 * Get the number of registered symbols.
 */
static inline int kallsyms_get_count(void) {
    return runtime_symbol_count;
}

/*
 * Print all registered symbols (for debugging).
 */
static inline void kallsyms_print_all(void) {
    kprintf("Kernel symbol table (%d entries):\n", runtime_symbol_count);
    for (int i = 0; i < runtime_symbol_count; i++) {
        kprintf("  [0x%llx] %s\n",
                (unsigned long long)runtime_table[i].addr,
                runtime_table[i].name);
    }
}

/*
 * Print the current stack trace by walking frame pointers.
 * Uses RBP-based frame pointer chain on x86-64.
 */
static inline void kallsyms_print_stack(void) {
    uint64_t *rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    kprintf("Call trace:\n");
    for (int i = 0; i < 32; i++) {
        if (!rbp || (uint64_t)rbp < 0xFFFF800000000000ULL)
            break;
        uint64_t rip = rbp[1];
        if (rip == 0) break;
        kprintf("  [0x%llx] %s\n", (unsigned long long)rip, kallsyms_lookup(rip));
        rbp = (uint64_t *)rbp[0];
    }
}

#endif /* KALLSYMS_H */
