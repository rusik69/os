/*
 * nm — display the symbol table of an ELF64 file.
 *
 * GNU nm style: prints each symbol's address (or "        " for undefined),
 * a type letter from ELF_ST_TYPE, and the symbol name.  Symbols are read from
 * the .symtab section (or .dynsym if no .symtab); names are resolved through
 * the string table linked by that section's sh_link.  Type letters follow
 * the ELF <-> nm convention: A(abs), B/B(bss), D/D(data), T/T(text),
 * R/R(readonly), U(undefined), W/W(weak), etc.
 *
 * Usage: nm <elf-file>
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

/* ── ELF64 structures (userspace libc has no <elf.h>) ───────────────── */
struct elf64_header {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int e_version;
    unsigned long long e_entry;
    unsigned long long e_phoff;
    unsigned long long e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
} __attribute__((packed));

struct elf64_shdr {
    unsigned int sh_name;
    unsigned int sh_type;
    unsigned long long sh_flags;
    unsigned long long sh_addr;
    unsigned long long sh_offset;
    unsigned long long sh_size;
    unsigned int sh_link;
    unsigned int sh_info;
    unsigned long long sh_addralign;
    unsigned long long sh_entsize;
} __attribute__((packed));

struct elf64_sym {
    unsigned int st_name;
    unsigned char st_info;
    unsigned char st_other;
    unsigned short st_shndx;
    unsigned long long st_value;
    unsigned long long st_size;
} __attribute__((packed));

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_DYNSYM 11

#define SHF_ALLOC 0x2

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5

#define STB_GLOBAL 1
#define STB_WEAK 2

#define SHN_UNDEF 0

