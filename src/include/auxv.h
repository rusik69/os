#ifndef _AUXV_H
#define _AUXV_H

/* Auxiliary vector (auxv) entries for ELF initial stack layout.
 * Values from Linux x86_64 ABI (generic). */

typedef struct {
    uint64_t a_type;  /* Entry type */
    uint64_t a_val;   /* Entry value */
} Elf64_auxv_t;

/* Auxiliary vector types */
#define AT_NULL         0   /* End of vector */
#define AT_IGNORE       1   /* Ignore entry */
#define AT_EXECFD       2   /* File descriptor of program */
#define AT_PHDR         3   /* Address of program headers */
#define AT_PHENT        4   /* Size of program header entry */
#define AT_PHNUM        5   /* Number of program headers */
#define AT_PAGESZ       6   /* System page size */
#define AT_BASE         7   /* Base address of interpreter */
#define AT_FLAGS        8   /* Flags */
#define AT_ENTRY        9   /* Entry point of program */
#define AT_NOTELF       10  /* Program is not ELF */
#define AT_UID          11  /* Real user ID */
#define AT_EUID         12  /* Effective user ID */
#define AT_GID          13  /* Real group ID */
#define AT_EGID         14  /* Effective group ID */
#define AT_PLATFORM     15  /* String identifying CPU platform */
#define AT_HWCAP        16  /* Hardware capabilities */
#define AT_CLKTCK       17  /* Frequency of times() */
#define AT_SECURE       23  /* Secure mode boolean */
#define AT_BASE_PLATFORM 24 /* String identifying base platform */
#define AT_RANDOM       25  /* Address of 16 random bytes */
#define AT_HWCAP2       26  /* Extended hardware capabilities */
#define AT_EXECFN       31  /* Filename of executable */
#define AT_SYSINFO_EHDR 33  /* vDSO address */

#endif /* _AUXV_H */
