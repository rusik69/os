#ifndef MODULE_ELF_H
#define MODULE_ELF_H

#include "types.h"
#include "elf.h"

/*
 * module_elf.h — ELF .ko module loader interface (M11-M16)
 *
 * Provides functions to parse, validate, and load ELF relocatable object
 * files (.ko modules) into kernel memory, resolving symbols against the
 * kernel's exported symbol table.
 *
 * Architecture:
 *   1. module_elf_validate()    — check ELF header, machine, type
 *   2. module_elf_find_section() — find a section by name
 *   3. module_elf_read_symtab()  — parse symbol table
 *   4. module_elf_read_rela()    — parse RELA relocation entries
 *   5. module_elf_load_sections() — copy PROGBITS/NOBITS to module memory
 *   6. module_elf_resolve()      — resolve undefined symbols against kernel exports
 *   7. module_elf_apply_rela()   — apply RELA relocations to loaded sections
 *   8. module_elf_set_perms()    — set final page permissions (RX, RW, RO)
 *   9. module_elf_finalize()     — call init function, mark module LIVE
 */

/* Maximum number of sections we track */
#define MODULE_ELF_MAX_SECTIONS 64

/* Maximum number of relocations per section */
#define MODULE_ELF_MAX_RELOCS   4096

/* Per-section descriptor after loading */
struct module_elf_section {
    const char *name;          /* section name (from shstrtab) */
    uint32_t    shndx;         /* section header index */
    uint32_t    sh_type;       /* SHT_* */
    uint64_t    sh_flags;      /* SHF_* */
    uint64_t    file_offset;   /* offset in the .ko file data */
    uint64_t    file_size;     /* size in file (0 for NOBITS) */
    uint64_t    mem_addr;      /* virtual address after loading */
    uint64_t    mem_size;      /* size in memory (may be > file_size for BSS) */
    int         loaded;        /* 1 = section has been loaded to memory */
};

/* Single RELA relocation entry (after parsing) */
struct module_elf_rela {
    uint64_t offset;       /* byte offset within the target section */
    uint32_t type;         /* R_X86_64_* */
    uint32_t sym_idx;      /* index into symbol table */
    int64_t  addend;       /* constant addend */
};

/* Symbol table entry (after parsing) */
struct module_elf_sym {
    const char *name;      /* symbol name (from strtab) */
    uint64_t    value;     /* value (address after resolution) */
    uint64_t    size;      /* size of object */
    uint16_t    shndx;     /* section index (0 = undefined) */
    uint8_t     bind;      /* STB_LOCAL/GLOBAL/WEAK */
    uint8_t     type;      /* STT_NOTYPE/FUNC/OBJECT */
    int         resolved;  /* 1 = value has been resolved */
};

/*
 * Parsed context for a single .ko file.
 * Filled in by module_elf_parse(); used by load/resolve/apply stages.
 */
/* Relocation sets by target section — named type for type-safety */
struct module_elf_rela_group {
    int                          section_idx;   /* index into sections[] of target */
    struct module_elf_rela       entries[MODULE_ELF_MAX_RELOCS];
    int                          count;
};

struct module_elf_context {
    const uint8_t       *file_data;          /* pointer to the full .ko file content */
    uint64_t             file_size;          /* total file size in bytes */

    /* ELF header */
    struct elf64_header  hdr;                /* validated copy of ELF header */

    /* Section header string table */
    const char          *shstrtab;           /* pointer to section name string table */
    uint64_t             shstrtab_size;      /* size of shstrtab */

    /* Section headers */
    struct elf64_shdr    shdrs[MODULE_ELF_MAX_SECTIONS];
    int                  num_sections;

    /* Section descriptors (after loading) */
    struct module_elf_section sections[MODULE_ELF_MAX_SECTIONS];

    /* Symbol table */
    const char          *strtab;             /* pointer to .strtab (symbol names) */
    uint64_t             strtab_size;        /* size of strtab */
    struct module_elf_sym syms[MODULE_ELF_MAX_SECTIONS * 4]; /* symbol table entries */
    int                  num_syms;

    /* Relocation sets by target section */
    struct module_elf_rela_group relas[MODULE_ELF_MAX_SECTIONS];
    int num_rela_sections;

    /* Name string for the module (derived from .modinfo or filename) */
    char name[64];

    /* Dependency string from .modinfo "depends=..." (M25) */
    char depends[128];

    /* Error message from last failed operation */
    char error_msg[256];
};

/*
 * ── API functions ──────────────────────────────────────────────────────
 */

/*
 * module_elf_validate - Check that @data contains a valid ELF .ko file
 * for this architecture.
 *
 * Returns 0 on success, -1 with error_msg set on failure.
 */
