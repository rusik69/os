/*
 * module_elf.c — ELF .ko module loader
 *
 * Implements parsing, validation, and relocation of ELF relocatable object
 * files (.ko modules) for the modular kernel transition (M11-M16).
 *
 * Architecture:
 *   A .ko file is an ELF64 ET_REL (relocatable) object.  It contains:
 *     - Section headers (.text, .rodata, .data, .bss, .symtab, .strtab,
 *       .shstrtab, .rela.text, .rela.data, .rela.rodata, .modinfo, etc.)
 *     - A symbol table (.symtab) with both exported and imported symbols
 *     - Relocation sections (.rela.*) describing how to patch each
 *       code/data section so that cross-references refer to kernel addresses
 *
 * Loading sequence:
 *   1. Validate ELF header (M11)
 *   2. Parse section headers, locate shstrtab/strtab (M12)
 *   3. Parse symbol table, identify undefined (imported) symbols (M13)
 *   4. Parse RELA relocation entries (M11)
 *   5. Copy PROGBITS sections to module memory, zero BSS (M12)
 *   6. Resolve undefined symbols against kernel export table (M13)
 *   7. Apply relocations to loaded sections (M14)
 *   8. Set final page permissions (M15)
 *   9. Call module init function (M16)
 */

#define KERNEL_INTERNAL
#include "module_elf.h"
#include "module.h"
#include "module_signature.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "export.h"

/* ── Forward declarations of internal helpers ────────────────────────── */

static int parse_section_headers(struct module_elf_context *ctx);
static int parse_symbol_table(struct module_elf_context *ctx);
static int parse_rela_sections(struct module_elf_context *ctx);

/* ── Validation (M11) ────────────────────────────────────────────────── */

int module_elf_validate(struct module_elf_context *ctx,
                        const uint8_t *data, uint64_t size)
{
    if (!ctx || !data) {
        if (ctx) snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                          "module_elf: NULL arguments");
        return -1;
    }

    /* Clear context */
    memset(ctx, 0, sizeof(*ctx));
    ctx->file_data = data;
    ctx->file_size = size;

    /* Must be at least as big as the ELF header */
    if (size < sizeof(struct elf64_header)) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: file too small (%llu bytes, need %zu)",
                 (unsigned long long)size, sizeof(struct elf64_header));
        return -1;
    }

    /* Copy header for safe access (data may be in userspace buffer) */
    memcpy(&ctx->hdr, data, sizeof(ctx->hdr));

    /* Check magic */
    if (*(const uint32_t *)ctx->hdr.e_ident != ELF_MAGIC) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: bad ELF magic 0x%08x",
                 *(const uint32_t *)ctx->hdr.e_ident);
        return -1;
    }

    /* Must be 64-bit */
    if (ctx->hdr.e_ident[4] != ELF_CLASS64) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: not a 64-bit ELF (class=%d)", ctx->hdr.e_ident[4]);
        return -1;
    }

    /* Must be x86-64 */
    if (ctx->hdr.e_machine != EM_X86_64) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: not x86-64 (machine=0x%x)", ctx->hdr.e_machine);
        return -1;
    }

    /* Must be relocatable object file (ET_REL) */
    if (ctx->hdr.e_type != ET_REL) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: not a relocatable object (type=%d)", ctx->hdr.e_type);
        return -1;
    }

    /* Validate section header table location */
    if (ctx->hdr.e_shoff == 0 || ctx->hdr.e_shnum == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: no section headers");
        return -1;
    }

    if (ctx->hdr.e_shentsize < sizeof(struct elf64_shdr)) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: section header entry too small (%d bytes)",
                 ctx->hdr.e_shentsize);
        return -1;
    }

    uint64_t shdr_table_end = ctx->hdr.e_shoff +
        (uint64_t)ctx->hdr.e_shnum * (uint64_t)ctx->hdr.e_shentsize;
    if (shdr_table_end > size) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: section header table out of bounds (%llu > %llu)",
                 (unsigned long long)shdr_table_end, (unsigned long long)size);
        return -1;
    }

    /* ── Verify module signature (Item 75) ── */
    {
        int sig_ret = module_verify_elf(data, size);
        if (sig_ret != 0) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: signature verification failed (err=%d)",
                     sig_ret);
            return -1;
        }
    }

    return 0;
}

/* ── Section header parsing (M12) ────────────────────────────────────── */

