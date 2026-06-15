/* ld.c — ELF64 static linker for Hermes OS userspace
 *
 * A simple but functional linker that reads relocatable ELF64 .o files,
 * concatenates sections, resolves relocations, and writes a static
 * ELF64 executable.
 *
 * Usage: ld [-o output] input1.o [input2.o ...]
 *
 * Supports:
 *   .text, .data, .rodata, .bss section merging
 *   R_X86_64_64  (absolute 64-bit)
 *   R_X86_64_PC32 (32-bit PC-relative)
 *   R_X86_64_PLT32 (treated as PC32)
 *   R_X86_64_32 / R_X86_64_32S (32-bit absolute)
 *   R_X86_64_PC64 (64-bit PC-relative)
 *   Global symbol resolution across input files
 *   ELF64 ET_EXEC output with single PT_LOAD segment
 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* ── ELF64 constants and structures ─────────────────────────────── */

#define ELF_MAGIC      0x464C457FUL  /* \x7fELF */
#define ET_REL         1
#define ET_EXEC        2
#define EM_X86_64      62
#define EV_CURRENT     1

/* ELF class / data / version in e_ident */
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6
#define ELFCLASS64     2
#define ELFDATA2LSB    1

/* Section types */
#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_RELA       4
#define SHT_NOBITS     8

/* Section flags */
#define SHF_WRITE      0x1
#define SHF_ALLOC      0x2
#define SHF_EXECINSTR  0x4

/* Program header types */
#define PT_LOAD        1

/* Symbol bindings */
#define STB_LOCAL      0
#define STB_GLOBAL     1
#define STB_WEAK       2

/* Symbol types */
#define STT_NOTYPE     0
#define STT_OBJECT     1
#define STT_FUNC       2

#define SHN_UNDEF      0
#define SHN_ABS        0xFFF1

/* Relocation types */
#define R_X86_64_NONE   0
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_GOT32  3
#define R_X86_64_PLT32  4
#define R_X86_64_32     10
#define R_X86_64_32S    11
#define R_X86_64_PC64   24

/* ELF64 header */
struct elf64_ehdr {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int   e_version;
    unsigned long  e_entry;
    unsigned long  e_phoff;
    unsigned long  e_shoff;
    unsigned int   e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
} __attribute__((packed));

/* ELF64 section header */
struct elf64_shdr {
    unsigned int   sh_name;
    unsigned int   sh_type;
    unsigned long  sh_flags;
    unsigned long  sh_addr;
    unsigned long  sh_offset;
    unsigned long  sh_size;
    unsigned int   sh_link;
    unsigned int   sh_info;
    unsigned long  sh_addralign;
    unsigned long  sh_entsize;
} __attribute__((packed));

/* ELF64 symbol table entry */
struct elf64_sym {
    unsigned int   st_name;
    unsigned char  st_info;
    unsigned char  st_other;
    unsigned short st_shndx;
    unsigned long  st_value;
    unsigned long  st_size;
} __attribute__((packed));

/* ELF64 RELA relocation entry */
struct elf64_rela {
    unsigned long  r_offset;
    unsigned long  r_info;
    long           r_addend;
} __attribute__((packed));

/* ELF64 program header */
struct elf64_phdr {
    unsigned int   p_type;
    unsigned int   p_flags;
    unsigned long  p_offset;
    unsigned long  p_vaddr;
    unsigned long  p_paddr;
    unsigned long  p_filesz;
    unsigned long  p_memsz;
    unsigned long  p_align;
} __attribute__((packed));

/* Helper macros */
#define ELF64_ST_BIND(i)     ((i) >> 4)
#define ELF64_ST_TYPE(i)     ((i) & 0xf)
#define ELF64_ST_INFO(b,t)   (((b) << 4) | ((t) & 0xf))
#define ELF64_R_SYM(i)       ((i) >> 32)
#define ELF64_R_TYPE(i)      ((unsigned int)(i))
#define ELF64_R_INFO(s,t)    ((((unsigned long)(s)) << 32) | (unsigned int)(t))

/* ── Internal data structures ──────────────────────────────────── */

#define MAX_OBJS      256
#define MAX_SECTIONS  32
#define MAX_SYMBOLS   4096
#define MAX_TEXT_SIZE (2 * 1024 * 1024)    /* 2 MB max .text */
#define MAX_DATA_SIZE (512 * 1024)         /* 512 KB max .data/.rodata */

/* Per-input-object tracking */
struct input_obj {
    char         *data;        /* raw file buffer */
    unsigned long size;
    struct elf64_ehdr *ehdr;

