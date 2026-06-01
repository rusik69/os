#include "seccomp_bpf.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"
#include "heap.h"

/*
 * seccomp-bpf implementation.
 *
 * Each process can have at most one seccomp filter installed.
 * The filter is stored inside struct process (seccomp_filter),
 * and evaluated by the BPF virtual machine before each syscall.
 */

/* A filter is simply a copy of the sock_fprog + the instruction array. */
struct seccomp_filter {
    struct sock_filter *insns;    /* heap-allocated instruction array */
    uint16_t           len;      /* number of instructions */
    int                refcount;
};

static int seccomp_initialised;

void seccomp_bpf_init(void)
{
    if (seccomp_initialised)
        return;

    seccomp_initialised = 1;

    kprintf("[OK] seccomp-bpf: syscall filter initialised\n");
}

/* ── BPF virtual machine ─────────────────────────────────────── */

/* The seccomp BPF VM operates on a fixed-length seccomp_data structure.
 * The data buffer (pointed to by A = data + offset) contains:
 *   [0..3]   nr        (syscall number, 32-bit, little-endian)
 *   [4..7]   arch      (architecture, 32-bit)
 *   [8..15]  instruction_pointer (64-bit)
 *   [16..63] args[0..5] (each 64-bit)
 *
 * The accumulator 'A' holds a 32-bit value.  The temporary 'X' holds 32 bits.
 * Jump offsets are added to the PC (which points to 'next' instruction).
 **/

/* Evaluate a single seccomp filter program.
 * Returns the SECCOMP_RET_* value. */
static uint32_t seccomp_run_filter(struct seccomp_data *sd,
                                   const struct sock_filter *insns,
                                   uint16_t len)
{
    uint32_t A = 0;        /* accumulator */
    uint32_t X = 0;        /* index register */
    int      pc = 0;       /* program counter */

    if (!insns || len == 0)
        return SECCOMP_RET_ALLOW;

    while (pc >= 0 && pc < len) {
        const struct sock_filter *inst = &insns[pc];
        uint16_t code = inst->code;

        switch (code) {
        case BPF_LD | BPF_W | BPF_ABS: {
            /* A = *(uint32_t *)(seccomp_data + k) */
            if (inst->k + 4 <= SECCOMP_DATA_SIZE) {
                uint32_t val = 0;
                /* Read 4 bytes at offset k from seccomp_data */
                const uint8_t *base = (const uint8_t *)sd;
                val = ((uint32_t)base[inst->k]) |
                      ((uint32_t)base[inst->k + 1] << 8) |
                      ((uint32_t)base[inst->k + 2] << 16) |
                      ((uint32_t)base[inst->k + 3] << 24);
                A = val;
            } else {
                A = 0;
            }
            pc++;
            break;
        }

        case BPF_LD | BPF_W | BPF_IND: {
            /* A = *(uint32_t *)(seccomp_data + X + k) */
            uint32_t offset = X + inst->k;
            if (offset + 4 <= SECCOMP_DATA_SIZE) {
                const uint8_t *base = (const uint8_t *)sd;
                A = ((uint32_t)base[offset]) |
                    ((uint32_t)base[offset + 1] << 8) |
                    ((uint32_t)base[offset + 2] << 16) |
                    ((uint32_t)base[offset + 3] << 24);
            } else {
                A = 0;
            }
            pc++;
            break;
        }

        case BPF_JMP | BPF_JEQ: {
            /* Jump if A == k */
            uint32_t target = (A == inst->k) ? inst->jt : inst->jf;
            pc = pc + 1 + (int)target;
            break;
        }

        case BPF_JMP | BPF_JGT: {
            /* Jump if A > k (unsigned) */
            uint32_t target = (A > inst->k) ? inst->jt : inst->jf;
            pc = pc + 1 + (int)target;
            break;
        }

        case BPF_JMP | BPF_JGE: {
            /* Jump if A >= k (unsigned) */
            uint32_t target = (A >= inst->k) ? inst->jt : inst->jf;
            pc = pc + 1 + (int)target;
            break;
        }

        case BPF_JMP | BPF_JSET: {
            /* Jump if (A & k) != 0 */
            uint32_t target = (A & inst->k) ? inst->jt : inst->jf;
            pc = pc + 1 + (int)target;
            break;
        }

        case BPF_RET: {
            /* Return k */
            return inst->k;
        }

        default:
            /* Unknown instruction: skip */
            pc++;
            break;
        }
    }

    /* If we fall off the end, allow */
    return SECCOMP_RET_ALLOW;
}

/* ── Public API ───────────────────────────────────────────────── */

int seccomp_filter_install(const struct sock_fprog *prog)
{
    if (!seccomp_initialised)
        return -ENOSYS;
    if (!prog || !prog->filter)
        return -EFAULT;
    if (prog->len == 0 || prog->len > 4096)  /* sanity limit */
        return -EINVAL;

    struct process *current = process_get_current();
    if (!current)
        return -ESRCH;

    /* If the process already has a seccomp filter, reject */
    if (current->seccomp_mode != 0 && current->seccomp_filter != NULL)
        return -EEXIST;

    /* Allocate and copy the filter program */
    size_t insn_size = prog->len * sizeof(struct sock_filter);
    struct sock_filter *insns = (struct sock_filter *)kmalloc(insn_size);
    if (!insns)
        return -ENOMEM;

    memcpy(insns, prog->filter, insn_size);

    /* Allocate the filter wrapper */
    struct seccomp_filter *filter = (struct seccomp_filter *)kmalloc(sizeof(struct seccomp_filter));
    if (!filter) {
        kfree(insns);
        return -ENOMEM;
    }

    filter->insns    = insns;
    filter->len      = prog->len;
    filter->refcount = 1;

    current->seccomp_filter = filter;
    current->seccomp_mode   = 2;   /* SECCOMP_MODE_FILTER */

    return 0;
}

uint32_t seccomp_filter_evaluate(int syscall_nr, uint32_t arch)
{
    struct process *current = process_get_current();
    if (!current)
        return SECCOMP_RET_ALLOW;

    if (current->seccomp_mode != 2 || !current->seccomp_filter)
        return SECCOMP_RET_ALLOW;

    /* Build the seccomp_data structure */
    struct seccomp_data sd;
    memset(&sd, 0, sizeof(sd));
    sd.nr   = syscall_nr;
    sd.arch = arch;

    struct seccomp_filter *filter = (struct seccomp_filter *)current->seccomp_filter;

    return seccomp_run_filter(&sd, filter->insns, filter->len);
}
