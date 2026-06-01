#ifndef SECCOMP_BPF_H
#define SECCOMP_BPF_H

#include "types.h"

/*
 * seccomp-bpf — Berkeley Packet Filter-based syscall filtering.
 *
 * This is a minimal implementation of Linux seccomp(2) with
 * SECCOMP_SET_MODE_FILTER.  Filters are written as a sequence
 * of BPF instructions (struct sock_filter) encapsulated in a
 * struct sock_fprog.
 */

/* BPF instruction classes (subset used by seccomp) */
#define BPF_LD    0x00   /* load operation */
#define BPF_JMP   0x05   /* jump operation */
#define BPF_RET   0x06   /* return operation */
#define BPF_W     0x00   /* 32-bit word size */
#define BPF_ABS   0x20   /* absolute load (from seccomp_data) */
#define BPF_IND   0x40   /* indirect load (from seccomp_data + X) */

/* BPF jump conditions */
#define BPF_JEQ   0x10   /* jump if equal */
#define BPF_JGT   0x20   /* jump if greater than */
#define BPF_JGE   0x30   /* jump if greater or equal */
#define BPF_JSET  0x40   /* jump if bits set */

/* seccomp return values */
#ifndef SECCOMP_RET_KILL
#define SECCOMP_RET_KILL      0x00000000
#define SECCOMP_RET_ALLOW     0x7FFF0000
#define SECCOMP_RET_ERRNO     0x00050000   /* OR with errno value */
#endif

/* Size of seccomp_data structure (simplified — just syscall nr and arch) */
#define SECCOMP_DATA_SIZE  16

struct seccomp_data {
    int   nr;                   /* syscall number */
    uint32_t arch;              /* architecture AUDIT_ARCH_* */
    uint64_t instruction_pointer;    /* CPU instruction pointer */
    uint64_t args[6];           /* syscall arguments */
};

/* BPF instruction for seccomp filter programs */
struct sock_filter {
    uint16_t code;       /* filter code (BPF_LD | BPF_JMP | BPF_ABS | BPF_RET ...) */
    uint8_t  jt;         /* jump true offset (in instructions) */
    uint8_t  jf;         /* jump false offset */
    uint32_t k;          /* generic multi-purpose field */
} __attribute__((packed));

/* sock_fprog: a complete filter program */
struct sock_fprog {
    uint16_t          len;        /* number of filter instructions */
    struct sock_filter *filter;   /* pointer to instructions */
};

/* ── API ──────────────────────────────────────────────────────── */

/* Install a seccomp filter for the current process.
 * Returns 0 on success, negative errno on failure. */
int seccomp_filter_install(const struct sock_fprog *prog);

/* Evaluate a seccomp filter for a given syscall number and architecture.
 * Returns the SECCOMP_RET_* action. */
uint32_t seccomp_filter_evaluate(int syscall_nr, uint32_t arch);

/* Init called during kernel boot */
void seccomp_bpf_init(void);

#endif /* SECCOMP_BPF_H */