static int parse_section_headers(struct module_elf_context *ctx)
{
    const uint8_t *data = ctx->file_data;
    uint64_t size = ctx->file_size;
    uint16_t shnum = ctx->hdr.e_shnum;
    uint16_t shstrndx = ctx->hdr.e_shstrndx;
    uint16_t shentsize = ctx->hdr.e_shentsize;

    if (shnum > MODULE_ELF_MAX_SECTIONS) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: too many sections (%d, max %d)",
                 shnum, MODULE_ELF_MAX_SECTIONS);
        return -1;
    }

    /* Read all section headers */
    for (int i = 0; i < shnum; i++) {
        uint64_t off = ctx->hdr.e_shoff + (uint64_t)i * (uint64_t)shentsize;
        if (off + sizeof(struct elf64_shdr) > size) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: section header %d out of bounds", i);
            return -1;
        }
        memcpy(&ctx->shdrs[i], data + off, sizeof(struct elf64_shdr));

        /* Validate section data is within file (for non-NOBITS sections) */
        if (ctx->shdrs[i].sh_type != SHT_NOBITS &&
            ctx->shdrs[i].sh_offset + ctx->shdrs[i].sh_size > size &&
            ctx->shdrs[i].sh_size > 0) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: section %d data out of bounds (off=%llu + sz=%llu > %llu)",
                     i,
                     (unsigned long long)ctx->shdrs[i].sh_offset,
                     (unsigned long long)ctx->shdrs[i].sh_size,
                     (unsigned long long)size);
            return -1;
        }
    }
    ctx->num_sections = shnum;

    /* Locate section header string table */
    if (shstrndx >= shnum) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: shstrndx %d out of range (sections=%d)",
                 shstrndx, shnum);
        return -1;
    }

    ctx->shstrtab = (const char *)(data + ctx->shdrs[shstrndx].sh_offset);
    ctx->shstrtab_size = ctx->shdrs[shstrndx].sh_size;

    /* Verify at least the first byte of shstrtab is accessible */
    if (ctx->shstrtab_size == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: shstrtab is empty");
        return -1;
    }

    return 0;
}

/* ── Symbol table parsing (M13) ──────────────────────────────────────── */

static int parse_symbol_table(struct module_elf_context *ctx)
{
    const uint8_t *data = ctx->file_data;
    int symtab_idx = -1;
    int strtab_idx = -1;

    /* Find .symtab and .strtab sections */
    for (int i = 0; i < ctx->num_sections; i++) {
        if (ctx->shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_idx = i;
        }
        if (ctx->shdrs[i].sh_type == SHT_STRTAB) {
            /* .strtab is usually the first STRTAB that is NOT shstrtab */
            if (i != ctx->hdr.e_shstrndx && strtab_idx < 0) {
                strtab_idx = i;
            }
        }
    }

    if (symtab_idx < 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: no .symtab section found");
        return -1;
    }

    if (strtab_idx < 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: no .strtab section found");
        return -1;
    }

    /* Locate strtab data */
    ctx->strtab = (const char *)(data + ctx->shdrs[strtab_idx].sh_offset);
    ctx->strtab_size = ctx->shdrs[strtab_idx].sh_size;

    if (ctx->strtab_size == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: strtab is empty");
        return -1;
    }

    /* Validate symtab entry size */
    struct elf64_shdr *symtab_sh = &ctx->shdrs[symtab_idx];
    if (symtab_sh->sh_entsize < sizeof(struct elf64_sym)) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: symtab entry too small (%llu bytes)",
                 (unsigned long long)symtab_sh->sh_entsize);
        return -1;
    }

    int num_syms = (int)(symtab_sh->sh_size / symtab_sh->sh_entsize);
    if (num_syms > (int)(sizeof(ctx->syms) / sizeof(ctx->syms[0]))) {
        num_syms = (int)(sizeof(ctx->syms) / sizeof(ctx->syms[0]));
    }
    ctx->num_syms = num_syms;

    /* Parse each symbol */
    for (int i = 0; i < num_syms; i++) {
        uint64_t off = symtab_sh->sh_offset + (uint64_t)i * (uint64_t)symtab_sh->sh_entsize;
        if (off + sizeof(struct elf64_sym) > ctx->file_size) {
            /* Truncated symbol table — stop here */
            ctx->num_syms = i;
            break;
        }

        struct elf64_sym raw;
        memcpy(&raw, data + off, sizeof(raw));

        struct module_elf_sym *sym = &ctx->syms[i];
        sym->shndx = raw.st_shndx;
        sym->value = raw.st_value;
        sym->size  = raw.st_size;
        sym->bind  = ELF64_ST_BIND(raw.st_info);
        sym->type  = ELF64_ST_TYPE(raw.st_info);
        sym->resolved = 0;

        /* Look up name in strtab */
        if (raw.st_name < ctx->strtab_size) {
            sym->name = ctx->strtab + raw.st_name;
        } else {
            sym->name = "";
        }
    }

    return 0;
}