    /* Section indices for known sections in this object */
    int idx_text;
    int idx_data;
    int idx_rodata;
    int idx_bss;
    int idx_symtab;
    int idx_strtab;
    int idx_shstrtab;
};

/* Merged section tracking */
struct merged_section {
    unsigned char *data;       /* concatenated data (NULL for bss) */
    unsigned long  size;       /* total merged size */
    unsigned long  align;      /* required alignment */
    unsigned int   sh_flags;   /* section flags */
    unsigned int   sh_type;    /* section type */
    unsigned long  vaddr;      /* assigned virtual address */
    unsigned long  offset;     /* file offset in output */
};

/* Global symbol table entry */
struct link_sym {
    char           name[64];
    unsigned char  bind;       /* STB_GLOBAL, STB_LOCAL, STB_WEAK */
    unsigned char  type;       /* STT_NOTYPE, STT_FUNC, STT_OBJECT */
    int            defined;
    unsigned long  value;      /* final address */
};

/* ── Globals ───────────────────────────────────────────────────── */

static struct input_obj  g_objs[MAX_OBJS];
static int               g_nobj;

static struct merged_section g_sections[4]; /* 0=text, 1=data, 2=rodata, 3=bss */
static int                  g_nsections;

static struct link_sym      g_symtab[MAX_SYMBOLS];
static int                  g_nsyms;

static unsigned long        g_base_vaddr = 0x400000;   /* default base address */

/* ── Low-level read helpers ────────────────────────────────────── */

static unsigned int rd32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static void wr32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff); p[3] = (unsigned char)((v >> 24) & 0xff);
}
static void wr64(unsigned char *p, unsigned long v) {
    wr32(p, (unsigned int)(v & 0xffffffffUL));
    wr32(p+4, (unsigned int)(v >> 32));
}

/* ── Error reporting ───────────────────────────────────────────── */

static int g_exit_code = 0;

/* Format an error message (accepts %s format string, single string arg) */
static void err_msg(const char *fmt, const char *arg) {
    printf("ld: ");
    printf(fmt, arg);
    printf("\n");
    g_exit_code = 1;
}

/* ── Read entire file into buffer ──────────────────────────────── */

static char *read_whole_file(const char *path, unsigned long *out_size) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        err_msg("cannot open '%s'", path);
        return 0;
    }
    /* Get size via lseek */
    long sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        err_msg("empty or unreadable: '%s'", path);
        close(fd);
        return 0;
    }
    lseek(fd, 0, SEEK_SET);

    char *buf = malloc((unsigned long)sz);
    if (!buf) {
        err_msg("out of memory reading '%s'", path);
        close(fd);
        return 0;
    }
    long total = 0;
    while (total < sz) {
        int n = read(fd, buf + total, (unsigned long)(sz - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    *out_size = (unsigned long)sz;
    return buf;
}

/* ── Parse an input object file ────────────────────────────────── */

static int parse_obj(struct input_obj *obj, const char *path) {
    obj->data = read_whole_file(path, &obj->size);
    if (!obj->data) return -1;

    obj->ehdr = (struct elf64_ehdr *)obj->data;

    /* Validate ELF header */
    if (rd32((unsigned char *)obj->data) != ELF_MAGIC ||
        obj->ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        obj->ehdr->e_type != ET_REL) {
        printf("ld: '%s' is not a valid ELF64 relocatable object\n", path);
        g_exit_code = 1;
        return -1;
    }

    /* Locate section header string table */
    unsigned long shoff = obj->ehdr->e_shoff;
    unsigned short shnum = obj->ehdr->e_shnum;
    unsigned short shstrndx = obj->ehdr->e_shstrndx;

    if (shoff == 0 || shnum == 0) {
        err_msg("'%s' has no section headers", path);
        return -1;
    }

    struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + shoff);

    /* Get section name string table */
    const char *shstrtab = "";
    unsigned long shstrtab_size = 0;
    if (shstrndx < shnum) {
        struct elf64_shdr *shstrhdr = &shdrs[shstrndx];
        if (shstrhdr->sh_offset + shstrhdr->sh_size <= obj->size) {
            shstrtab = obj->data + shstrhdr->sh_offset;
            shstrtab_size = shstrhdr->sh_size;
        }
    }

    obj->idx_text = -1;
    obj->idx_data = -1;
    obj->idx_rodata = -1;
    obj->idx_bss = -1;
    obj->idx_symtab = -1;
    obj->idx_strtab = -1;
    obj->idx_shstrtab = -1;

    /* Identify sections by name */
    for (int i = 0; i < shnum; i++) {
        unsigned int name_off = shdrs[i].sh_name;
        const char *name = "";
        if (name_off < shstrtab_size)
            name = shstrtab + name_off;

        if (strcmp(name, ".text") == 0)
            obj->idx_text = i;
        else if (strcmp(name, ".data") == 0)
            obj->idx_data = i;
        else if (strcmp(name, ".rodata") == 0)
            obj->idx_rodata = i;
        else if (strcmp(name, ".bss") == 0)
            obj->idx_bss = i;
        else if (shdrs[i].sh_type == SHT_SYMTAB)
            obj->idx_symtab = i;
        else if (shdrs[i].sh_type == SHT_STRTAB && i != shstrndx)
            obj->idx_strtab = i;
    }

    obj->idx_shstrtab = shstrndx;
    return 0;
}

/* ── Get string table for an object ────────────────────────────── */

static const char *obj_strtab(const struct input_obj *obj) {
    if (obj->idx_strtab < 0) return "";
    struct elf64_ehdr *ehdr = obj->ehdr;
    struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + ehdr->e_shoff);
    struct elf64_shdr *strhdr = &shdrs[obj->idx_strtab];
    if (strhdr->sh_offset + strhdr->sh_size > obj->size) return "";
    return obj->data + strhdr->sh_offset;
}

