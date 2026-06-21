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

/* Additional BPF mode/size constants for packet filter (AF_PACKET) */
#define BPF_IMM   0x00   /* immediate value mode (LDX) */
#define BPF_LEN   0x80   /* packet length mode (LD) */
#define BPF_H     0x08   /* 16-bit half-word size */
#define BPF_B     0x10   /* 8-bit byte size */
#define BPF_LDX   0x01   /* load into X register */
#define BPF_ALU   0x04   /* ALU operation */
#define BPF_K     0x00   /* constant operand */
#define BPF_AND   0x50   /* ALU AND */
#define BPF_LSH   0x60   /* ALU left shift */
#define BPF_RSH   0x70   /* ALU right shift */
#define BPF_NEG   0x80   /* ALU negate */

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

/* Mode constant for BPF-based filters (distinct from legacy SECCOMP_MODE_FILTER=2) */
#define SECCOMP_MODE_FILTER_BPF  3

#ifndef SECCOMP_MODE_DISABLED
#define SECCOMP_MODE_DISABLED    0
#endif

/* BPF-specific return value constants (Linux-compatible).
 * Always defined, regardless of whether seccomp.h is also included. */
#define SECCOMP_BPF_RET_KILL      0x00000000U
#define SECCOMP_BPF_RET_ALLOW     0x7FFF0000U
#define SECCOMP_BPF_RET_TRAP      0x00030000U
#define SECCOMP_BPF_RET_LOG       0x7FFC0000U
#define SECCOMP_BPF_RET_ERRNO     0x00050000U

/* Mask to extract action from full 32-bit seccomp return value */
#define SECCOMP_RET_ACTION_FULL   0xFFFF0000U

/* x86_64 audit architecture constant (EM_X86_64 | AUDIT_ARCH_LE) */
#define AUDIT_ARCH_X86_64         0xC000003EU

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

/* Release the seccomp filter for the current process (frees instructions + filter struct). */
void seccomp_bpf_release(void);

/* Init called during kernel boot */
void seccomp_bpf_init(void);

#endif /* SECCOMP_BPF_H */
