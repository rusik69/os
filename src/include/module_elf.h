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
    struct {
        int                          section_idx;   /* index into sections[] of target */
        struct module_elf_rela       entries[MODULE_ELF_MAX_RELOCS];
        int                          count;
    } relas[MODULE_ELF_MAX_SECTIONS];
    int num_rela_sections;

    /* Name string for the module (derived from .modinfo or filename) */
    char name[64];

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
 * ── Low-level helpers (public for unit testing) ──────────────────────
 */

/*
 * Find a section by name in the parsed context.
 * Returns the index into ctx->sections[], or -1 if not found.
 */
int module_elf_find_section(const struct module_elf_context *ctx,
                            const char *name);

#endif /* MODULE_ELF_H */