/* ── Symbol table operations ───────────────────────────────────── */

/* Find or add a global symbol */
static struct link_sym *find_or_add_sym(const char *name, unsigned char bind) {
    for (int i = 0; i < g_nsyms; i++) {
        if (strcmp(g_symtab[i].name, name) == 0)
            return &g_symtab[i];
    }
    if (g_nsyms >= MAX_SYMBOLS) return 0;
    struct link_sym *s = &g_symtab[g_nsyms++];
    /* strncpy without using it */
    unsigned long len = strlen(name);
    if (len >= sizeof(s->name)) len = sizeof(s->name) - 1;
    memcpy(s->name, name, len);
    s->name[len] = '\0';
    s->bind = bind;
    s->type = STT_NOTYPE;
    s->defined = 0;
    s->value = 0;
    return s;
}

/* ── Collect global symbols from all input objects ──────────────── */

static void collect_symbols(void) {
    for (int oi = 0; oi < g_nobj; oi++) {
        struct input_obj *obj = &g_objs[oi];
        if (obj->idx_symtab < 0) continue;

        struct elf64_ehdr *ehdr = obj->ehdr;
        struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + ehdr->e_shoff);
        struct elf64_shdr *symhdr = &shdrs[obj->idx_symtab];
        const char *strtab = obj_strtab(obj);

        unsigned long nsym = symhdr->sh_size / symhdr->sh_entsize;
        struct elf64_sym *syms = (struct elf64_sym *)(obj->data + symhdr->sh_offset);

        for (unsigned long si = 0; si < nsym; si++) {
            unsigned char bind = ELF64_ST_BIND(syms[si].st_info);
            unsigned char type = ELF64_ST_TYPE(syms[si].st_info);

            /* Skip locals, sections, and undefined weak */
            if (bind == STB_LOCAL) continue;
            if (syms[si].st_shndx == SHN_UNDEF && bind == STB_WEAK) continue;

            const char *sym_name = "";
            if (syms[si].st_name > 0)
                sym_name = strtab + syms[si].st_name;

            if (sym_name[0] == '\0') continue;

            struct link_sym *ls = find_or_add_sym(sym_name, bind);
            if (!ls) {
                printf("ld: too many symbols (max 4096)\n");
                g_exit_code = 1;
                return;
            }

            ls->type = type;

            if (syms[si].st_shndx != SHN_UNDEF && syms[si].st_shndx < ehdr->e_shnum) {
                /* Defined symbol — only set if not already defined or if global overrides weak */
                if (!ls->defined || (ls->bind == STB_WEAK && bind == STB_GLOBAL)) {
                    /* Compute the final value later during layout; store section info for now */
                    /* We'll compute addresses after layout */
                }
            }
        }
    }
}

/* ── Section layout: assign addresses and file offsets ──────────── */