/* ── RELA relocation section parsing (M11) ──────────────────────────── */

static int parse_rela_sections(struct module_elf_context *ctx)
{
    const uint8_t *data = ctx->file_data;
    int num_rela_seen = 0;

    for (int i = 0; i < ctx->num_sections; i++) {
        if (ctx->shdrs[i].sh_type != SHT_RELA)
            continue;

        if (num_rela_seen >= MODULE_ELF_MAX_SECTIONS) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: too many RELA sections");
            return -1;
        }

        /* sh_info = section index that this RELA section applies to */
        uint32_t target_idx = ctx->shdrs[i].sh_info;

        if (target_idx >= (uint32_t)ctx->num_sections) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: RELA section %d has invalid sh_info=%u",
                     i, (unsigned int)target_idx);
            return -1;
        }

        /* Validate RELA entry size */
        if (ctx->shdrs[i].sh_entsize < sizeof(struct elf64_rela)) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: RELA section %d entry too small (%llu bytes)",
                     i, (unsigned long long)ctx->shdrs[i].sh_entsize);
            return -1;
        }

        int num_entries = (int)(ctx->shdrs[i].sh_size / ctx->shdrs[i].sh_entsize);
        if (num_entries > MODULE_ELF_MAX_RELOCS) {
            num_entries = MODULE_ELF_MAX_RELOCS;
        }

        struct {
            int section_idx;
            struct module_elf_rela entries[MODULE_ELF_MAX_RELOCS];
            int count;
        } *rela = &ctx->relas[num_rela_seen];

        rela->section_idx = (int)target_idx;
        rela->count = 0;

        for (int j = 0; j < num_entries; j++) {
            uint64_t off = ctx->shdrs[i].sh_offset +
                (uint64_t)j * (uint64_t)ctx->shdrs[i].sh_entsize;
            if (off + sizeof(struct elf64_rela) > ctx->file_size) {
                /* Truncated — stop */
                break;
            }

            struct elf64_rela raw;
            memcpy(&raw, data + off, sizeof(raw));

            struct module_elf_rela *entry = &rela->entries[rela->count];
            entry->offset  = raw.r_offset;
            entry->type    = ELF64_R_TYPE(raw.r_info);
            entry->sym_idx = (uint32_t)ELF64_R_SYM(raw.r_info);
            entry->addend  = raw.r_addend;
            rela->count++;
        }

        num_rela_seen++;
    }

    ctx->num_rela_sections = num_rela_seen;
    return 0;
}

/* ── Top-level parse ─────────────────────────────────────────────────── */

int module_elf_parse(struct module_elf_context *ctx)
{
    if (!ctx || !ctx->file_data) {
        if (ctx) snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                          "module_elf: NULL context or file data");
        return -1;
    }

    /* Step 1: Parse section headers */
    if (parse_section_headers(ctx) < 0)
        return -1;

    /* Build section descriptors with names */
    for (int i = 0; i < ctx->num_sections; i++) {
        struct module_elf_section *sec = &ctx->sections[i];
        sec->shndx = i;
        sec->sh_type = ctx->shdrs[i].sh_type;
        sec->sh_flags = ctx->shdrs[i].sh_flags;
        sec->file_offset = ctx->shdrs[i].sh_offset;
        sec->file_size = ctx->shdrs[i].sh_size;
        sec->mem_addr = 0;   /* will be set during loading */
        sec->mem_size = 0;
        sec->loaded = 0;

        /* Look up name from shstrtab */
        if (ctx->shdrs[i].sh_name < ctx->shstrtab_size) {
            sec->name = ctx->shstrtab + ctx->shdrs[i].sh_name;
        } else {
            sec->name = "";
        }
    }

    /* Step 2: Parse symbol table */
    if (parse_symbol_table(ctx) < 0)
        return -1;

    /* Step 3: Parse RELA sections */
    if (parse_rela_sections(ctx) < 0)
        return -1;

    /* Try to extract module name from common sections */
    {
        const char *name = NULL;

        /* Look for .modinfo section and parse 'name=' field */
        int modinfo_idx = module_elf_find_section(ctx, ".modinfo");
        if (modinfo_idx >= 0) {
            const char *modinfo_data = (const char *)(ctx->file_data +
                ctx->sections[modinfo_idx].file_offset);
            uint64_t modinfo_size = ctx->sections[modinfo_idx].file_size;

            uint64_t pos = 0;
            while (pos < modinfo_size) {
                if (strncmp(modinfo_data + pos, "name=", 5) == 0) {
                    name = modinfo_data + pos + 5;
                    break;
                }
                /* Skip to next null terminator */
                while (pos < modinfo_size && modinfo_data[pos] != '\0')
                    pos++;
                pos++; /* skip null */
            }
        }

        /* Fall back to first text section name or "unnamed" */
        if (name) {
            strncpy(ctx->name, name, sizeof(ctx->name) - 1);
            ctx->name[sizeof(ctx->name) - 1] = '\0';
        } else if (module_elf_find_section(ctx, ".text") >= 0) {
            snprintf(ctx->name, sizeof(ctx->name), "module");
        } else {
            snprintf(ctx->name, sizeof(ctx->name), "unnamed");
        }
    }

    return 0;
}

