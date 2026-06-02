#ifndef ELF_H
#define ELF_H

#include "types.h"

/* ELF64 identification */
#define ELF_MAGIC   0x464C457F  /* "\\x7FELF" as LE uint32 */
#define ET_EXEC     2           /* Executable file */
#define ET_DYN      3           /* Position-independent executable */
#define ET_REL      1           /* Relocatable object file */
#define EM_X86_64   62
#define PT_LOAD     1           /* Loadable segment */
#define PT_DYNAMIC  2           /* Dynamic linking info */
#define ELF_CLASS64 2
#define ELF_DATA2LSB 1

/* Section types (SHT_*) */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_DYNSYM   11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15

/* Section flags (SHF_*) */
#define SHF_WRITE     0x0001
#define SHF_ALLOC     0x0002
#define SHF_EXECINSTR 0x0004
#define SHF_MERGE     0x0010
#define SHF_STRINGS   0x0020
#define SHF_INFO_LINK 0x0040
#define SHF_TLS       0x0400

/* Relocation types (R_X86_64_*) — subset needed for kernel modules */
#define R_X86_64_NONE    0
#define R_X86_64_64      1  /* S + A (absolute 64-bit) */
#define R_X86_64_PC32    2  /* S + A - P (32-bit relative) */
#define R_X86_64_GOT32   3
#define R_X86_64_PLT32   4  /* S + A - P (PLT-relative, same as PC32 for kernel) */
#define R_X86_64_COPY    5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32       10 /* S + A (32-bit absolute, zero-extend) */
#define R_X86_64_32S      11 /* S + A (32-bit absolute, sign-extend) */
#define R_X86_64_PC64     24 /* S + A - P (64-bit relative) */

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

/* Section header — for ELF .ko module parsing */
struct elf64_shdr {
    uint32_t sh_name;      /* index into section header string table */
    uint32_t sh_type;      /* SHT_* */
    uint64_t sh_flags;     /* SHF_* */
    uint64_t sh_addr;      /* virtual address (0 if not loaded) */
    uint64_t sh_offset;    /* offset in file */
    uint64_t sh_size;      /* size in bytes */
    uint32_t sh_link;      /* index of associated section (symtab, etc.) */
    uint32_t sh_info;      /* extra info (depends on sh_type) */
    uint64_t sh_addralign; /* alignment constraint */
    uint64_t sh_entsize;   /* entry size for table sections */
} __attribute__((packed));

/* Symbol table entry */
struct elf64_sym {
    uint32_t st_name;      /* index into string table */
    uint8_t  st_info;      /* type and binding */
    uint8_t  st_other;     /* visibility */
    uint16_t st_shndx;     /* section header index (SHN_UNDEF=0 for imports) */
    uint64_t st_value;     /* value (address or offset) */
    uint64_t st_size;      /* size of object */
} __attribute__((packed));

/* Symbol binding (top 4 bits of st_info) */
#define ELF64_ST_BIND(i)      ((i) >> 4)
#define ELF64_ST_TYPE(i)      ((i) & 0x0F)
#define ELF64_ST_INFO(b,t)    (((b) << 4) | ((t) & 0x0F))
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3

/* Special section indices */
#define SHN_UNDEF     0
#define SHN_ABS       0xFFF1
#define SHN_COMMON    0xFFF2

/* Relocation entry (RELA — with explicit addend) */
struct elf64_rela {
    uint64_t r_offset;     /* location to be patched (relative to section) */
    uint64_t r_info;       /* symbol index + type */
    int64_t  r_addend;     /* constant addend */
} __attribute__((packed));

/* Extract symbol index and relocation type from r_info */
#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((uint32_t)(i))
#define ELF64_R_INFO(s,t) ((((uint64_t)(s)) << 32) | (uint32_t)(t))

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

/*
 * execve: replace the current user process image with a new ELF.
 * Destroys old user page tables, loads the new binary, allocates
 * a fresh user stack, and redirects the syscall return path to
 * start execution at the new entry point with rax=0.
 * Kernel threads cannot call execve (must be a user process).
 * argv/envp are currently not passed to userspace.
 */
int process_execve(const char *path, char *const argv[], char *const envp[]);

#endif
