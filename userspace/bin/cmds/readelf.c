/*
 * readelf — display ELF header and section headers of an ELF64 file.
 *
 * GNU readelf style: prints the file header (-h) and the section header
 * table (-S).  Section names are resolved through the section-name string
 * table (.shstrtab).  Unrecognised section types are shown numerically
 * instead of failing.
 *
 * Usage: readelf <elf-file>
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

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_SHLIB 10
#define SHT_DYNSYM 11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_GROUP 17
#define SHT_SYMTAB_SHNDX 18
#define SHT_RELR 19

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_MERGE 0x10
#define SHF_STRINGS 0x20
#define SHF_INFO_LINK 0x40
#define SHF_LINK_ORDER 0x80
#define SHF_OS_NONCONFORMING 0x100
#define SHF_GROUP 0x200
#define SHF_TLS 0x400

static const char *e_type_name(unsigned short t) {
    switch (t) {
    case 0: return "NONE";
    case 1: return "REL";
    case 2: return "EXEC";
    case 3: return "DYN";
    case 4: return "CORE";
    default: return "LOOS/none";
    }
}

static const char *sh_type_name(unsigned int t) {
    switch (t) {
    case SHT_NULL: return "NULL";
    case SHT_PROGBITS: return "PROGBITS";
    case SHT_SYMTAB: return "SYMTAB";
    case SHT_STRTAB: return "STRTAB";
    case SHT_RELA: return "RELA";
    case SHT_HASH: return "HASH";
    case SHT_DYNAMIC: return "DYNAMIC";
    case SHT_NOTE: return "NOTE";
    case SHT_NOBITS: return "NOBITS";
    case SHT_REL: return "REL";
    case SHT_SHLIB: return "SHLIB";
    case SHT_DYNSYM: return "DYNSYM";
    case SHT_INIT_ARRAY: return "INIT_ARRAY";
    case SHT_FINI_ARRAY: return "FINI_ARRAY";
    case SHT_GROUP: return "GROUP";
    case SHT_SYMTAB_SHNDX: return "SYMTAB_SHNDX";
    case SHT_RELR: return "RELR";
    default: return NULL; /* print numerically */
    }
}