static void layout_sections(void) {
    unsigned long cur_offset = 0;
    unsigned long cur_vaddr = g_base_vaddr;

    /* ELF header + program header size */
    unsigned long header_size = sizeof(struct elf64_ehdr) + sizeof(struct elf64_phdr);
    cur_offset = header_size;
    cur_vaddr = g_base_vaddr;

    /* Initialise merged sections */
    g_nsections = 4;
    /* .text: code, read+execute */
    g_sections[0].data = 0;
    g_sections[0].size = 0;
    g_sections[0].align = 16;
    g_sections[0].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    g_sections[0].sh_type = SHT_PROGBITS;
    /* .data: read+write */
    g_sections[1].data = 0;
    g_sections[1].size = 0;
    g_sections[1].align = 16;
    g_sections[1].sh_flags = SHF_ALLOC | SHF_WRITE;
    g_sections[1].sh_type = SHT_PROGBITS;
    /* .rodata: read only */
    g_sections[2].data = 0;
    g_sections[2].size = 0;
    g_sections[2].align = 16;
    g_sections[2].sh_flags = SHF_ALLOC;
    g_sections[2].sh_type = SHT_PROGBITS;
    /* .bss: zero-initialised */
    g_sections[3].data = 0;
    g_sections[3].size = 0;
    g_sections[3].align = 16;
    g_sections[3].sh_flags = SHF_ALLOC | SHF_WRITE;
    g_sections[3].sh_type = SHT_NOBITS;

    /* First pass: calculate merged sizes */
    unsigned long text_size = 0;
    unsigned long data_size = 0;
    unsigned long rodata_size = 0;
    unsigned long bss_size = 0;
    unsigned long text_align = 1;
    unsigned long data_align = 1;
    unsigned long rodata_align = 1;
    unsigned long bss_align = 1;

    for (int oi = 0; oi < g_nobj; oi++) {
        struct input_obj *obj = &g_objs[oi];
        struct elf64_ehdr *ehdr = obj->ehdr;
        struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + ehdr->e_shoff);

        int idxes[] = {
            obj->idx_text, obj->idx_data, obj->idx_rodata, obj->idx_bss
        };
        unsigned long *sizes[] = { &text_size, &data_size, &rodata_size, &bss_size };
        unsigned long *aligns[] = { &text_align, &data_align, &rodata_align, &bss_align };

        for (int si = 0; si < 4; si++) {
            if (idxes[si] >= 0 && idxes[si] < ehdr->e_shnum) {
                struct elf64_shdr *sh = &shdrs[idxes[si]];
                if (sh->sh_addralign > *aligns[si])
                    *aligns[si] = sh->sh_addralign;
                *sizes[si] += sh->sh_size;
            }
        }
    }

    /* Allocate section data buffers */
    for (int si = 0; si < 4; si++) {
        unsigned long sz = 0;
        switch (si) {
            case 0: sz = text_size; break;
            case 1: sz = data_size; break;
            case 2: sz = rodata_size; break;
            case 3: sz = bss_size; break;
        }
        g_sections[si].size = sz;
        if (sz > 0 && g_sections[si].sh_type != SHT_NOBITS) {
            g_sections[si].data = malloc(sz ? sz : 1);
            if (!g_sections[si].data) {
                err_msg("out of memory for merged section", "");
                return;
            }
        }
        /* Update alignment */
        switch (si) {
            case 0: g_sections[si].align = text_align; break;
            case 1: g_sections[si].align = data_align; break;
            case 2: g_sections[si].align = rodata_align; break;
            case 3: g_sections[si].align = bss_align; break;
        }
    }

    /* Second pass: copy section data */
    unsigned long text_pos = 0;
    unsigned long data_pos = 0;
    unsigned long rodata_pos = 0;

    for (int oi = 0; oi < g_nobj; oi++) {
        struct input_obj *obj = &g_objs[oi];
        struct elf64_ehdr *ehdr = obj->ehdr;
        struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + ehdr->e_shoff);

        /* Copy .text */
        if (obj->idx_text >= 0) {
            struct elf64_shdr *sh = &shdrs[obj->idx_text];
            unsigned long sz = sh->sh_size;
            if (sz > 0 && sh->sh_offset + sz <= obj->size) {
                memcpy(g_sections[0].data + text_pos, obj->data + sh->sh_offset, sz);
            }
            text_pos += sz;
        }
        /* Copy .data */
        if (obj->idx_data >= 0) {
            struct elf64_shdr *sh = &shdrs[obj->idx_data];
            unsigned long sz = sh->sh_size;
            if (sz > 0 && sh->sh_offset + sz <= obj->size) {
                memcpy(g_sections[1].data + data_pos, obj->data + sh->sh_offset, sz);
            }
            data_pos += sz;
        }
        /* Copy .rodata */
        if (obj->idx_rodata >= 0) {
            struct elf64_shdr *sh = &shdrs[obj->idx_rodata];
            unsigned long sz = sh->sh_size;
            if (sz > 0 && sh->sh_offset + sz <= obj->size) {
                memcpy(g_sections[2].data + rodata_pos, obj->data + sh->sh_offset, sz);
            }
            rodata_pos += sz;
        }
    }

    /* Assign vaddrs and file offsets */

    /* .text first (needs page alignment) */
    unsigned long page_size = 0x1000;
    /* Align current offset up to page boundary */
    unsigned long off = cur_offset;
    off = (off + page_size - 1) & ~(page_size - 1);
    g_sections[0].offset = off;
    g_sections[0].vaddr = cur_vaddr;
    cur_vaddr += g_sections[0].size;
    off += g_sections[0].size;

    /* .rodata */
    off = (off + 0x10 - 1) & ~(0xfUL);
    g_sections[2].offset = off;
    g_sections[2].vaddr = cur_vaddr;
    g_sections[2].vaddr = (g_sections[2].vaddr + 0x10 - 1) & ~(0xfUL);
    cur_vaddr = g_sections[2].vaddr + g_sections[2].size;
    off += g_sections[2].size;

    /* .data */
    off = (off + 0x10 - 1) & ~(0xfUL);
    g_sections[1].offset = off;
    g_sections[1].vaddr = cur_vaddr;
    g_sections[1].vaddr = (g_sections[1].vaddr + 0x10 - 1) & ~(0xfUL);
    cur_vaddr = g_sections[1].vaddr + g_sections[1].size;
    off += g_sections[1].size;

    /* .bss (vaddr only, no file space) */
    g_sections[3].vaddr = cur_vaddr;
    g_sections[3].vaddr = (g_sections[3].vaddr + 0x10 - 1) & ~(0xfUL);
    g_sections[3].offset = 0; /* not in file */
    cur_vaddr = g_sections[3].vaddr + g_sections[3].size;
}