static char type_letter(unsigned char info, unsigned long long sh_flags) {
    unsigned char t = info & 0x0f;
    unsigned char b = (info >> 4) & 0x0f;
    char c;
    switch (t) {
    case STT_OBJECT:
        c = (sh_flags & SHF_ALLOC) ? 'D' : 'd';
        break;
    case STT_FUNC:
        c = (sh_flags & SHF_ALLOC) ? 'T' : 't';
        break;
    case STT_COMMON:
        c = 'C';
        break;
    case STT_NOTYPE:
    default:
        c = 'n';
        break;
    }
    /* weak binding lowercases and prefixes 'V'/'W' for data/func */
    if (b == STB_WEAK) {
        if (c == 'D' || c == 'd') c = (c == 'D') ? 'V' : 'v';
        else if (c == 'T' || c == 't') c = (c == 'T') ? 'W' : 'w';
        else if (c == 'n') c = 'w';
    } else if (b == STB_GLOBAL) {
        if (c != 'C') c = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    /* undefined symbols always print as 'U' */
    /* (handled by caller via SHN_UNDEF) */
    return c;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("usage: nm <elf-file>");
        return 1;
    }
    int fd = open(argv[1], O_RDONLY, 0);
    if (fd < 0) {
        printf("nm: %s: cannot open\n", argv[1]);
        return 1;
    }
    long end = lseek(fd, 0, 2); /* SEEK_END=2 */
    if (end < 0) {
        close(fd);
        return 1;
    }
    if (end > 32 * 1024 * 1024) {
        puts("nm: file too large");
        close(fd);
        return 1;
    }
    lseek(fd, 0, 0);
    unsigned char *img = malloc((unsigned long)end);
    if (!img) {
        close(fd);
        return 1;
    }
    long got = 0;
    while (got < end) {
        long r = read(fd, img + got, (unsigned long)(end - got));
        if (r <= 0)
            break;
        got += r;
    }
    close(fd);

    if ((unsigned long)end < 64 || img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' ||
        img[3] != 'F') {
        printf("nm: %s: not an ELF file\n", argv[1]);
        free(img);
        return 1;
    }
    struct elf64_header *eh = (struct elf64_header *)img;
    if (img[4] != 2) {
        puts("nm: not 64-bit");
        free(img);
        return 1;
    }

    unsigned short shnum = eh->e_shnum;
    unsigned long long shoff = eh->e_shoff;
    if (shoff == 0 || shnum == 0 || eh->e_shentsize < sizeof(struct elf64_shdr)) {
        puts("nm: no section headers");
        free(img);
        return 1;
    }

    /* Locate the symbol table (.symtab preferred, fall back to .dynsym)
     * and the string table linked via sh_link. */
    const struct elf64_shdr *symsec = NULL;
    int have_symtab = 0;
    unsigned short n;
    for (n = 0; n < shnum; n++) {
        if (shoff + (unsigned long long)(n + 1) * sizeof(struct elf64_shdr) >
            (unsigned long long)end)
            break;
        const struct elf64_shdr *sh =
            (const struct elf64_shdr *)(img + shoff +
                                        (unsigned long long)n * sizeof(struct elf64_shdr));
        if (sh->sh_type == SHT_SYMTAB && !have_symtab) {
            symsec = sh;
            have_symtab = 1;
        } else if (!symsec && sh->sh_type == SHT_DYNSYM) {
            symsec = sh;
        }
    }
    if (!symsec) {
        puts("nm: no symbol table found");
        free(img);
        return 1;
    }

    unsigned long long symoff = symsec->sh_offset;
    unsigned long long symsize = symsec->sh_size;
    unsigned long long entsz = symsec->sh_entsize;
    if (entsz == 0 || entsz > sizeof(struct elf64_sym)) {
        /* older tools may have a bogus sh_entsize; default to the struct size */
        entsz = sizeof(struct elf64_sym);
    }
    unsigned long long nsyms = symsize / entsz;

    /* String table for symbol names (sh_link of the symbol section). */
    const char *strtab = NULL;
    unsigned long strcap = 0;
    unsigned int link = symsec->sh_link;
    if (link < shnum &&
        shoff + (unsigned long long)(link + 1) * sizeof(struct elf64_shdr) <=
            (unsigned long long)end) {
        const struct elf64_shdr *ss =
            (const struct elf64_shdr *)(img + shoff +
                                        (unsigned long long)link * sizeof(struct elf64_shdr));
        unsigned long long o = ss->sh_offset;
        if (o + ss->sh_size <= (unsigned long long)end) {
            strtab = (const char *)(img + o);
            strcap = (unsigned long)ss->sh_size;
        }
    }

    /* Map section indexes -> SHF_ALLOC flag for the D/T vs d/t type hint. */
    unsigned short k;
    unsigned long long secflags[64];
    for (k = 0; k < shnum && k < 64; k++) {
        if (shoff + (unsigned long long)(k + 1) * sizeof(struct elf64_shdr) <=
            (unsigned long long)end) {
            const struct elf64_shdr *sh =
                (const struct elf64_shdr *)(img + shoff +
                                            (unsigned long long)k * sizeof(struct elf64_shdr));
            secflags[k] = sh->sh_flags;
        } else {
            secflags[k] = 0;
        }
    }

    if (symoff + symsize > (unsigned long long)end) {
        puts("nm: truncated symbol table");
        free(img);
        return 1;
    }

    unsigned long long i;
    for (i = 0; i < nsyms; i++) {
        const struct elf64_sym *s =
            (const struct elf64_sym *)(img + symoff + i * entsz);
        unsigned long long flags =
            (s->st_shndx < 64) ? secflags[s->st_shndx] : 0;
        char t = type_letter(s->st_info, flags);
        if (s->st_shndx == SHN_UNDEF)
            t = 'U';
        if ((s->st_info & 0x0f) == STT_FILE || (s->st_info & 0x0f) == STT_SECTION) {
            /* skip file/section markers like GNU nm's default */
            continue;
        }
        char name[256];
        unsigned int no = s->st_name;
        if (strtab && no < strcap) {
            unsigned long j;
            for (j = 0; j < sizeof(name) - 1 && no + j < strcap && strtab[no + j]; j++)
                name[j] = strtab[no + j];
            name[j] = '\0';
        } else {
            name[0] = '\0';
        }
        if (name[0] == '\0')
            continue;

        if (s->st_shndx == SHN_UNDEF && s->st_value == 0) {
            printf("         %c %s\n", t, name);
        } else {
            printf("%016llx %c %s\n", s->st_value, t, name);
        }
    }

    free(img);
    return 0;
}