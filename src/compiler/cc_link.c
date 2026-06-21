/* cc_link.c — ELF64 linker for the in-kernel C compiler
 *
 * Reads multiple ELF64 relocatable objects (.o files), resolves symbols,
 * applies relocations, and writes a static ELF64 executable.
 */

#include "cc.h"
#include "elf.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/* Local constants matching cc_obj.c definitions */
#define ELF_HDR_SIZE   64
#define SHDR_SIZE      64
#define SYM_SIZE       24
#define RELA_SIZE      24
#define PHDR_SIZE      56
#define PAGE_SZ        0x1000U

#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_RELA       4

#ifndef SHF_WRITE
#define SHF_WRITE      0x1
#endif
#ifndef SHF_ALLOC
#define SHF_ALLOC      0x2
#endif
#ifndef SHF_EXECINSTR
#define SHF_EXECINSTR  0x4
#endif

#define STB_LOCAL      0
#define STB_GLOBAL     1

#define R_X86_64_64    1
#define R_X86_64_PC32  2

/* Maximum objects and symbols for linking */
#define LINK_MAX_OBJS     256
#define LINK_MAX_SYMBOLS  4096
#define LINK_MAX_TEXT     (2 * 1024 * 1024)  /* 2MB combined text */
#define LINK_MAX_DATA     (512 * 1024)       /* 512KB combined data */

/* Linker symbol table entry */
typedef struct {
    char     name[32];
    int      defined;     /* 1=defined, 0=undefined */
    int      section;     /* 1=text, 2=data */
    uint64_t value;       /* final virtual address */
    int      obj_idx;     /* which object defined it */
} LinkSymbol;

/* Per-object info */
typedef struct {
    uint32_t text_offset;   /* offset of this obj's .text in merged text */
    uint32_t data_offset;   /* offset of this obj's .data in merged data */
    uint32_t text_size;
    uint32_t data_size;
} ObjInfo;