/* ── Resolve symbol values after layout ─────────────────────────── */

static void resolve_symbols(void) {
    for (int oi = 0; oi < g_nobj; oi++) {
        struct input_obj *obj = &g_objs[oi];
        if (obj->idx_symtab < 0) continue;

        struct elf64_ehdr *ehdr = obj->ehdr;
        struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + ehdr->e_shoff);
        struct elf64_shdr *symhdr = &shdrs[obj->idx_symtab];
        const char *strtab = obj_strtab(obj);

        unsigned long nsym = symhdr->sh_size / symhdr->sh_entsize;
        struct elf64_sym *syms = (struct elf64_sym *)(obj->data + symhdr->sh_offset);

        /* Pre-compute per-object section offsets */
        unsigned long obj_text_off = 0;
        unsigned long obj_data_off = 0;
        unsigned long obj_rodata_off = 0;

        /* Find section contribution order matches our concatenation order */
        for (int po = 0; po < oi; po++) {
            struct input_obj *pobj = &g_objs[po];
            struct elf64_ehdr *pe = pobj->ehdr;
            struct elf64_shdr *ps = (struct elf64_shdr *)(pobj->data + pe->e_shoff);
            if (pobj->idx_text >= 0) obj_text_off += ps[pobj->idx_text].sh_size;
            if (pobj->idx_data >= 0) obj_data_off += ps[pobj->idx_data].sh_size;
            if (pobj->idx_rodata >= 0) obj_rodata_off += ps[pobj->idx_rodata].sh_size;
        }

        for (unsigned long si = 0; si < nsym; si++) {
            unsigned char bind = ELF64_ST_BIND(syms[si].st_info);
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            if (syms[si].st_shndx == SHN_UNDEF) continue;
            if (syms[si].st_shndx == SHN_ABS) continue;

            const char *sym_name = "";
            if (syms[si].st_name > 0)
                sym_name = strtab + syms[si].st_name;
            if (sym_name[0] == '\0') continue;

            unsigned short shndx = syms[si].st_shndx;
            unsigned long base_vaddr = 0;
            unsigned long obj_offset = 0;

            if (shndx == obj->idx_text) {
                base_vaddr = g_sections[0].vaddr;
                obj_offset = obj_text_off;
            } else if (shndx == obj->idx_data) {
                base_vaddr = g_sections[1].vaddr;
                obj_offset = obj_data_off;
            } else if (shndx == obj->idx_rodata) {
                base_vaddr = g_sections[2].vaddr;
                obj_offset = obj_rodata_off;
            } else if (shndx == obj->idx_bss) {
                base_vaddr = g_sections[3].vaddr;
                /* Compute bss offset */
                unsigned long bss_off = 0;
                for (int po = 0; po < oi; po++) {
                    struct input_obj *pobj = &g_objs[po];
                    struct elf64_ehdr *pe = pobj->ehdr;
                    struct elf64_shdr *ps = (struct elf64_shdr *)(pobj->data + pe->e_shoff);
                    if (pobj->idx_bss >= 0) bss_off += ps[pobj->idx_bss].sh_size;
                }
                obj_offset = bss_off;
            } else {
                continue; /* Section not merged (debug, comment, etc.) */
            }

            struct link_sym *ls = find_or_add_sym(sym_name, bind);
            if (!ls) continue;

            unsigned long final_val = base_vaddr + obj_offset + syms[si].st_value;
            if (!ls->defined || (ls->bind == STB_WEAK && bind == STB_GLOBAL)) {
                ls->defined = 1;
                ls->value = final_val;
                ls->bind = bind;
                ls->type = ELF64_ST_TYPE(syms[si].st_info);
            }
        }
    }
}