/* ── Section lookup ──────────────────────────────────────────────────── */

int module_elf_find_section(const struct module_elf_context *ctx,
                            const char *name)
{
    if (!ctx || !name)
        return -1;

    for (int i = 0; i < ctx->num_sections; i++) {
        if (ctx->sections[i].name &&
            strcmp(ctx->sections[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ── Debug dump ──────────────────────────────────────────────────────── */

void module_elf_print_info(const struct module_elf_context *ctx)
{
    if (!ctx) return;

    kprintf("[MOD_ELF] Module: %s\n", ctx->name);
    kprintf("[MOD_ELF]   File size: %llu bytes\n",
            (unsigned long long)ctx->file_size);
    kprintf("[MOD_ELF]   Sections: %d\n", ctx->num_sections);

    for (int i = 0; i < ctx->num_sections; i++) {
        const struct module_elf_section *sec = &ctx->sections[i];
        kprintf("[MOD_ELF]   [%2d] %s (type=%d flags=0x%llx size=%llu)\n",
                i, sec->name ? sec->name : "?",
                sec->sh_type,
                (unsigned long long)sec->sh_flags,
                (unsigned long long)sec->file_size);
    }

    kprintf("[MOD_ELF]   Symbols: %d\n", ctx->num_syms);
    for (int i = 1; i < ctx->num_syms; i++) { /* skip index 0 (null symbol) */
        const struct module_elf_sym *sym = &ctx->syms[i];
        if (sym->name && sym->name[0]) {
            kprintf("[MOD_ELF]     [%3d] %-32s bind=%d type=%d shndx=%u "
                    "value=0x%llx size=%llu%s\n",
                    i, sym->name, sym->bind, sym->type,
                    (unsigned int)sym->shndx,
                    (unsigned long long)sym->value,
                    (unsigned long long)sym->size,
                    sym->shndx == 0 ? " (UNDEF/import)" : "");
        }
    }

    kprintf("[MOD_ELF]   RELA groups: %d\n", ctx->num_rela_sections);
    for (int r = 0; r < ctx->num_rela_sections; r++) {
        const char *target_name = (ctx->relas[r].section_idx >= 0 &&
            ctx->relas[r].section_idx < ctx->num_sections)
            ? ctx->sections[ctx->relas[r].section_idx].name : "?";
        kprintf("[MOD_ELF]     Group %d: section %d (%s), %d entries\n",
                r,
                ctx->relas[r].section_idx,
                target_name ? target_name : "?",
                ctx->relas[r].count);

        /* Show first 5 entries as a sample */
        int show = ctx->relas[r].count < 5 ? ctx->relas[r].count : 5;
        for (int j = 0; j < show; j++) {
            const struct module_elf_rela *rel = &ctx->relas[r].entries[j];
            const char *sym_name = (rel->sym_idx < (uint32_t)ctx->num_syms)
                ? ctx->syms[rel->sym_idx].name : "?";
            kprintf("[MOD_ELF]       [%3d] off=0x%llx type=%d sym=%d(%s) "
                    "addend=%lld\n",
                    j,
                    (unsigned long long)rel->offset,
                    rel->type, rel->sym_idx,
                    sym_name ? sym_name : "?",
                    (long long)rel->addend);
        }
        if (ctx->relas[r].count > 5) {
            kprintf("[MOD_ELF]       ... (%d more)\n",
                    ctx->relas[r].count - 5);
        }
    }
}

/* ── Cleanup ─────────────────────────────────────────────────────────── */

void module_elf_free(struct module_elf_context *ctx)
{
    if (!ctx) return;
    /* Currently all arrays are static within the context struct,
     * so there is nothing to free.  This hook exists for future
     * dynamic allocations (e.g., if we move to heap-allocated
     * section descriptors for very large modules). */
    memset(ctx, 0, sizeof(*ctx));
}
