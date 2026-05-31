#ifndef KALLSYMS_H
#define KALLSYMS_H

#include "types.h"

/*
 * Kallsyms — simple symbol table lookup for stack traces.
 *
 * Currently a stub that always returns "unknown". In a full implementation
 * this would parse a symbol table embedded in the kernel ELF.
 */

/* Look up a symbol name by address. Returns a string or "unknown". */
static inline const char *kallsyms_lookup(uint64_t addr) {
    (void)addr;
    return "unknown";
}

/* Print the current stack trace by walking frame pointers.
 * Uses RBP-based frame pointer chain on x86-64. */
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
