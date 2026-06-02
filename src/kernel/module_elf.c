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

/* ── Integer limit constants (not all compilers provide <stdint.h>
 *     in -ffreestanding, and types.h only defines INT32_MAX). ─────── */
#ifndef INT32_MIN
#define INT32_MIN  (-2147483647 - 1)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 4294967295U
#endif

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

        struct module_elf_rela_group *rela = &ctx->relas[num_rela_seen];

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

    /* Try to extract module name and verify vermagic from .modinfo */
    {
        const char *name = NULL;
        const char *mod_vermagic = NULL;

        /* Look for .modinfo section and parse key=value fields */
        int modinfo_idx = module_elf_find_section(ctx, ".modinfo");
        if (modinfo_idx >= 0) {
            const char *modinfo_data = (const char *)(ctx->file_data +
                ctx->sections[modinfo_idx].file_offset);
            uint64_t modinfo_size = ctx->sections[modinfo_idx].file_size;

            uint64_t pos = 0;
            while (pos < modinfo_size) {
                if (strncmp(modinfo_data + pos, "name=", 5) == 0) {
                    name = modinfo_data + pos + 5;
                } else if (strncmp(modinfo_data + pos, "vermagic=", 9) == 0) {
                    mod_vermagic = modinfo_data + pos + 9;
                }
                /* Skip to next null terminator */
                while (pos < modinfo_size && modinfo_data[pos] != '\0')
                    pos++;
                pos++; /* skip null */
            }
        }

        /* ── Vermagic validation ────────────────────────────────────
         * If the module has a vermagic string embedded, check it against
         * the running kernel's VERMAGIC_STRING.  A mismatch indicates
         * the module was built for a different kernel version or with
         * different configuration flags (SMP, preempt, etc.) and could
         * cause subtle ABI corruption. */
        if (mod_vermagic) {
            if (strcmp(mod_vermagic, VERMAGIC_STRING) != 0) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "module_elf: vermagic mismatch for '%s': "
                         "kernel has \"%s\", module has \"%s\"",
                         name ? name : "?",
                         VERMAGIC_STRING, mod_vermagic);
                return -1;
            }
            kprintf("[MOD_ELF] Vermagic OK for '%s' (%s)\n",
                    name ? name : "?", VERMAGIC_STRING);
        } else {
            /* Modules without vermagic are allowed but warned.
             * During transition, not all modules may have it yet. */
            kprintf("[MOD_ELF] WARNING: '%s' has no vermagic field\n",
                    name ? name : ctx->name);
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

/* ══════════════════════════════════════════════════════════════════════
 *  M13 — Symbol resolver: connect module imports to kernel exports
 * ══════════════════════════════════════════════════════════════════════ */

int module_elf_resolve(struct module_elf_context *ctx, int gpl_ok)
{
    if (!ctx || !ctx->file_data) {
        if (ctx) snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                          "module_elf_resolve: NULL context");
        return -1;
    }

    int unresolved = 0;

    for (int i = 1; i < ctx->num_syms; i++) { /* skip index 0 (null symbol) */
        struct module_elf_sym *sym = &ctx->syms[i];

        /* Only resolve symbols that are undefined (imported) */
        if (sym->shndx != SHN_UNDEF) {
            /* Local/defined symbols: mark as resolved with their value */
            sym->resolved = 1;
            continue;
        }

        /* Skip unnamed symbols (section-relative, etc.) */
        if (!sym->name || sym->name[0] == '\0') {
            sym->resolved = 1;
            sym->value = 0;
            continue;
        }

        /* Look up in kernel export table */
        uint64_t addr = find_ksym(sym->name, gpl_ok);
        if (addr != 0) {
            sym->value = addr;
            sym->resolved = 1;
            continue;
        }

        /* Weak symbols: unresolved is OK — treat as 0 */
        if (sym->bind == STB_WEAK) {
            sym->value = 0;
            sym->resolved = 1;
            continue;
        }

        /* Unresolved required symbol — collect first error */
        if (unresolved == 0) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: unresolved symbol '%s' (index %d)",
                     sym->name, i);
        }
        unresolved++;
    }

    if (unresolved > 0) {
        /* Append total count if more than one */
        if (unresolved > 1) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), " (+%d more unresolved symbols)",
                     unresolved - 1);
            size_t cur = strlen(ctx->error_msg);
            size_t max = sizeof(ctx->error_msg) - cur - 1;
            if (cur < sizeof(ctx->error_msg) - 1) {
                strncat(ctx->error_msg, tmp, max);
            }
        }
        return -1;
    }

    kprintf("[MOD_ELF] Resolved %d symbols (including %d imports)\n",
            ctx->num_syms - 1, 0); /* second count is for imports only; skip detail */
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  M12 (continued) — load PROGBITS sections to module memory, zero BSS
 * ══════════════════════════════════════════════════════════════════════ */