/* ── Apply relocations ──────────────────────────────────────────── */

static int apply_relocations_for(struct input_obj *obj, int rela_idx,
                                  unsigned long section_vaddr,
                                  unsigned long section_offset,
                                  unsigned long section_size)
{
    if (rela_idx < 0) return 0;
    struct elf64_ehdr *ehdr = obj->ehdr;
    struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + ehdr->e_shoff);
    struct elf64_shdr *relahdr = &shdrs[rela_idx];

    unsigned long nrela = relahdr->sh_size / relahdr->sh_entsize;
    struct elf64_rela *relas = (struct elf64_rela *)(obj->data + relahdr->sh_offset);
    const char *strtab = obj_strtab(obj);

    /* Find the symbol table for this object */
    struct elf64_shdr *symhdr = 0;
    unsigned long nsym = 0;
    struct elf64_sym *syms = 0;
    if (obj->idx_symtab >= 0) {
        symhdr = &shdrs[obj->idx_symtab];
        nsym = symhdr->sh_size / symhdr->sh_entsize;
        syms = (struct elf64_sym *)(obj->data + symhdr->sh_offset);
    }

    for (unsigned long ri = 0; ri < nrela; ri++) {
        unsigned long r_type = ELF64_R_TYPE(relas[ri].r_info);
        unsigned long r_sym = ELF64_R_SYM(relas[ri].r_info);
        long addend = relas[ri].r_addend;
        unsigned long r_off = relas[ri].r_offset;

        if (r_off >= section_size) {
            err_msg("relocation offset out of bounds", "");
            return -1;
        }

        unsigned char *patch_loc = g_sections[0].data + section_offset + r_off;
        unsigned long S = 0;

        if (r_sym < nsym && syms) {
            unsigned char bind = ELF64_ST_BIND(syms[r_sym].st_info);
            const char *sym_name = "";
            if (syms[r_sym].st_name > 0)
                sym_name = strtab + syms[r_sym].st_name;

            if (syms[r_sym].st_shndx == SHN_UNDEF || bind == STB_GLOBAL || bind == STB_WEAK) {
                /* Look up in global symbol table */
                struct link_sym *ls = 0;
                for (int i = 0; i < g_nsyms; i++) {
                    if (strcmp(g_symtab[i].name, sym_name) == 0) {
                        ls = &g_symtab[i];
                        break;
                    }
                }
                if (ls && ls->defined) {
                    S = ls->value;
                } else {
                    printf("ld: undefined symbol '%s'\n", sym_name);
                }
            } else if (syms[r_sym].st_shndx < ehdr->e_shnum) {
                /* Local symbol — compute its address */
                unsigned short shndx = syms[r_sym].st_shndx;
                unsigned long sym_val = syms[r_sym].st_value;
                /* Compute section base address */
                if (shndx == obj->idx_text) {
                    S = g_sections[0].vaddr + section_offset + sym_val;
                } else if (shndx == obj->idx_data) {
                    S = g_sections[1].vaddr + section_offset + sym_val;
                } else if (shndx == obj->idx_rodata) {
                    S = g_sections[2].vaddr + section_offset + sym_val;
                } else {
                    S = sym_val;
                }
            } else {
                S = syms[r_sym].st_value;
            }
        }

        unsigned long P = section_vaddr + r_off;

        switch (r_type) {
            case R_X86_64_NONE:
                break;

            case R_X86_64_64:
                /* S + A */
                wr64(patch_loc, S + addend);
                break;

            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                /* S + A - P (32-bit) */
                wr32(patch_loc, (unsigned int)((S + addend - P) & 0xffffffffUL));
                break;

            case R_X86_64_PC64:
                /* S + A - P (64-bit) */
                wr64(patch_loc, S + addend - P);
                break;

            case R_X86_64_32:
            case R_X86_64_32S:
                /* S + A (32-bit) */
                wr32(patch_loc, (unsigned int)((S + addend) & 0xffffffffUL));
                break;

            default:
                printf("ld: unsupported relocation type %lu\n", r_type);
                g_exit_code = 1;
                return -1;
        }
    }
    return 0;
}