int module_elf_validate(struct module_elf_context *ctx,
                        const uint8_t *data, uint64_t size);

/*
 * module_elf_parse - Parse section headers, symbol table, and RELA
 * sections from the ELF data.  Populates ctx->sections, ctx->syms,
 * ctx->relas, and ctx->shstrtab/strtab.
 *
 * module_elf_validate() must be called first.
 *
 * Returns 0 on success, -1 with error_msg on failure.
 */
int module_elf_parse(struct module_elf_context *ctx);

/*
 * module_elf_print_info - Debug dump of the parsed module structure.
 * Prints to kprintf.
 */
void module_elf_print_info(const struct module_elf_context *ctx);

/*
 * module_elf_free - Release any dynamically allocated resources in ctx.
 * Currently nothing is dynamically allocated (all arrays are static), but
 * this provides a hook for future expansion.
 */
void module_elf_free(struct module_elf_context *ctx);

/*
 * ── Section loading (M12 completion), symbol resolution (M13),
 *    relocation (M14), permissions (M15), finalization (M16) ─────
 */

/*
 * module_elf_load_sections - Allocate module memory, copy PROGBITS
 * sections, and zero-fill BSS (NOBITS) sections.
 *
 * @ctx:      Parsed module context (from module_elf_parse).
 * @total_out: On success, receives the total allocated size (bytes).
 *
 * Allocates a contiguous region from the module memory allocator,
 * initially mapped RW (for patching).  On success, ctx->sections[i].mem_addr
 * is set for each loaded section and ctx->sections[i].loaded = 1.
 *
 * Returns the virtual base address of the loaded module, or 0 on failure.
 */
uint64_t module_elf_load_sections(struct module_elf_context *ctx,
                                   uint64_t *total_out);

/*
 * module_elf_resolve - Resolve undefined (imported) symbols in the
 * module's symbol table against the kernel's export table.
 *
 * @ctx:    Parsed and loaded module context.
 * @gpl_ok: Non-zero to allow GPL-only symbols.
 *
 * For each symbol with shndx == SHN_UNDEF, searches the kernel
 * export table via find_ksym().  If found, sets sym->value to the
 * kernel address and sym->resolved = 1.  Weak symbols (STB_WEAK)
 * that remain unresolved are silently zeroed (treated as optional).
 *
 * Returns 0 on success (all required symbols resolved), -1 with
 * error_msg set on failure.
 */
int module_elf_resolve(struct module_elf_context *ctx, int gpl_ok);

/*
 * module_elf_apply_rela - Apply all RELA relocations to the loaded
 * section data.
 *
 * @ctx: Parsed and loaded module context with symbols resolved.
 *
 * Handles: R_X86_64_NONE, R_X86_64_64, R_X86_64_PC32, R_X86_64_PLT32,
 * R_X86_64_32, R_X86_64_32S, R_X86_64_PC64.
 *
 * Returns 0 on success, -1 with error_msg on failure (e.g. unresolved
 * symbol, overflow on 32-bit relocation).
 */
int module_elf_apply_rela(struct module_elf_context *ctx);

/*
 * module_elf_set_perms - Set final page permissions on each loaded
 * section based on its ELF section header flags (SHF_WRITE, SHF_EXECINSTR).
 *
 * @ctx: Loaded module context.
 *
 * Typical mapping:
 *   .text   (SHF_ALLOC|SHF_EXECINSTR)          → RX  (present, no NX)
 *   .rodata (SHF_ALLOC)                        → RO  (present, NX)
 *   .data   (SHF_ALLOC|SHF_WRITE)              → RW  (present, write, NX)
 *   .bss    (SHF_ALLOC|SHF_WRITE)              → RW  (present, write, NX)
 *
 * Returns 0 on success, -1 on failure.
 */
int module_elf_set_perms(struct module_elf_context *ctx);

/*
 * module_elf_finalize - Complete module loading: set final permissions,
 * resolve symbols, apply relocations, call the module init function,
 * and record the module in the kernel module table.
 *
 * @ctx:  Parsed module context.
 * @name: Module name (or NULL to use name from .modinfo).
 *
 * Convenience wrapper that calls load_sections → resolve → apply_rela
 * → set_perms, then calls module_init() from the module, sets state to
 * LIVE, and registers the module.
 *
 * Returns module_id (>= 0) on success, -1 on failure.
 */
int module_elf_finalize(struct module_elf_context *ctx, const char *name);

/*
 * ── Low-level helpers (public for unit testing) ──────────────────────
 */

/*
 * Find a section by name in the parsed context.
 * Returns the index into ctx->sections[], or -1 if not found.
 */
int module_elf_find_section(const struct module_elf_context *ctx,
                            const char *name);

#endif /* MODULE_ELF_H */