uint64_t module_elf_load_sections(struct module_elf_context *ctx,
                                   uint64_t *total_out)
{
    if (!ctx || !ctx->file_data) {
        if (ctx) snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                          "module_elf_load_sections: NULL context");
        return 0;
    }

    /* ── Phase 1: Calculate layout ─────────────────────────────────────
     *
     * Walk all sections; loadable ones (SHF_ALLOC) get a slot in module
     * memory.  We lay them out sequentially with page-alignment for
     * per-section permission changes.  Sections that are not SHF_ALLOC
     * (debug, symbol tables, string tables, etc.) are not loaded.
     */
    uint64_t total = 0;
    int loadable_count = 0;

    for (int i = 0; i < ctx->num_sections; i++) {
        struct module_elf_section *sec = &ctx->sections[i];

        if (!(sec->sh_flags & SHF_ALLOC))
            continue;

        /* Determine memory size: for NOBITS it's sh_size; for PROGBITS
         * it's the max of file_size and sh_size (usually they match). */
        uint64_t mem_size;
        if (sec->sh_type == SHT_NOBITS) {
            mem_size = ctx->shdrs[i].sh_size;
        } else {
            mem_size = ctx->shdrs[i].sh_size;
            if (sec->file_size > mem_size)
                mem_size = sec->file_size;
        }

        /* Page-align the start of each section so we can set per-section
         * page permissions independently. */
        total = (total + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        sec->mem_addr = total;  /* offset within module region (relative) */
        sec->mem_size = (mem_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        sec->loaded = 0;

        total += sec->mem_size;
        loadable_count++;
    }

    if (loadable_count == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: no loadable (SHF_ALLOC) sections found");
        return 0;
    }

    if (total > MODULES_SIZE) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: module too large (%llu bytes, max %llu)",
                 (unsigned long long)total,
                 (unsigned long long)MODULES_SIZE);
        return 0;
    }

    /* ── Phase 2: Allocate module memory (RW initially for patching) ─── */
    uint64_t base_vaddr = module_alloc_region(total,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    if (base_vaddr == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: failed to allocate %llu bytes in module region",
                 (unsigned long long)total);
        return 0;
    }

    /* ── Phase 3: Copy data and zero BSS ─────────────────────────────── */
    uint64_t offset = 0;
    for (int i = 0; i < ctx->num_sections; i++) {
        struct module_elf_section *sec = &ctx->sections[i];

        if (!(sec->sh_flags & SHF_ALLOC))
            continue;

        /* Align to page boundary */
        offset = (offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        uint64_t vaddr = base_vaddr + offset;
        sec->mem_addr = vaddr;
        sec->mem_size = ctx->shdrs[i].sh_size;  /* actual content size */

        if (sec->sh_type == SHT_NOBITS) {
            /* BSS: zero-fill */
            memset((void *)vaddr, 0, ctx->shdrs[i].sh_size);
        } else if (sec->sh_type == SHT_PROGBITS) {
            /* Copy data from file */
            if (sec->file_offset + sec->file_size > ctx->file_size) {
                /* Truncated section — zero-fill what we can and error */
                uint64_t copy_size = ctx->file_size - sec->file_offset;
                if (copy_size > sec->file_size)
                    copy_size = sec->file_size;
                memcpy((void *)vaddr,
                       ctx->file_data + sec->file_offset, copy_size);
                if (sec->file_size > copy_size)
                    memset((void *)(vaddr + copy_size), 0,
                           sec->file_size - copy_size);
            } else {
                memcpy((void *)vaddr,
                       ctx->file_data + sec->file_offset, sec->file_size);
            }
            /* Zero-fill any remaining memory beyond file_size (padding) */
            uint64_t pg_size = (ctx->shdrs[i].sh_size + PAGE_SIZE - 1)
                               & ~(PAGE_SIZE - 1);
            if (ctx->shdrs[i].sh_size < pg_size) {
                memset((void *)(vaddr + ctx->shdrs[i].sh_size), 0,
                       pg_size - ctx->shdrs[i].sh_size);
            }
        }
        /* Round up the stored mem_size to page boundary for later
         * permission changes */
        sec->mem_size = (ctx->shdrs[i].sh_size + PAGE_SIZE - 1)
                        & ~(PAGE_SIZE - 1);
        sec->loaded = 1;

        offset += sec->mem_size;

        kprintf("[MOD_ELF]   Loaded section '%s' at 0x%llx (size=%llu)\n",
                sec->name ? sec->name : "?",
                (unsigned long long)vaddr,
                (unsigned long long)ctx->shdrs[i].sh_size);
    }

    if (total_out)
        *total_out = total;

    kprintf("[MOD_ELF] Loaded %d sections, total %llu bytes at 0x%llx\n",
            loadable_count, (unsigned long long)total,
            (unsigned long long)base_vaddr);
    return base_vaddr;
}