/* ── Apply all relocations across all objects ───────────────────── */

static int apply_all_relocations(void) {
    for (int oi = 0; oi < g_nobj; oi++) {
        struct input_obj *obj = &g_objs[oi];
        struct elf64_ehdr *ehdr = obj->ehdr;
        struct elf64_shdr *shdrs = (struct elf64_shdr *)(obj->data + ehdr->e_shoff);
        unsigned short shnum = ehdr->e_shnum;

        /* Compute the offset of this object's .text in the merged section */
        unsigned long text_offset = 0;
        unsigned long data_offset = 0;
        unsigned long rodata_offset = 0;
        for (int po = 0; po < oi; po++) {
            struct input_obj *pobj = &g_objs[po];
            struct elf64_ehdr *pe = pobj->ehdr;
            struct elf64_shdr *ps = (struct elf64_shdr *)(pobj->data + pe->e_shoff);
            if (pobj->idx_text >= 0) text_offset += ps[pobj->idx_text].sh_size;
            if (pobj->idx_data >= 0) data_offset += ps[pobj->idx_data].sh_size;
            if (pobj->idx_rodata >= 0) rodata_offset += ps[pobj->idx_rodata].sh_size;
        }

        /* Scan all sections for .rela.text and .rela.data */
        for (unsigned short i = 0; i < shnum; i++) {
            if (shdrs[i].sh_type != SHT_RELA) continue;
            unsigned int info = shdrs[i].sh_info;   /* section to apply to */

            if (info >= shnum) continue;

            unsigned long section_vaddr = 0;
            unsigned long section_offset = 0;
            unsigned long section_size = 0;

            if (obj->idx_text >= 0 && (int)info == obj->idx_text) {
                section_vaddr = g_sections[0].vaddr;
                section_offset = text_offset;
                /* We need the size of this object's .text */
                section_size = shdrs[obj->idx_text].sh_size;
            } else if (obj->idx_data >= 0 && (int)info == obj->idx_data) {
                section_vaddr = g_sections[1].vaddr;
                section_offset = data_offset;
                section_size = shdrs[obj->idx_data].sh_size;
            } else if (obj->idx_rodata >= 0 && (int)info == obj->idx_rodata) {
                section_vaddr = g_sections[2].vaddr;
                section_offset = rodata_offset;
                section_size = shdrs[obj->idx_rodata].sh_size;
            } else {
                continue; /* Section not in our merge list */
            }

            /* Adjust for section vaddr base (the merged section base, not per-object) */
            /* We need to pick the right section data buffer */
            /* Relocatable references use absolute addresses within the merged output */

            if (apply_relocations_for(obj, i, section_vaddr, section_offset, section_size) < 0)
                return -1;
        }
    }
    return 0;
}

/* ── Write output ELF executable ────────────────────────────────── */