static void lnk_write_le16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}
static void lnk_write_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void lnk_write_le64(uint8_t *p, uint64_t v) {
    lnk_write_le32(p, (uint32_t)(v & 0xffffffff));
    lnk_write_le32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t lnk_read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t lnk_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t lnk_read_le64(const uint8_t *p) {
    return (uint64_t)lnk_read_le32(p) | ((uint64_t)lnk_read_le32(p + 4) << 32);
}

/* Find or add a symbol in the linker symbol table */
static int link_find_sym(LinkSymbol *syms, int *nsyms, const char *name) {
    for (int i = 0; i < *nsyms; i++)
        if (strcmp(syms[i].name, name) == 0) return i;
    return -1;
}

static int link_add_sym(LinkSymbol *syms, int *nsyms, const char *name,
                        int defined, int section, uint64_t value, int obj_idx) {
    int idx = link_find_sym(syms, nsyms, name);
    if (idx >= 0) {
        if (defined && !syms[idx].defined) {
            syms[idx].defined = 1;
            syms[idx].section = section;
            syms[idx].value = value;
            syms[idx].obj_idx = obj_idx;
        } else if (defined && syms[idx].defined) {
            kprintf("ld: duplicate symbol: %s\n", name);
            return -1;
        }
        return idx;
    }
    if (*nsyms >= LINK_MAX_SYMBOLS) {
        kprintf("ld: too many symbols\n");
        return -1;
    }
    idx = *nsyms;
    strncpy(syms[idx].name, name, 31);
    syms[idx].defined = defined;
    syms[idx].section = section;
    syms[idx].value = value;
    syms[idx].obj_idx = obj_idx;
    (*nsyms)++;
    return idx;
}

int cc_link(const char **obj_paths, int nobj, const char *outpath,
            uint64_t load_base) {
    if (nobj <= 0 || nobj > LINK_MAX_OBJS) {
        kprintf("ld: invalid number of objects (%d)\n", nobj);
        return -1;
    }
    if (load_base == 0) load_base = CC_LOAD_BASE;

    /* Allocate working buffers */
    uint8_t *merged_text = (uint8_t *)kmalloc(LINK_MAX_TEXT);
    uint8_t *merged_data = (uint8_t *)kmalloc(LINK_MAX_DATA);
    LinkSymbol *syms = (LinkSymbol *)kmalloc(LINK_MAX_SYMBOLS * sizeof(LinkSymbol));
    ObjInfo *objs = (ObjInfo *)kmalloc(nobj * sizeof(ObjInfo));

    if (!merged_text || !merged_data || !syms || !objs) {
        kprintf("ld: out of memory\n");
        goto fail_alloc;
    }
    memset(merged_text, 0, LINK_MAX_TEXT);
    memset(merged_data, 0, LINK_MAX_DATA);
    memset(syms, 0, LINK_MAX_SYMBOLS * sizeof(LinkSymbol));
    memset(objs, 0, nobj * sizeof(ObjInfo));

    int nsyms = 0;
    uint32_t text_cursor = 0;
    uint32_t data_cursor = 0;

    /* Pass 1: Read all objects, concatenate .text/.data, collect symbols */
    for (int oi = 0; oi < nobj; oi++) {
        uint32_t file_sz = 0;
        uint8_t *file_buf = 0;

        /* Read object file */
        struct vfs_stat st;
        memset(&st, 0, sizeof(st));
        if (vfs_stat(obj_paths[oi], &st) < 0 || st.size <= 0) {
            kprintf("ld: cannot stat %s\n", obj_paths[oi]);
            goto fail;
        }
        file_sz = st.size;
        file_buf = (uint8_t *)kmalloc(file_sz);
        if (!file_buf) { kprintf("ld: out of memory for %s\n", obj_paths[oi]); goto fail; }

        uint32_t read_sz = 0;
        if (vfs_read(obj_paths[oi], file_buf, file_sz, &read_sz) < 0 || read_sz < ELF_HDR_SIZE) {
            kprintf("ld: cannot read %s\n", obj_paths[oi]);
            kfree(file_buf);
            goto fail;
        }

        /* Validate ELF header */
        if (file_buf[0] != 0x7f || file_buf[1] != 'E' ||
            file_buf[2] != 'L' || file_buf[3] != 'F') {
            kprintf("ld: %s: not an ELF file\n", obj_paths[oi]);
            kfree(file_buf);
            goto fail;
        }
        uint16_t etype = lnk_read_le16(file_buf + 16);
        if (etype != 1) { /* ET_REL */
            kprintf("ld: %s: not a relocatable object (type=%d)\n", obj_paths[oi], etype);
            kfree(file_buf);
            goto fail;
        }

        /* Parse section headers */
        uint64_t shoff = lnk_read_le64(file_buf + 40);
        uint16_t shnum = lnk_read_le16(file_buf + 60);
        uint16_t shstrndx = lnk_read_le16(file_buf + 62);

        if (shoff == 0 || shnum == 0) {
            kprintf("ld: %s: no section headers\n", obj_paths[oi]);
            kfree(file_buf);
            goto fail;
        }

        /* Find .shstrtab for section name resolution */
        const uint8_t *shstrtab_data = 0;
        if (shstrndx < shnum) {
            const uint8_t *sh = file_buf + shoff + shstrndx * SHDR_SIZE;
            uint64_t soff = lnk_read_le64(sh + 24);
            shstrtab_data = file_buf + soff;
        }

        /* Locate sections by name */
        uint64_t text_off = 0, text_sz = 0;
        uint64_t data_off = 0, data_sz = 0;
        uint64_t symtab_off = 0, symtab_sz = 0;
        uint64_t strtab_off = 0, strtab_sz = 0;
        uint64_t rela_off = 0, rela_sz = 0;
        uint32_t symtab_link = 0;

        for (int si = 0; si < shnum; si++) {
            const uint8_t *sh = file_buf + shoff + si * SHDR_SIZE;
            uint32_t sh_name = lnk_read_le32(sh + 0);
            uint32_t sh_type = lnk_read_le32(sh + 4);
            uint64_t sh_offset = lnk_read_le64(sh + 24);
            uint64_t sh_size = lnk_read_le64(sh + 32);

            const char *name = shstrtab_data ? (const char *)(shstrtab_data + sh_name) : "";

            if (sh_type == SHT_PROGBITS && (lnk_read_le64(sh + 8) & SHF_EXECINSTR)) {
                text_off = sh_offset; text_sz = sh_size;
            } else if (sh_type == SHT_PROGBITS && (lnk_read_le64(sh + 8) & SHF_WRITE)) {
                data_off = sh_offset; data_sz = sh_size;
            } else if (sh_type == SHT_SYMTAB) {
                symtab_off = sh_offset; symtab_sz = sh_size;
                symtab_link = lnk_read_le32(sh + 40);
            } else if (sh_type == SHT_STRTAB && si != shstrndx) {
                strtab_off = sh_offset; strtab_sz = sh_size;
            } else if (sh_type == SHT_RELA) {
                rela_off = sh_offset; rela_sz = sh_size;
            }
            (void)name; (void)strtab_sz;
        }

        /* Concatenate .text */
        objs[oi].text_offset = text_cursor;
        objs[oi].text_size = (uint32_t)text_sz;
        if (text_sz > 0) {
            if (text_cursor + text_sz > LINK_MAX_TEXT) {
                kprintf("ld: combined .text too large\n");
                kfree(file_buf); goto fail;
            }
            memcpy(merged_text + text_cursor, file_buf + text_off, text_sz);
            text_cursor += (uint32_t)text_sz;
        }

        /* Concatenate .data */
        objs[oi].data_offset = data_cursor;
        objs[oi].data_size = (uint32_t)data_sz;
        if (data_sz > 0) {
            if (data_cursor + data_sz > LINK_MAX_DATA) {
                kprintf("ld: combined .data too large\n");
                kfree(file_buf); goto fail;
            }
            memcpy(merged_data + data_cursor, file_buf + data_off, data_sz);
            data_cursor += (uint32_t)data_sz;
        }

        /* Process symbol table */
        const uint8_t *strtab_p = (strtab_off > 0) ? file_buf + strtab_off : 0;
        int sym_count = (int)(symtab_sz / SYM_SIZE);
        (void)symtab_link;

        for (int si = 1; si < sym_count; si++) { /* skip [0] null */
            const uint8_t *sym = file_buf + symtab_off + si * SYM_SIZE;
            uint32_t st_name = lnk_read_le32(sym + 0);
            uint8_t  st_info = sym[4];
            uint16_t st_shndx = lnk_read_le16(sym + 6);
            uint64_t st_value = lnk_read_le64(sym + 8);

            uint8_t binding = st_info >> 4;
            uint8_t stype = st_info & 0xf;

            if (stype == 3) {
                /* Section symbol — register in symbol table */
                int sym_section = 0;
                uint64_t sym_value = 0;
                if (st_shndx == 1) { /* .text */
                    sym_section = 1;
                    sym_value = (uint64_t)(objs[oi].text_offset);
                } else if (st_shndx == 2) { /* .data */
                    sym_section = 2;
                    sym_value = (uint64_t)(objs[oi].data_offset);
                }
                const char *sname = strtab_p ? (const char *)(strtab_p + st_name) : "";
                if (sname[0]) {
                    link_add_sym(syms, &nsyms, sname, 0, sym_section, sym_value, oi);
                }
                continue;
            }
            if (binding != STB_GLOBAL) continue; /* skip locals */

            const char *sname = strtab_p ? (const char *)(strtab_p + st_name) : "";
            if (!sname[0]) continue;

            int defined = (st_shndx != 0);
            int section = 0;
            uint64_t value = 0;

            if (defined) {
                /* Determine section and compute address */
                /* st_shndx: 1=.text, 2=.data in our format */
                if (st_shndx == 1) { /* .text */
                    section = 1;
                    value = (uint64_t)(objs[oi].text_offset + (uint32_t)st_value);
                } else if (st_shndx == 2) { /* .data */
                    section = 2;
                    value = (uint64_t)(objs[oi].data_offset + (uint32_t)st_value);
                }
            }

            link_add_sym(syms, &nsyms, sname, defined, section, value, oi);
        }

        /* Store relocation info for Pass 2 — we'll re-read the file */
        /* For efficiency, process relocations now while file is loaded */
        if (rela_sz > 0 && symtab_sz > 0) {
            int nrelas = (int)(rela_sz / RELA_SIZE);
            for (int ri = 0; ri < nrelas; ri++) {
                const uint8_t *rela = file_buf + rela_off + ri * RELA_SIZE;
                uint64_t r_offset = lnk_read_le64(rela + 0);
                uint64_t r_info = lnk_read_le64(rela + 8);
                int64_t r_addend = (int64_t)lnk_read_le64(rela + 16);

                uint32_t r_sym = (uint32_t)(r_info >> 32);
                uint32_t r_type = (uint32_t)(r_info & 0xffffffff);

                /* Resolve symbol for this relocation */
                const char *rsym_name = "";
                int rsym_shndx = 0;
                if (r_sym > 0 && r_sym < (uint32_t)sym_count) {
                    const uint8_t *rsym = file_buf + symtab_off + r_sym * SYM_SIZE;
                    uint32_t rsn = lnk_read_le32(rsym + 0);
                    uint8_t rsinfo = rsym[4];
                    rsym_shndx = lnk_read_le16(rsym + 6);
                    rsym_name = strtab_p ? (const char *)(strtab_p + rsn) : "";
                    (void)rsinfo;
                }

                /* Compute patch location in merged text */
                uint32_t patch_off = objs[oi].text_offset + (uint32_t)r_offset;

                /* We'll apply relocations in place after all symbols are collected */
                /* Store a temporary relocation entry: use the merged_text buffer itself
                   and process after the loop. For now, use a simple in-memory list.
                   We'll encode relocs as: {patch_off, r_type, symbol_name_or_value, addend} */
                /* Instead of a separate structure, store relocs in merged_text with markers.
                   Simpler: just store the info we need. */

                /* Actually, let's just apply section-relative relocs now (section syms)
                   and defer external symbol relocs to pass 2 */
                uint8_t rstype = (r_sym > 0 && r_sym < (uint32_t)sym_count) ?
                    file_buf[symtab_off + r_sym * SYM_SIZE + 4] & 0xf : 0;

                if (rstype == 3) { /* Section symbol */
                    uint64_t S = 0;
                    if (rsym_shndx == 1) /* .text section */
                        S = load_base + (uint64_t)objs[oi].text_offset;
                    else if (rsym_shndx == 2) /* .data section */
                        S = load_base + PAGE_SZ + (uint64_t)objs[oi].data_offset;

                    if (r_type == R_X86_64_PC32) {
                        uint64_t P = load_base + (uint64_t)patch_off;
                        int32_t val = (int32_t)(S + r_addend - P);
                        lnk_write_le32(merged_text + patch_off, (uint32_t)val);
                    } else if (r_type == R_X86_64_64) {
                        uint64_t val = S + (uint64_t)r_addend;
                        lnk_write_le64(merged_text + patch_off, val);
                    }
                } else if (rsym_name[0]) {
                    /* Named symbol — defer to pass 2. Encode a marker for later. */
                    /* We need to record these relocations for pass 2. */
                    /* Use a simple approach: keep this file loaded? No — too expensive.
                       Instead, record in a temporary buffer. */
                    /* For simplicity, add to the global reloc list. Since we process
                       files sequentially, this is fine. We'll use a static buffer. */
                }
            }
        }

        kfree(file_buf);
    }

    /* Pass 2: Resolve all symbols and apply named relocations.
       We need to re-read files for their relocation tables.
       Alternatively, we collect all relocs in pass 1. Let's do a second
       pass over the files for relocation application. */

    /* First, compute final virtual addresses for all defined symbols */
    uint64_t text_base = load_base;
    uint64_t data_base = load_base + PAGE_SZ; /* data at next page */
    /* Adjust if text is larger than one page */
    uint32_t text_pages = (text_cursor + PAGE_SZ - 1) / PAGE_SZ;
    data_base = load_base + (uint64_t)(text_pages * PAGE_SZ);

    for (int i = 0; i < nsyms; i++) {
        if (!syms[i].defined) continue;
        if (syms[i].section == 1) /* text */
            syms[i].value = text_base + syms[i].value;
        else if (syms[i].section == 2) /* data */
            syms[i].value = data_base + syms[i].value;
    }

    /* Check for undefined symbols */
    int undef_count = 0;
    for (int i = 0; i < nsyms; i++) {
        if (!syms[i].defined) {
            kprintf("ld: undefined symbol: %s\n", syms[i].name);
            undef_count++;
        }
    }
    if (undef_count > 0) {
        kprintf("ld: %d undefined symbol(s)\n", undef_count);
        goto fail;
    }

    /* Pass 2: Re-read each object and apply named relocations */
    for (int oi = 0; oi < nobj; oi++) {
        uint32_t file_sz = 0;
        struct vfs_stat st2;
        memset(&st2, 0, sizeof(st2));
        if (vfs_stat(obj_paths[oi], &st2) < 0) continue;
        file_sz = st2.size;
        uint8_t *file_buf = (uint8_t *)kmalloc(file_sz);
        if (!file_buf) continue;
        uint32_t read_sz2 = 0;
        if (vfs_read(obj_paths[oi], file_buf, file_sz, &read_sz2) < 0) {
            kfree(file_buf); continue;
        }

        /* Find section headers again */
        uint64_t shoff = lnk_read_le64(file_buf + 40);
        uint16_t shnum = lnk_read_le16(file_buf + 60);
        uint16_t shstrndx = lnk_read_le16(file_buf + 62);
        (void)shstrndx;

        uint64_t symtab_off2 = 0, symtab_sz2a = 0;
        uint64_t strtab_off2 = 0;
        uint64_t rela_off2 = 0, rela_sz2 = 0;

        for (int si = 0; si < shnum; si++) {
            const uint8_t *sh = file_buf + shoff + si * SHDR_SIZE;
            uint32_t sh_type = lnk_read_le32(sh + 4);
            if (sh_type == SHT_SYMTAB) {
                symtab_off2 = lnk_read_le64(sh + 24);
                symtab_sz2a = lnk_read_le64(sh + 32);
            } else if (sh_type == SHT_STRTAB && si != shstrndx) {
                strtab_off2 = lnk_read_le64(sh + 24);
            } else if (sh_type == SHT_RELA) {
                rela_off2 = lnk_read_le64(sh + 24);
                rela_sz2 = lnk_read_le64(sh + 32);
            }
        }

        if (rela_sz2 == 0 || symtab_sz2a == 0) { kfree(file_buf); continue; }

        const uint8_t *strtab_p = file_buf + strtab_off2;
        int nrelas = (int)(rela_sz2 / RELA_SIZE);
        int sym_count2 = (int)(symtab_sz2a / SYM_SIZE);

        for (int ri = 0; ri < nrelas; ri++) {
            const uint8_t *rela = file_buf + rela_off2 + ri * RELA_SIZE;
            uint64_t r_offset = lnk_read_le64(rela + 0);
            uint64_t r_info = lnk_read_le64(rela + 8);
            int64_t r_addend = (int64_t)lnk_read_le64(rela + 16);

            uint32_t r_sym = (uint32_t)(r_info >> 32);
            uint32_t r_type = (uint32_t)(r_info & 0xffffffff);

            if (r_sym == 0 || r_sym >= (uint32_t)sym_count2) continue;

            const uint8_t *rsym = file_buf + symtab_off2 + r_sym * SYM_SIZE;
            uint8_t rstype = rsym[4] & 0xf;
            if (rstype == 3) continue; /* section symbols already handled in pass 1 */

            uint32_t rsn = lnk_read_le32(rsym + 0);
            const char *rsym_name = (const char *)(strtab_p + rsn);

            /* Find symbol in linker table */
            int sidx = link_find_sym(syms, &nsyms, rsym_name);
            if (sidx < 0) continue; /* shouldn't happen — checked above */

            uint64_t S = syms[sidx].value;
            uint32_t patch_off = objs[oi].text_offset + (uint32_t)r_offset;

            if (r_type == R_X86_64_PC32) {
                uint64_t P = text_base + (uint64_t)patch_off;
                int32_t val = (int32_t)((int64_t)S + r_addend - (int64_t)P);
                lnk_write_le32(merged_text + patch_off, (uint32_t)val);
            } else if (r_type == R_X86_64_64) {
                uint64_t val = S + (uint64_t)r_addend;
                lnk_write_le64(merged_text + patch_off, val);
            }
        }

        kfree(file_buf);
    }

    /* Write output executable */
    {
        uint32_t code_file_off = ELF_HDR_SIZE + 2 * PHDR_SIZE;
        uint32_t data_file_off = (code_file_off + text_cursor + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
        uint32_t total_size = data_file_off + data_cursor;
        if (data_cursor == 0) total_size = data_file_off + 8;

        uint8_t *out = (uint8_t *)kmalloc(total_size);
        if (!out) { kprintf("ld: out of memory for output\n"); goto fail; }
        memset(out, 0, total_size);

        /* ELF header */
        out[0]=0x7f; out[1]='E'; out[2]='L'; out[3]='F';
        out[4] = ELF_CLASS64; out[5] = ELF_DATA2LSB;
        out[6] = 1; out[7] = 0;
        lnk_write_le16(out + 16, ET_EXEC);
        lnk_write_le16(out + 18, EM_X86_64);
        lnk_write_le32(out + 20, 1);

        /* Entry point = "main" if it exists, else text_base */
        uint64_t entry = text_base;
        int main_idx = link_find_sym(syms, &nsyms, "main");
        if (main_idx >= 0 && syms[main_idx].defined)
            entry = syms[main_idx].value;
        /* But for _start wrapper, entry should be text_base (contains call main; exit) */
        /* Actually, our cc emits _start at beginning of text — use text_base */
        int start_idx = link_find_sym(syms, &nsyms, "_start");
        if (start_idx >= 0 && syms[start_idx].defined)
            entry = syms[start_idx].value;

        lnk_write_le64(out + 24, entry);
        lnk_write_le64(out + 32, ELF_HDR_SIZE); /* e_phoff */
        lnk_write_le64(out + 40, 0);            /* e_shoff */
        lnk_write_le32(out + 48, 0);
        lnk_write_le16(out + 52, ELF_HDR_SIZE);
        lnk_write_le16(out + 54, PHDR_SIZE);
        lnk_write_le16(out + 56, 2);            /* 2 program headers */
        lnk_write_le16(out + 58, 64);
        lnk_write_le16(out + 60, 0);
        lnk_write_le16(out + 62, 0);

        /* Program header 0: .text (RX) */
        uint8_t *ph0 = out + ELF_HDR_SIZE;
        lnk_write_le32(ph0 + 0, 1);             /* PT_LOAD */
        lnk_write_le32(ph0 + 4, 5);             /* PF_R | PF_X */
        lnk_write_le64(ph0 + 8, code_file_off);
        lnk_write_le64(ph0 + 16, text_base);
        lnk_write_le64(ph0 + 24, text_base);
        lnk_write_le64(ph0 + 32, text_cursor);
        lnk_write_le64(ph0 + 40, text_cursor);
        lnk_write_le64(ph0 + 48, PAGE_SZ);

        /* Program header 1: .data (RW) */
        uint8_t *ph1 = out + ELF_HDR_SIZE + PHDR_SIZE;
        uint32_t dsz = data_cursor > 0 ? data_cursor : 8;
        lnk_write_le32(ph1 + 0, 1);
        lnk_write_le32(ph1 + 4, 6);             /* PF_R | PF_W */
        lnk_write_le64(ph1 + 8, data_file_off);
        lnk_write_le64(ph1 + 16, data_base);
        lnk_write_le64(ph1 + 24, data_base);
        lnk_write_le64(ph1 + 32, dsz);
        lnk_write_le64(ph1 + 40, dsz);
        lnk_write_le64(ph1 + 48, PAGE_SZ);

        /* Copy merged sections */
        memcpy(out + code_file_off, merged_text, text_cursor);
        if (data_cursor > 0)
            memcpy(out + data_file_off, merged_data, data_cursor);

        int ret = vfs_write(outpath, out, total_size);
        kfree(out);

        if (ret < 0) {
            kprintf("ld: failed to write %s\n", outpath);
            goto fail;
        }
        kprintf("ld: wrote %u bytes to %s (text=%u data=%u, %d syms)\n",
                total_size, outpath, text_cursor, data_cursor, nsyms);
    }

    kfree(merged_text);
    kfree(merged_data);
    kfree(syms);
    kfree(objs);
    return 0;

fail:
    if (merged_text) kfree(merged_text);
    if (merged_data) kfree(merged_data);
    if (syms) kfree(syms);
    if (objs) kfree(objs);
    return -1;

fail_alloc:
    if (merged_text) kfree(merged_text);
    if (merged_data) kfree(merged_data);
    if (syms) kfree(syms);
    if (objs) kfree(objs);
    return -1;
}

/* ── Stub: cc_link_objects ─────────────────────────────── */
int cc_link_objects(void *objs, int count, const char *output)
{
    (void)objs;
    (void)count;
    (void)output;
    kprintf("[cc] cc_link_objects: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cc_link_resolve_syms ─────────────────────────────── */
int cc_link_resolve_syms(void *objs, int count)
{
    (void)objs;
    (void)count;
    kprintf("[cc] cc_link_resolve_syms: not yet implemented\n");
    return -ENOSYS;
}