/* ══════════════════════════════════════════════════════════════════════
 *  M14 — Relocation applier
 * ══════════════════════════════════════════════════════════════════════ */

int module_elf_apply_rela(struct module_elf_context *ctx)
{
    if (!ctx || !ctx->file_data) {
        if (ctx) snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                          "module_elf_apply_rela: NULL context");
        return -1;
    }

    if (ctx->num_rela_sections == 0)
        return 0; /* No relocations to apply — success */

    int applied = 0;

    for (int r = 0; r < ctx->num_rela_sections; r++) {
        int target_idx = ctx->relas[r].section_idx;
        if (target_idx < 0 || target_idx >= ctx->num_sections) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: RELA group %d has invalid target section %d",
                     r, target_idx);
            return -1;
        }

        /* Target section must be loaded */
        struct module_elf_section *target = &ctx->sections[target_idx];
        if (!target->loaded || target->mem_addr == 0) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "module_elf: RELA group %d targets unloaded section %d",
                     r, target_idx);
            return -1;
        }

        int count = ctx->relas[r].count;
        for (int j = 0; j < count; j++) {
            const struct module_elf_rela *rel = &ctx->relas[r].entries[j];

            /* Calculate the location to patch (absolute virtual address) */
            uint64_t patch_addr = target->mem_addr + rel->offset;

            /* Get the symbol value */
            if (rel->sym_idx >= (uint32_t)ctx->num_syms) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "module_elf: RELA entry (group %d, entry %d) "
                         "has out-of-range sym_idx %u",
                         r, j, (unsigned int)rel->sym_idx);
                return -1;
            }

            struct module_elf_sym *sym = &ctx->syms[rel->sym_idx];
            if (!sym->resolved) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "module_elf: RELA entry references unresolved "
                         "symbol '%s' (index %u)",
                         sym->name ? sym->name : "?",
                         (unsigned int)rel->sym_idx);
                return -1;
            }

            uint64_t S = sym->value;   /* resolved symbol address */
            int64_t  A = rel->addend;  /* addend from RELA entry */
            uint64_t P = patch_addr;   /* location being patched */

            switch (rel->type) {
            case R_X86_64_NONE:
                /* No action required */
                break;

            case R_X86_64_64: {
                /* S + A: 64-bit absolute value */
                uint64_t value = S + (uint64_t)A;
                *(volatile uint64_t *)patch_addr = value;
                break;
            }

            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                /* S + A - P: 32-bit PC-relative offset */
                int64_t value = (int64_t)(S + (uint64_t)A - P);
                if (value < INT32_MIN || value > INT32_MAX) {
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                             "module_elf: R_X86_64_PC32 overflow at "
                             "0x%llx (value=%lld, sym='%s')",
                             (unsigned long long)patch_addr,
                             (long long)value,
                             sym->name ? sym->name : "?");
                    return -1;
                }
                *(volatile int32_t *)patch_addr = (int32_t)value;
                break;
            }

            case R_X86_64_32: {
                /* S + A: 32-bit absolute (zero-extended) */
                uint64_t value = S + (uint64_t)A;
                if (value > UINT32_MAX) {
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                             "module_elf: R_X86_64_32 overflow at "
                             "0x%llx (value=0x%llx, sym='%s')",
                             (unsigned long long)patch_addr,
                             (unsigned long long)value,
                             sym->name ? sym->name : "?");
                    return -1;
                }
                *(volatile uint32_t *)patch_addr = (uint32_t)value;
                break;
            }

            case R_X86_64_32S: {
                /* S + A: 32-bit absolute (sign-extended) */
                int64_t value = (int64_t)(S + (uint64_t)A);
                if (value < INT32_MIN || value > INT32_MAX) {
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                             "module_elf: R_X86_64_32S overflow at "
                             "0x%llx (value=%lld, sym='%s')",
                             (unsigned long long)patch_addr,
                             (long long)value,
                             sym->name ? sym->name : "?");
                    return -1;
                }
                *(volatile int32_t *)patch_addr = (int32_t)value;
                break;
            }

            case R_X86_64_PC64: {
                /* S + A - P: 64-bit PC-relative offset */
                int64_t value = (int64_t)(S + (uint64_t)A - P);
                *(volatile int64_t *)patch_addr = value;
                break;
            }

            default:
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "module_elf: unsupported relocation type %d "
                         "at 0x%llx (sym='%s')",
                         rel->type,
                         (unsigned long long)patch_addr,
                         sym->name ? sym->name : "?");
                return -1;
            }

            applied++;
        }
    }

    kprintf("[MOD_ELF] Applied %d relocations across %d groups\n",
            applied, ctx->num_rela_sections);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  M15 — Set per-section page permissions
 * ══════════════════════════════════════════════════════════════════════ */

