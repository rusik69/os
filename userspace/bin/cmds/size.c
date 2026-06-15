/* size.c — display section sizes of an ELF binary */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Minimal ELF64 definitions */
#define EI_NIDENT 16

typedef unsigned long long Elf64_Addr;
typedef unsigned short      Elf64_Half;
typedef unsigned int        Elf64_Word;
typedef unsigned long long  Elf64_Off;
typedef unsigned long long  Elf64_Xword;
typedef signed long long    Elf64_Sxword;

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_REL      1
#define ET_EXEC     2
#define ET_DYN      3
#define SHT_STRTAB  3

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
};

struct elf64_shdr {
    Elf64_Word    sh_name;
    Elf64_Word    sh_type;
    Elf64_Xword   sh_flags;
    Elf64_Addr    sh_addr;
    Elf64_Off     sh_offset;
    Elf64_Xword   sh_size;
    Elf64_Word    sh_link;
    Elf64_Word    sh_info;
    Elf64_Xword   sh_addralign;
    Elf64_Xword   sh_entsize;
};

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: size <elf-file>\n");
        return 1;
    }

    const char *path = argv[1];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("size: cannot open '%s'\n", path);
        return 1;
    }

    /* Read ELF header */
    struct elf64_ehdr ehdr;
    int n = read(fd, &ehdr, sizeof(ehdr));
    if (n < (int)sizeof(ehdr)) {
        printf("size: not a valid ELF file (too short)\n");
        close(fd);
        return 1;
    }

    /* Verify ELF magic */
    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        printf("size: not an ELF file\n");
        close(fd);
        return 1;
    }

    /* Verify 64-bit */
    if (ehdr.e_ident[4] != ELFCLASS64) {
        printf("size: not a 64-bit ELF file\n");
        close(fd);
        return 1;
    }

    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0) {
        printf("size: no section headers\n");
        close(fd);
        return 1;
    }

    /* Read section header string table */
    if (ehdr.e_shstrndx >= ehdr.e_shnum) {
        printf("size: invalid section string table index\n");
        close(fd);
        return 1;
    }

    /* Read the section header for the string table */
    struct elf64_shdr shstrtab_hdr;
    lseek(fd, ehdr.e_shoff + ehdr.e_shstrndx * sizeof(struct elf64_shdr), SEEK_SET);
    n = read(fd, &shstrtab_hdr, sizeof(shstrtab_hdr));
    if (n < (int)sizeof(shstrtab_hdr)) {
        printf("size: failed to read string table header\n");
        close(fd);
        return 1;
    }

    /* Read string table data */
    char *strtab = malloc(shstrtab_hdr.sh_size);
    if (!strtab) {
        printf("size: out of memory\n");
        close(fd);
        return 1;
    }
    lseek(fd, shstrtab_hdr.sh_offset, SEEK_SET);
    n = read(fd, strtab, shstrtab_hdr.sh_size);
    if (n < (int)shstrtab_hdr.sh_size) {
        printf("size: failed to read string table\n");
        free(strtab);
        close(fd);
        return 1;
    }

    /* Print header */
    printf("section            size      addr\n");
    printf("----------------  ------  ----------\n");

    Elf64_Xword total_size = 0;

    /* Read section headers one at a time */
    for (int i = 0; i < ehdr.e_shnum; i++) {
        struct elf64_shdr shdr;
        lseek(fd, ehdr.e_shoff + i * sizeof(struct elf64_shdr), SEEK_SET);
        n = read(fd, &shdr, sizeof(shdr));
        if (n < (int)sizeof(shdr)) break;

        /* Skip SHT_NULL type sections */
        if (shdr.sh_type == 0 && i > 0) continue;

        /* Get section name */
        const char *sname = "";
        if (shdr.sh_name < shstrtab_hdr.sh_size)
            sname = strtab + shdr.sh_name;

        /* Print section info */
        /* Pad name to 16 chars */
        int nlen = strlen(sname);
        if (nlen > 16) nlen = 16;
        write(1, sname, nlen);
        for (int p = nlen; p < 16; p++) write(1, " ", 1);

        /* Print size right-aligned in 8 chars */
        write(1, "  ", 2);
        char szbuf[16];
        int szpos = 15;
        szbuf[15] = 0;
        Elf64_Xword sz = shdr.sh_size;
        if (sz == 0) {
            szpos = 14;
            szbuf[14] = '0';
        } else {
            while (sz > 0 && szpos > 0) {
                szpos--;
                szbuf[szpos] = '0' + (sz % 10);
                sz /= 10;
            }
        }
        /* Need 8 chars for alignment */
        int printed = 15 - szpos;
        for (int p = printed; p < 8; p++) write(1, " ", 1);
        write(1, szbuf + szpos, 15 - szpos);

        /* Print address */
        write(1, "  ", 2);
        char addrbuf[17];
        Elf64_Addr addr = shdr.sh_addr;
        addrbuf[16] = 0;
        int addrpos = 16;
        if (addr == 0) {
            addrpos = 15;
            addrbuf[15] = '0';
        } else {
            while (addr > 0 && addrpos > 0) {
                addrpos--;
                int digit = addr & 0xf;
                addrbuf[addrpos] = digit < 10 ? '0' + digit : 'a' + digit - 10;
                addr >>= 4;
            }
        }
        for (int p = addrpos; p > 0; p--) write(1, "0", 1);
        write(1, addrbuf + addrpos, 16 - addrpos);

        write(1, "\n", 1);
        total_size += shdr.sh_size;
    }

    /* Print total */
    printf("----------------  ------  ----------\n");
    char tbuf[32];
    int tpos = 31;
    tbuf[31] = 0;
    Elf64_Xword t = total_size;
    if (t == 0) {
        tpos = 30;
        tbuf[30] = '0';
    } else {
        while (t > 0 && tpos > 0) {
            tpos--;
            tbuf[tpos] = '0' + (t % 10);
            t /= 10;
        }
    }
    write(1, "Total            ", 17);
    int printed_t = 31 - tpos;
    for (int p = printed_t; p < 8; p++) write(1, " ", 1);
    write(1, tbuf + tpos, 31 - tpos);
    write(1, "\n", 1);

    free(strtab);
    close(fd);
    return 0;
}