static void flag_bits(unsigned long long f, char *out, int cap) {
    int i = 0;
    if (f & SHF_WRITE) out[i++] = 'W';
    if (f & SHF_ALLOC) out[i++] = 'A';
    if (f & SHF_EXECINSTR) out[i++] = 'X';
    if (f & SHF_MERGE) out[i++] = 'M';
    if (f & SHF_STRINGS) out[i++] = 'S';
    if (f & SHF_INFO_LINK) out[i++] = 'I';
    if (f & SHF_LINK_ORDER) out[i++] = 'L';
    if (f & SHF_OS_NONCONFORMING) out[i++] = 'O';
    if (f & SHF_GROUP) out[i++] = 'G';
    if (f & SHF_TLS) out[i++] = 'T';
    if (i == 0) out[i++] = ' ';
    if (i >= cap) i = cap - 1;
    out[i] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("usage: readelf <elf-file>");
        return 1;
    }
    int fd = open(argv[1], O_RDONLY, 0);
    if (fd < 0) {
        printf("readelf: %s: cannot open\n", argv[1]);
        return 1;
    }
    long end = lseek(fd, 0, 2); /* SEEK_END=2 */
    if (end < 0) {
        close(fd);
        return 1;
    }
    if (end > 32 * 1024 * 1024) {
        puts("readelf: file too large");
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
        printf("readelf: %s: not an ELF file\n", argv[1]);
        free(img);
        return 1;
    }
    struct elf64_header *eh = (struct elf64_header *)img;
    if (img[4] != 2) {
        puts("readelf: not 64-bit");
        free(img);
        return 1;
    }

    printf("ELF Header:\n");
    printf("  Magic:   %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
           img[0], img[1], img[2], img[3], img[4], img[5], img[6], img[7], img[8], img[9],
           img[10], img[11], img[12], img[13], img[14], img[15]);
    printf("  Class:                             ELF64\n");
    printf("  Data:                              %s endian\n",
           img[5] == 1 ? "little" : "unknown");
    printf("  Version:                           %u\n", eh->e_version);
    printf("  Type:                              %s (%u)\n", e_type_name(eh->e_type),
           eh->e_type);
    printf("  Machine:                           %s (%u)\n", eh->e_machine == 62 ? "x86-64" : "unknown",
           eh->e_machine);
    printf("  Entry point address:               0x%llx\n", eh->e_entry);
    printf("  Start of program headers:          %llu (bytes into file)\n", eh->e_phoff);
    printf("  Start of section headers:          %llu (bytes into file)\n", eh->e_shoff);
    printf("  Flags:                             0x%x\n", eh->e_flags);
    printf("  Size of this header:               %u (bytes)\n", eh->e_ehsize);
    printf("  Size of program headers:           %u (bytes)\n", eh->e_phentsize);
    printf("  Number of program headers:         %u\n", eh->e_phnum);
    printf("  Size of section headers:           %u (bytes)\n", eh->e_shentsize);
    printf("  Number of section headers:         %u\n", eh->e_shnum);
    printf("  Section header string table index: %u\n", eh->e_shstrndx);

    printf("\nSection Headers:\n");
    printf("  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al\n");
    unsigned short shnum = eh->e_shnum;
    unsigned long long shoff = eh->e_shoff;
    unsigned short shstrndx = eh->e_shstrndx;

    /* Locate .shstrtab for section-name resolution. */
    const char *shstr = NULL;
    unsigned long shstrcap = 0;
    if (shstrndx < shnum) {
        if (shoff + (unsigned long long)(shstrndx + 1) * sizeof(struct elf64_shdr) <=
            (unsigned long long)end) {
            struct elf64_shdr *ss =
                (struct elf64_shdr *)(img + shoff +
                                      (unsigned long long)shstrndx * sizeof(struct elf64_shdr));
            unsigned long long o = ss->sh_offset;
            if (o + ss->sh_size <= (unsigned long long)end) {
                shstr = (const char *)(img + o);
                shstrcap = (unsigned long)ss->sh_size;
            }
        }
    }

    if (shoff == 0 || shnum == 0 || eh->e_shentsize < sizeof(struct elf64_shdr)) {
        puts("readelf: no section headers");
    } else {
        unsigned short n = 0;
        while (n < shnum) {
            if (shoff + (unsigned long long)(n + 1) * sizeof(struct elf64_shdr) >
                (unsigned long long)end) {
                printf("readelf: truncated section headers near index %u\n", n);
                break;
            }
            struct elf64_shdr *sh =
                (struct elf64_shdr *)(img + shoff + (unsigned long long)n * sizeof(struct elf64_shdr));

            char name[64];
            unsigned int no = sh->sh_name;
            if (shstr && no < shstrcap) {
                unsigned long i;
                for (i = 0; i < sizeof(name) - 1 && no + i < shstrcap && shstr[no + i]; i++)
                    name[i] = shstr[no + i];
                name[i] = '\0';
            } else {
                name[0] = '\0';
            }

            const char *tn = sh_type_name(sh->sh_type);
            char tbuf[16];
            if (!tn) {
                /* show the numeric type for unrecognised section types */
                tbuf[0] = '#';
                tbuf[1] = '\0';
                /* sprintf not used; build decimal manually */
                unsigned long long v = sh->sh_type;
                char rev[16];
                int len = 0;
                do {
                    rev[len++] = (char)('0' + v % 10);
                    v /= 10;
                } while (v && len < 14);
                for (int i = 0; i < len; i++)
                    tbuf[1 + i] = rev[len - 1 - i];
                tbuf[1 + len] = '\0';
                tn = tbuf;
            }
            char flg[8];
            flag_bits(sh->sh_flags, flg, sizeof(flg));
            printf("  [%2u] %-17s %-15s %016llx %06llx %08llx %02llx %-3s %2u %2u %llu\n",
                   n, name, tn, sh->sh_addr, sh->sh_offset, sh->sh_size,
                   sh->sh_entsize, flg, sh->sh_link, sh->sh_info, sh->sh_addralign);
            n++;
        }
    }

    printf("\nKey to Flags:\n");
    printf("  W (write), A (alloc), X (execute), M (merge), S (strings), I (info),\n");
    printf("  L (link order), O (extra OS processing required), G (group), T (TLS)\n");
    free(img);
    return 0;
}