static uint64_t section_to_vmm_flags(uint64_t sh_flags)
{
    uint64_t flags = VMM_FLAG_PRESENT;

    if (sh_flags & SHF_WRITE)
        flags |= VMM_FLAG_WRITE;

    /* Executable sections do NOT have NX set */
    if (!(sh_flags & SHF_EXECINSTR))
        flags |= VMM_FLAG_NOEXEC;

    return flags;
}

int module_elf_set_perms(struct module_elf_context *ctx)
{
    if (!ctx) {
        return -1;
    }

    for (int i = 0; i < ctx->num_sections; i++) {
        struct module_elf_section *sec = &ctx->sections[i];

        if (!sec->loaded || sec->mem_addr == 0 || sec->mem_size == 0)
            continue;

        uint64_t flags = section_to_vmm_flags(sec->sh_flags);
        uint64_t pages = sec->mem_size / PAGE_SIZE;
        uint64_t vaddr = sec->mem_addr;

        for (uint64_t p = 0; p < pages; p++) {
            /* Get the physical address currently mapped and remap with
             * new flags.  We cannot use module_alloc_region with different
             * flags per page, so we update the page table entries directly. */
            uint64_t phys = vmm_get_physaddr(vaddr + p * PAGE_SIZE);
            if (phys == 0) {
                /* Page not mapped — skip (should not happen for loaded sections) */
                continue;
            }

            if (vmm_map_page(vaddr + p * PAGE_SIZE, phys, flags) < 0) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                         "module_elf: failed to set permissions on "
                         "section '%s' at 0x%llx",
                         sec->name ? sec->name : "?",
                         (unsigned long long)(vaddr + p * PAGE_SIZE));
                return -1;
            }
        }
    }

    kprintf("[MOD_ELF] Set page permissions on loaded sections\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  M16 — Module finalization (init + register)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Find the module init function in the symbol table.
 * Standard .ko files use the symbol name "init_module".
 */
static uint64_t find_module_entry(const struct module_elf_context *ctx)
{
    for (int i = 1; i < ctx->num_syms; i++) {
        const struct module_elf_sym *sym = &ctx->syms[i];
        if (sym->name && sym->resolved &&
            sym->value != 0 &&
            strcmp(sym->name, "init_module") == 0) {
            return sym->value;
        }
    }
    return 0;
}

int module_elf_finalize(struct module_elf_context *ctx, const char *name)
{
    if (!ctx || !ctx->file_data) {
        if (ctx) snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                          "module_elf_finalize: NULL context");
        return -1;
    }

    /* Step 1: Resolve symbols */
    kprintf("[MOD_ELF] Resolving symbols for '%s'...\n",
            name ? name : ctx->name);
    if (module_elf_resolve(ctx, 1) < 0) {
        /* error_msg already set */
        return -1;
    }

    /* Step 2: Load sections to module memory */
    uint64_t total_size = 0;
    uint64_t base = module_elf_load_sections(ctx, &total_size);
    if (base == 0) {
        /* error_msg already set */
        return -1;
    }

    /* Step 3: Apply relocations */
    kprintf("[MOD_ELF] Applying relocations for '%s'...\n",
            name ? name : ctx->name);
    if (module_elf_apply_rela(ctx) < 0) {
        /* Free the allocated region on failure */
        if (base != 0 && total_size > 0)
            module_free_region(base, total_size);
        return -1;
    }

    /* Step 4: Set final page permissions */
    kprintf("[MOD_ELF] Setting permissions for '%s'...\n",
            name ? name : ctx->name);
    if (module_elf_set_perms(ctx) < 0) {
        if (base != 0 && total_size > 0)
            module_free_region(base, total_size);
        return -1;
    }

    /* Step 5: Find the init function */
    uint64_t entry_addr = find_module_entry(ctx);
    if (entry_addr == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: no 'init_module' symbol found in '%s'",
                 name ? name : ctx->name);
        if (base != 0 && total_size > 0)
            module_free_region(base, total_size);
        return -1;
    }

    module_entry_t entry_fn = (module_entry_t)entry_addr;

    /* Step 6: Register with module system */
    const char *mod_name = name ? name : ctx->name;
    int mod_id = module_load(mod_name, entry_fn);
    if (mod_id < 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: failed to register module '%s' "
                 "(duplicate or full table)", mod_name);
        if (base != 0 && total_size > 0)
            module_free_region(base, total_size);
        return -1;
    }

    /* Record the module's base address and size for later unload */
    struct kernel_module *mod = module_find(mod_name);
    if (mod) {
        mod->base_addr = base;
        mod->size = total_size;
    }

    /* Step 7: Call the init function */
    kprintf("[MOD] Initializing '%s' (entry=0x%llx)...\n",
            mod_name, (unsigned long long)entry_addr);

    int init_ret = entry_fn();

    if (init_ret != 0) {
        kprintf("[MOD] ERROR: '%s' init returned %d — unloading\n",
                mod_name, init_ret);
        module_unload(mod_id);
        if (base != 0 && total_size > 0)
            module_free_region(base, total_size);
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "module_elf: init function for '%s' returned %d",
                 mod_name, init_ret);
        return -1;
    }

    kprintf("[MOD] Loaded: %s (id=%d, base=0x%llx, size=%llu)\n",
            mod_name, mod_id,
            (unsigned long long)base, (unsigned long long)total_size);
    return mod_id;
}
