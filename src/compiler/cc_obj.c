/* cc_obj.c — Write relocatable ELF64 object file (.o) */

#include "cc.h"
#include "elf.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/*
 * ELF64 relocatable object layout:
 *
 *   ELF header          (64 bytes)
 *   .text section       (code bytes)
 *   .data section       (data bytes)
 *   .symtab section     (symbol table)
 *   .strtab section     (string table for symbols)
 *   .rela.text section  (relocations for .text)
 *   .shstrtab section   (section header string table)
 *   Section headers     (7 entries)
 *
 * Sections:
 *   [0] NULL
 *   [1] .text      (code, SHT_PROGBITS, SHF_ALLOC|SHF_EXECINSTR)
 *   [2] .data      (data, SHT_PROGBITS, SHF_ALLOC|SHF_WRITE)
 *   [3] .symtab    (SHT_SYMTAB)
 *   [4] .strtab    (SHT_STRTAB)
 *   [5] .rela.text (SHT_RELA, info=1 -> .text)
 *   [6] .shstrtab  (SHT_STRTAB)
 */

#define ELF_HDR_SIZE   64
#define SHDR_SIZE      64   /* sizeof(Elf64_Shdr) */
#define SYM_SIZE       24   /* sizeof(Elf64_Sym) */
#define RELA_SIZE      24   /* sizeof(Elf64_Rela) */

/* ELF section types */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4

/* ELF section flags */
#ifndef SHF_WRITE
#define SHF_WRITE     0x1
#endif
#ifndef SHF_ALLOC
#define SHF_ALLOC     0x2
#endif
#ifndef SHF_EXECINSTR
#define SHF_EXECINSTR 0x4
#endif

/* Symbol binding */
#define STB_LOCAL  0
#define STB_GLOBAL 1

/* Symbol type */
#define STT_NOTYPE  0
#define STT_FUNC    2
#define STT_SECTION 3

/* Relocation types */
#define R_X86_64_64    1   /* 64-bit absolute */
#define R_X86_64_PC32  2   /* 32-bit PC-relative */

/* Forward declarations */
static FuncInfo *find_func_by_name(CompilerState *cc, const char *name);

/* Section header indices */
#define SHN_UNDEF  0
#define SEC_TEXT   1
#define SEC_DATA   2
#define SEC_SYMTAB 3
#define SEC_STRTAB 4
#define SEC_RELA   5
#define SEC_SHSTRTAB 6
#define NUM_SECTIONS 7

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}
static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void write_le64(uint8_t *p, uint64_t v) {
    write_le32(p, (uint32_t)(v & 0xffffffff));
    write_le32(p + 4, (uint32_t)(v >> 32));
}

/* Add a string to a strtab buffer, return offset */
static int strtab_add(uint8_t *strtab, int *strtab_len, int max, const char *s) {
    int off = *strtab_len;
    int slen = strlen(s);
    if (off + slen + 1 > max) return 0;
    memcpy(strtab + off, s, slen + 1);
    *strtab_len = off + slen + 1;
    return off;
}