static int write_output(const char *out_path) {
    /* Create output file */
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        err_msg("cannot create '%s'", out_path);
        return -1;
    }

    /* Build ELF header */
    struct elf64_ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));

    /* e_ident */
    ehdr.e_ident[0] = 0x7f;
    ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';
    ehdr.e_ident[3] = 'F';
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;

    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = g_sections[0].vaddr; /* entry = start of .text */
    ehdr.e_phoff = sizeof(struct elf64_ehdr);
    ehdr.e_shoff = 0; /* no section headers in output */
    ehdr.e_ehsize = sizeof(struct elf64_ehdr);
    ehdr.e_phentsize = sizeof(struct elf64_phdr);
    ehdr.e_phnum = 1; /* single PT_LOAD */
    ehdr.e_shentsize = sizeof(struct elf64_shdr);
    ehdr.e_shnum = 0;
    ehdr.e_shstrndx = 0;

    /* Write ELF header */
    if (write(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        err_msg("write error on '%s'", out_path);
        close(fd);
        return -1;
    }

    /* Build program header for single PT_LOAD covering text+rodata+data+bss */
    struct elf64_phdr phdr;
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type = PT_LOAD;

    /* Flags: read + write + execute (we'll set accordingly) */
    unsigned int p_flags = 5; /* PF_R | PF_X */
    /* If we have data, also PF_W */
    if (g_sections[1].size > 0 || g_sections[3].size > 0)
        p_flags |= 2; /* PF_W */

    phdr.p_flags = p_flags;

    /* Load address — start at text vaddr */
    unsigned long load_vaddr = g_sections[0].vaddr;
    unsigned long load_offset = g_sections[0].offset;

    /* Calculate the range covering all sections */
    unsigned long mem_end = g_sections[3].vaddr + g_sections[3].size;
    /* If no bss, end at data */
    if (g_sections[1].size > 0 && mem_end < g_sections[1].vaddr + g_sections[1].size)
        mem_end = g_sections[1].vaddr + g_sections[1].size;
    if (g_sections[2].size > 0 && mem_end < g_sections[2].vaddr + g_sections[2].size)
        mem_end = g_sections[2].vaddr + g_sections[2].size;
    if (mem_end < g_sections[0].vaddr + g_sections[0].size)
        mem_end = g_sections[0].vaddr + g_sections[0].size;

    /* Page-align start */
    load_vaddr = load_vaddr & ~(0xfffUL);
    load_offset = load_offset & ~(0xfffUL);

    /* Filesize: from load_offset to end of last progbits section */
    unsigned long file_end = g_sections[1].offset + g_sections[1].size;
    if (g_sections[2].offset + g_sections[2].size > file_end)
        file_end = g_sections[2].offset + g_sections[2].size;
    if (g_sections[0].offset + g_sections[0].size > file_end)
        file_end = g_sections[0].offset + g_sections[0].size;

    phdr.p_offset = load_offset;
    phdr.p_vaddr = load_vaddr;
    phdr.p_paddr = load_vaddr;
    phdr.p_filesz = file_end - load_offset;
    phdr.p_memsz = mem_end - load_vaddr;
    phdr.p_align = 0x1000;

    if (write(fd, &phdr, sizeof(phdr)) != sizeof(phdr)) {
        err_msg("write error on '%s'", out_path);
        close(fd);
        return -1;
    }

    /* Write section data at their file offsets */
    /* We need to pad to .text offset */
    unsigned long cur_pos = sizeof(ehdr) + sizeof(phdr);
    unsigned long pad = g_sections[0].offset - cur_pos;
    if (pad > 0) {
        char *zeros = malloc(pad);
        if (zeros) {
            memset(zeros, 0, pad);
            write(fd, zeros, pad);
            free(zeros);
        }
    }

    /* Write .text */
    if (g_sections[0].size > 0 && g_sections[0].data) {
        /* Seek to correct offset */
        lseek(fd, g_sections[0].offset, SEEK_SET);
        if (write(fd, g_sections[0].data, g_sections[0].size) != (long)g_sections[0].size) {
            err_msg("write error writing .text", "");
            close(fd);
            return -1;
        }
    }

    /* Write .rodata */
    if (g_sections[2].size > 0 && g_sections[2].data) {
        lseek(fd, g_sections[2].offset, SEEK_SET);
        if (write(fd, g_sections[2].data, g_sections[2].size) != (long)g_sections[2].size) {
            err_msg("write error writing .rodata", "");
            close(fd);
            return -1;
        }
    }

    /* Write .data */
    if (g_sections[1].size > 0 && g_sections[1].data) {
        lseek(fd, g_sections[1].offset, SEEK_SET);
        if (write(fd, g_sections[1].data, g_sections[1].size) != (long)g_sections[1].size) {
            err_msg("write error writing .data", "");
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *out_path = "a.out";
    int first_arg = 1;

    /* Parse arguments */
    if (argc < 2) {
        printf("Usage: ld [-o output] input1.o [input2.o ...]\n");
        return 1;
    }

    if (argc > 2 && strcmp(argv[1], "-o") == 0) {
        out_path = argv[2];
        first_arg = 3;
    } else if (argc > 1 && strcmp(argv[1], "-o") == 0) {
        /* -o with no filename */
        first_arg = 3;
    }

    if (first_arg >= argc) {
        printf("ld: no input files\n");
        return 1;
    }

    /* Parse input objects */
    for (int i = first_arg; i < argc; i++) {
        if (g_nobj >= MAX_OBJS) {
            printf("ld: too many input files (max 256)\n");
            g_exit_code = 1;
            break;
        }
        if (parse_obj(&g_objs[g_nobj], argv[i]) < 0) {
            /* parse_obj already set error message */
            continue;
        }
        g_nobj++;
    }

    if (g_nobj == 0) {
        err_msg("no valid input files", "");
        return 1;
    }

    /* Phase 1: Collect all global symbols */
    collect_symbols();

    /* Phase 2: Layout merged sections */
    layout_sections();

    /* Phase 3: Resolve symbol values */
    resolve_symbols();

    /* Phase 4: Apply relocations */
    if (apply_all_relocations() < 0) {
        return 1;
    }

    /* Phase 5: Write output */
    if (write_output(out_path) < 0) {
        return 1;
    }

    if (g_exit_code == 0)
        printf("ld: wrote '%s' (%d objects linked)\n", out_path, g_nobj);

    return g_exit_code;
}
