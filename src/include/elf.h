#ifndef ELF_H
#define ELF_H

#include "types.h"

/* ELF64 identification */
#define ELF_MAGIC   0x464C457F  /* "\x7FELF" as LE uint32 */
#define ET_EXEC     2           /* Executable file */
#define ET_DYN      3           /* Position-independent executable */
#define EM_X86_64   62
#define PT_LOAD     1           /* Loadable segment */
#define ELF_CLASS64 2
#define ELF_DATA2LSB 1

struct elf64_header {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;    /* program header offset */
    uint64_t e_shoff;    /* section header offset */
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;   /* offset in file */
    uint64_t p_vaddr;    /* virtual address to load at */
    uint64_t p_paddr;
    uint64_t p_filesz;   /* bytes in file */
    uint64_t p_memsz;    /* bytes in memory (may be > filesz; rest zeroed) */
    uint64_t p_align;
} __attribute__((packed));

struct elf_load_result {
    uint64_t entry;      /* entry point virtual address */
    uint64_t load_base;  /* lowest virtual address loaded */
    int      error;      /* 0 = success */
};

/*
 * Load ELF64 binary from a buffer in memory.
 * Segments are loaded at their p_vaddr (statically linked).
 * Returns entry point or 0 on failure.
 */
uint64_t elf_load(const uint8_t *data, uint64_t size);

/*
 * Execute an ELF file from the filesystem (reads via VFS, then elf_load).
 * Creates a new kernel process whose entry jumps to the ELF entry point.
 * Returns 0 on success.
 */
int elf_exec(const char *path);

#endif