int cc_write_obj(CompilerState *cc, const char *outpath) {
    /* Sizes */
    uint32_t text_size = (uint32_t)cc->code_len;
    uint32_t data_size = (uint32_t)cc->data_len;
    if (data_size == 0) data_size = 1; /* need at least 1 byte */

    /* Build string tables */
    int strtab_max = 32768;
    int shstrtab_max = 256;
    uint8_t *strtab = (uint8_t *)kmalloc(strtab_max);
    uint8_t *shstrtab = (uint8_t *)kmalloc(shstrtab_max);
    if (!strtab || !shstrtab) {
        if (strtab) kfree(strtab);
        if (shstrtab) kfree(shstrtab);
        kprintf("cc_obj: out of memory\n");
        return -1;
    }
    int strtab_len = 0;
    int shstrtab_len = 0;

    /* First byte of strtab must be \0 */
    strtab[strtab_len++] = '\0';
    shstrtab[shstrtab_len++] = '\0';

    /* Section names */
    int nm_text = strtab_add(shstrtab, &shstrtab_len, shstrtab_max, ".text");
    int nm_data = strtab_add(shstrtab, &shstrtab_len, shstrtab_max, ".data");
    int nm_symtab = strtab_add(shstrtab, &shstrtab_len, shstrtab_max, ".symtab");
    int nm_strtab = strtab_add(shstrtab, &shstrtab_len, shstrtab_max, ".strtab");
    int nm_rela = strtab_add(shstrtab, &shstrtab_len, shstrtab_max, ".rela.text");
    int nm_shstrtab = strtab_add(shstrtab, &shstrtab_len, shstrtab_max, ".shstrtab");

    /* Build symbol table */
    /* Symbols: [0]=null, [1]=.text section, [2]=.data section, then functions & globals */
    int sym_max = 3 + cc->nfuncs + cc->nobj_syms;
    int sym_count = 0;
    int sym_buf_size = sym_max * SYM_SIZE;
    uint8_t *sym_buf = (uint8_t *)kmalloc(sym_buf_size);
    if (!sym_buf) { kfree(strtab); kfree(shstrtab); return -1; }
    memset(sym_buf, 0, sym_buf_size);

    /* [0] NULL symbol */
    sym_count++;

    /* [1] .text section symbol */
    uint8_t *sp = sym_buf + sym_count * SYM_SIZE;
    write_le32(sp + 0, 0);            /* st_name = 0 */
    sp[4] = (STB_LOCAL << 4) | STT_SECTION; /* st_info */
    sp[5] = 0;                         /* st_other */
    write_le16(sp + 6, SEC_TEXT);      /* st_shndx */
    write_le64(sp + 8, 0);            /* st_value */
    write_le64(sp + 16, 0);           /* st_size */
    sym_count++;

    /* [2] .data section symbol */
    sp = sym_buf + sym_count * SYM_SIZE;
    write_le32(sp + 0, 0);
    sp[4] = (STB_LOCAL << 4) | STT_SECTION;
    sp[5] = 0;
    write_le16(sp + 6, SEC_DATA);
    write_le64(sp + 8, 0);
    write_le64(sp + 16, 0);
    sym_count++;

    int first_global = sym_count; /* index of first global symbol */

    /* Add defined functions as global symbols */
    for (int i = 0; i < cc->nfuncs; i++) {
        if (!cc->funcs[i].defined) continue;
        sp = sym_buf + sym_count * SYM_SIZE;
        int name_off = strtab_add(strtab, &strtab_len, strtab_max, cc->funcs[i].name);
        write_le32(sp + 0, (uint32_t)name_off);
        sp[4] = (STB_GLOBAL << 4) | STT_FUNC;
        sp[5] = 0;
        write_le16(sp + 6, SEC_TEXT);
        write_le64(sp + 8, (uint64_t)cc->funcs[i].code_offset);
        write_le64(sp + 16, 0);
        sym_count++;
    }

    /* Add undefined functions (forward-referenced but not defined) */
    for (int i = 0; i < cc->nfuncs; i++) {
        if (cc->funcs[i].defined) continue;
        sp = sym_buf + sym_count * SYM_SIZE;
        int name_off = strtab_add(strtab, &strtab_len, strtab_max, cc->funcs[i].name);
        write_le32(sp + 0, (uint32_t)name_off);
        sp[4] = (STB_GLOBAL << 4) | STT_NOTYPE;
        sp[5] = 0;
        write_le16(sp + 6, SHN_UNDEF);
        write_le64(sp + 8, 0);
        write_le64(sp + 16, 0);
        sym_count++;
    }

    /* Add explicit object symbols if any */
    for (int i = 0; i < cc->nobj_syms; i++) {
        sp = sym_buf + sym_count * SYM_SIZE;
        int name_off = strtab_add(strtab, &strtab_len, strtab_max, cc->obj_syms[i].name);
        write_le32(sp + 0, (uint32_t)name_off);
        sp[4] = (uint8_t)((cc->obj_syms[i].binding == CC_SYM_GLOBAL ? STB_GLOBAL : STB_LOCAL) << 4);
        sp[5] = 0;
        write_le16(sp + 6, (uint16_t)cc->obj_syms[i].section);
        write_le64(sp + 8, (uint64_t)cc->obj_syms[i].offset);
        write_le64(sp + 16, 0);
        sym_count++;
    }

    uint32_t symtab_size = (uint32_t)(sym_count * SYM_SIZE);

    /* Build relocation table */
    /* Convert cc->relocs[] + unresolved patches to .rela.text entries */
    int rela_max = cc->nrelocs + cc->npatches;
    int rela_buf_size = rela_max * RELA_SIZE;
    uint8_t *rela_buf = (uint8_t *)kmalloc(rela_buf_size + 1);
    if (!rela_buf) { kfree(strtab); kfree(shstrtab); kfree(sym_buf); return -1; }
    memset(rela_buf, 0, rela_buf_size + 1);
    int rela_count = 0;

    /* Helper: find symbol index by name */
    #define FIND_SYM_IDX(name_str) ({ \
        int _idx = -1; \
        for (int _si = 0; _si < cc->nfuncs; _si++) { \
            if (strcmp(cc->funcs[_si].name, name_str) == 0) { \
                /* Count defined funcs before it + undefined position */ \
                int _fi = first_global; \
                for (int _fj = 0; _fj < _si; _fj++) { \
                    if (cc->funcs[_fj].defined) _fi++; \
                } \
                if (!cc->funcs[_si].defined) { \
                    /* Scan undefined symbols after defined ones */ \
                    _fi = first_global; \
                    for (int _fj = 0; _fj < cc->nfuncs; _fj++) { \
                        if (cc->funcs[_fj].defined) _fi++; \
                    } \
                    for (int _fj = 0; _fj < _si; _fj++) { \
                        if (!cc->funcs[_fj].defined) _fi++; \
                    } \
                } else { \
                    _fi = first_global; \
                    for (int _fj = 0; _fj < _si; _fj++) { \
                        if (cc->funcs[_fj].defined) _fi++; \
                    } \
                } \
                _idx = _fi; \
                break; \
            } \
        } \
        _idx; \
    })

    /* Emit relocations from cc->relocs[] */
    for (int i = 0; i < cc->nrelocs; i++) {
        RelocEntry *re = &cc->relocs[i];
        uint8_t *rp = rela_buf + rela_count * RELA_SIZE;
        write_le64(rp + 0, (uint64_t)re->code_off); /* r_offset */
        int sym_idx = 0;
        uint32_t rtype = 0;
        if (re->type == CC_RELOC_CALL) {
            sym_idx = FIND_SYM_IDX(re->name);
            if (sym_idx < 0) sym_idx = 0;
            rtype = R_X86_64_PC32;
        } else if (re->type == CC_RELOC_ABS64) {
            sym_idx = FIND_SYM_IDX(re->name);
            if (sym_idx < 0) sym_idx = 0;
            rtype = R_X86_64_64;
        } else if (re->type == CC_RELOC_DATA64) {
            sym_idx = 2; /* .data section symbol */
            rtype = R_X86_64_64;
        }
        uint64_t r_info = ((uint64_t)sym_idx << 32) | rtype;
        write_le64(rp + 8, r_info);
        write_le64(rp + 16, (uint64_t)(int64_t)re->addend);
        rela_count++;
    }

    /* Also emit relocations for unresolved patches (function calls to undefined) */
    for (int i = 0; i < cc->npatches; i++) {
        FuncInfo *f = find_func_by_name(cc, cc->patches[i].name);
        if (f && f->defined) continue; /* already resolved */
        uint8_t *rp = rela_buf + rela_count * RELA_SIZE;
        write_le64(rp + 0, (uint64_t)cc->patches[i].code_off);
        int sym_idx = FIND_SYM_IDX(cc->patches[i].name);
        if (sym_idx < 0) {
            /* Need to add as undefined symbol */
            sym_idx = 0;
        }
        uint64_t r_info = ((uint64_t)sym_idx << 32) | R_X86_64_PC32;
        write_le64(rp + 8, r_info);
        write_le64(rp + 16, (uint64_t)(int64_t)(-4)); /* addend = -4 for call rel32 */
        rela_count++;
    }

    #undef FIND_SYM_IDX

    uint32_t rela_size = (uint32_t)(rela_count * RELA_SIZE);

    /* Compute file layout */
    uint32_t off = ELF_HDR_SIZE;
    uint32_t text_off = off;           off += text_size;
    uint32_t data_off = off;           off += data_size;
    uint32_t symtab_off = off;         off += symtab_size;
    uint32_t strtab_off = off;         off += (uint32_t)strtab_len;
    uint32_t rela_off = off;           off += rela_size;
    uint32_t shstrtab_off = off;       off += (uint32_t)shstrtab_len;
    uint32_t shdr_off = (off + 7) & ~7U; /* align to 8 */
    uint32_t total_size = shdr_off + NUM_SECTIONS * SHDR_SIZE;

    /* Allocate and fill output buffer */
    uint8_t *buf = (uint8_t *)kmalloc(total_size);
    if (!buf) { kfree(strtab); kfree(shstrtab); kfree(sym_buf); kfree(rela_buf); return -1; }
    memset(buf, 0, total_size);

    /* ELF header */
    uint8_t *hdr = buf;
    hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
    hdr[4] = ELF_CLASS64;
    hdr[5] = ELF_DATA2LSB;
    hdr[6] = 1;               /* EV_CURRENT */
    hdr[7] = 0;               /* ELFOSABI_NONE */
    write_le16(hdr + 16, 1);  /* ET_REL */
    write_le16(hdr + 18, EM_X86_64);
    write_le32(hdr + 20, 1);  /* EV_CURRENT */
    write_le64(hdr + 24, 0);  /* e_entry (none for .o) */
    write_le64(hdr + 32, 0);  /* e_phoff (no program headers) */
    write_le64(hdr + 40, (uint64_t)shdr_off); /* e_shoff */
    write_le32(hdr + 48, 0);  /* e_flags */
    write_le16(hdr + 52, ELF_HDR_SIZE);
    write_le16(hdr + 54, 0);  /* e_phentsize */
    write_le16(hdr + 56, 0);  /* e_phnum */
    write_le16(hdr + 58, SHDR_SIZE);
    write_le16(hdr + 60, NUM_SECTIONS);
    write_le16(hdr + 62, SEC_SHSTRTAB); /* e_shstrndx */

    /* Copy section data */
    memcpy(buf + text_off, cc->code, text_size);
    memcpy(buf + data_off, cc->data, data_size > (uint32_t)cc->data_len ? (uint32_t)cc->data_len : data_size);
    memcpy(buf + symtab_off, sym_buf, symtab_size);
    memcpy(buf + strtab_off, strtab, strtab_len);
    memcpy(buf + rela_off, rela_buf, rela_size);
    memcpy(buf + shstrtab_off, shstrtab, shstrtab_len);

    /* Section headers */
    uint8_t *sh = buf + shdr_off;

    /* [0] NULL */
    /* already zeroed */

    /* [1] .text */
    sh += SHDR_SIZE;
    write_le32(sh + 0, (uint32_t)nm_text);    /* sh_name */
    write_le32(sh + 4, SHT_PROGBITS);          /* sh_type */
    write_le64(sh + 8, SHF_ALLOC | SHF_EXECINSTR); /* sh_flags */
    write_le64(sh + 16, 0);                     /* sh_addr */
    write_le64(sh + 24, text_off);              /* sh_offset */
    write_le64(sh + 32, text_size);             /* sh_size */
    write_le32(sh + 40, 0);                     /* sh_link */
    write_le32(sh + 44, 0);                     /* sh_info */
    write_le64(sh + 48, 16);                    /* sh_addralign */
    write_le64(sh + 56, 0);                     /* sh_entsize */

    /* [2] .data */
    sh += SHDR_SIZE;
    write_le32(sh + 0, (uint32_t)nm_data);
    write_le32(sh + 4, SHT_PROGBITS);
    write_le64(sh + 8, SHF_ALLOC | SHF_WRITE);
    write_le64(sh + 16, 0);
    write_le64(sh + 24, data_off);
    write_le64(sh + 32, data_size);
    write_le32(sh + 40, 0);
    write_le32(sh + 44, 0);
    write_le64(sh + 48, 8);
    write_le64(sh + 56, 0);

    /* [3] .symtab */
    sh += SHDR_SIZE;
    write_le32(sh + 0, (uint32_t)nm_symtab);
    write_le32(sh + 4, SHT_SYMTAB);
    write_le64(sh + 8, 0);
    write_le64(sh + 16, 0);
    write_le64(sh + 24, symtab_off);
    write_le64(sh + 32, symtab_size);
    write_le32(sh + 40, SEC_STRTAB);           /* sh_link -> .strtab */
    write_le32(sh + 44, (uint32_t)first_global); /* sh_info = first global sym idx */
    write_le64(sh + 48, 8);
    write_le64(sh + 56, SYM_SIZE);

    /* [4] .strtab */
    sh += SHDR_SIZE;
    write_le32(sh + 0, (uint32_t)nm_strtab);
    write_le32(sh + 4, SHT_STRTAB);
    write_le64(sh + 8, 0);
    write_le64(sh + 16, 0);
    write_le64(sh + 24, strtab_off);
    write_le64(sh + 32, (uint64_t)strtab_len);
    write_le32(sh + 40, 0);
    write_le32(sh + 44, 0);
    write_le64(sh + 48, 1);
    write_le64(sh + 56, 0);

    /* [5] .rela.text */
    sh += SHDR_SIZE;
    write_le32(sh + 0, (uint32_t)nm_rela);
    write_le32(sh + 4, SHT_RELA);
    write_le64(sh + 8, 0);
    write_le64(sh + 16, 0);
    write_le64(sh + 24, rela_off);
    write_le64(sh + 32, rela_size);
    write_le32(sh + 40, SEC_SYMTAB);          /* sh_link -> .symtab */
    write_le32(sh + 44, SEC_TEXT);             /* sh_info -> .text */
    write_le64(sh + 48, 8);
    write_le64(sh + 56, RELA_SIZE);

    /* [6] .shstrtab */
    sh += SHDR_SIZE;
    write_le32(sh + 0, (uint32_t)nm_shstrtab);
    write_le32(sh + 4, SHT_STRTAB);
    write_le64(sh + 8, 0);
    write_le64(sh + 16, 0);
    write_le64(sh + 24, shstrtab_off);
    write_le64(sh + 32, (uint64_t)shstrtab_len);
    write_le32(sh + 40, 0);
    write_le32(sh + 44, 0);
    write_le64(sh + 48, 1);
    write_le64(sh + 56, 0);

    /* Write to filesystem */
    int ret = vfs_write(outpath, buf, total_size);

    kfree(buf);
    kfree(strtab);
    kfree(shstrtab);
    kfree(sym_buf);
    kfree(rela_buf);

    if (ret < 0) {
        kprintf("cc_obj: failed to write %s\n", outpath);
        return -1;
    }
    kprintf("cc_obj: wrote %u bytes to %s (%d syms, %d relocs)\n",
            total_size, outpath, sym_count, rela_count);
    return 0;
}

/* Helper used by cc_obj: find function info by name */
static FuncInfo *find_func_by_name(CompilerState *cc, const char *name) {
    for (int i = 0; i < cc->nfuncs; i++)
        if (strcmp(cc->funcs[i].name, name) == 0)
            return &cc->funcs[i];
    return 0;
}

/* ── Stub: cc_obj_create ─────────────────────────────── */
int cc_obj_create(void *obj)
{
    (void)obj;
    kprintf("[cc] cc_obj_create: not yet implemented\n");
    return -EINVAL;
}
/* ── Stub: cc_obj_add_section ─────────────────────────────── */
int cc_obj_add_section(void *obj, const char *name, void *data, size_t len)
{
    (void)obj;
    (void)name;
    (void)data;
    (void)len;
    kprintf("[cc] cc_obj_add_section: not yet implemented\n");
    return -EINVAL;
}
/* ── Stub: cc_obj_add_symbol ─────────────────────────────── */
int cc_obj_add_symbol(void *obj, const char *name, void *addr)
{
    (void)obj;
    (void)name;
    (void)addr;
    kprintf("[cc] cc_obj_add_symbol: not yet implemented\n");
    return -EINVAL;
}
