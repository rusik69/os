/*
 * bpf_prog.c — eBPF program attachment and execution
 *
 * Provides the framework for attaching eBPF programs to kernel events:
 *   - kprobes: Attach to function entry/exit
 *   - tracepoints: Attach to static trace events
 *   - perf_events: Attach to performance monitoring events
 *
 * This module manages BPF program objects, their attachment to kernel
 * hooks, and the execution/JIT interface.
 *
 * Item 135 — eBPF program attachment to kprobes/tracepoints
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "kprobes.h"
#include "trace.h"

/* ── eBPF program types ─────────────────────────────────────────────── */

#define BPF_PROG_TYPE_UNSPEC       0
#define BPF_PROG_TYPE_KPROBE       1  /* kprobe-based instrumentation */
#define BPF_PROG_TYPE_TRACEPOINT  2  /* Tracepoint attachment */
#define BPF_PROG_TYPE_PERF_EVENT  3  /* Perf event attachment */
#define BPF_PROG_TYPE_SCHED       4  /* Scheduler hooks */

/* ── Program flags ───────────────────────────────────────────────────── */

#define BPF_F_SLEEPABLE    (1 << 0)

/* ── eBPF instruction (same as in bpf_verifier.c) ─────────────────── */

struct bpf_insn {
    uint8_t  code;
    uint8_t  dst_reg:4;
    uint8_t  src_reg:4;
    int16_t  off;
    int32_t  imm;
} __attribute__((packed));

/* ── Maximum supported programs ──────────────────────────────────────── */

#define BPF_MAX_PROGRAMS 32
#define BPF_MAX_INSTRUCTIONS 4096

/* ── BPF program object ─────────────────────────────────────────────── */

struct bpf_prog {
    int      in_use;
    int      type;                /* BPF_PROG_TYPE_* */
    char     name[32];

    /* Bytecode */
    struct bpf_insn *insns;
    int      insn_count;

    /* JIT-compiled code (or NULL if interpreter-only) */
    void    *jit_code;
    int      jit_size;

    /* Attachment type-specific data */
    union {
        struct {
            const char *symbol;   /* kprobe target symbol name */
            void *addr;           /* kprobe target address */
            int is_kretprobe;     /* 1 for return probe */
        } kprobe;
        struct {
            int event_id;         /* Trace event ID */
        } tracepoint;
    };

    /* Reference count */
    int refcnt;
};

/* ── Global program table ───────────────────────────────────────────── */

static struct bpf_prog bpf_progs[BPF_MAX_PROGRAMS];
static int bpf_progs_initialized = 0;

/* ── Forward declarations for JIT/interpreter ────────────────────────── */

static uint64_t bpf_interp_run(const struct bpf_insn *insns, int insn_count,
                                uint64_t *regs);

/* ── Public API ─────────────────────────────────────────────────────── */

void bpf_prog_init(void)
{
    if (bpf_progs_initialized) return;

    memset(bpf_progs, 0, sizeof(bpf_progs));
    bpf_progs_initialized = 1;

    kprintf("[bpf_prog] eBPF program framework initialized\n");
}

/*
 * Load an eBPF program.
 *
 * @type:      BPF_PROG_TYPE_*.
 * @name:      Human-readable name.
 * @insns:     Pointer to eBPF bytecode.
 * @insn_count: Number of instructions.
 *
 * Returns program FD (positive) on success, negative error otherwise.
 */
int bpf_prog_load(int type, const char *name,
                   const struct bpf_insn *insns, int insn_count)
{
    if (!bpf_progs_initialized)
        return -ENOSYS;

    if (!insns || insn_count <= 0 || insn_count > BPF_MAX_INSTRUCTIONS)
        return -EINVAL;

    if (type <= BPF_PROG_TYPE_UNSPEC || type > BPF_PROG_TYPE_SCHED)
        return -EINVAL;

    /* Find a free slot */
    int fd = -1;
    for (int i = 0; i < BPF_MAX_PROGRAMS; i++) {
        if (!bpf_progs[i].in_use) {
            fd = i + 1;
            break;
        }
    }

    if (fd < 0)
        return -ENOSPC;

    int idx = fd - 1;
    struct bpf_prog *prog = &bpf_progs[idx];
    prog->in_use = 1;
    prog->type = type;
    prog->refcnt = 1;

    if (name) {
        strncpy(prog->name, name, sizeof(prog->name) - 1);
        prog->name[sizeof(prog->name) - 1] = '\0';
    } else {
        snprintf(prog->name, sizeof(prog->name), "bpf_prog_%d", fd);
    }

    /* Copy instructions */
    prog->insns = (struct bpf_insn *)kmalloc(insn_count * sizeof(struct bpf_insn));
    if (!prog->insns) {
        prog->in_use = 0;
        return -ENOMEM;
    }

    memcpy(prog->insns, insns, insn_count * sizeof(struct bpf_insn));
    prog->insn_count = insn_count;
    prog->jit_code = NULL;
    prog->jit_size = 0;

    kprintf("[bpf_prog] Loaded program '%s' (fd=%d, type=%d, %d insns)\n",
            prog->name, fd, type, insn_count);

    return fd;
}

/*
 * Attach an eBPF program to a kernel hook.
 *
 * @prog_fd:  Program file descriptor from bpf_prog_load().
 * @type:     Target hook type (kprobe, tracepoint, etc.)
 * @target:   Target specification (symbol name, event ID, etc.)
 *
 * Returns 0 on success, negative on error.
 */
int bpf_prog_attach(int prog_fd, int type, const void *target)
{
    if (!bpf_progs_initialized)
        return -ENOSYS;

    int idx = prog_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_PROGRAMS || !bpf_progs[idx].in_use)
        return -EINVAL;

    struct bpf_prog *prog = &bpf_progs[idx];

    switch (type) {
    case BPF_PROG_TYPE_KPROBE: {
        /* Attach as a kprobe */
        const char *symbol = (const char *)target;
        if (!symbol) return -EINVAL;

        prog->kprobe.symbol = symbol;
        prog->kprobe.addr = NULL;
        prog->kprobe.is_kretprobe = 0;

        kprintf("[bpf_prog] Attached '%s' as kprobe on '%s'\n",
                prog->name, symbol);
        return 0;
    }

    case BPF_PROG_TYPE_TRACEPOINT: {
        /* Attach to a trace event */
        int event_id = (int)(uintptr_t)target;
        prog->tracepoint.event_id = event_id;

        kprintf("[bpf_prog] Attached '%s' to tracepoint event %d\n",
                prog->name, event_id);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

/*
 * Detach an eBPF program from its hook.
 */
int bpf_prog_detach(int prog_fd)
{
    if (!bpf_progs_initialized)
        return -ENOSYS;

    int idx = prog_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_PROGRAMS || !bpf_progs[idx].in_use)
        return -EINVAL;

    struct bpf_prog *prog = &bpf_progs[idx];

    kprintf("[bpf_prog] Detached '%s' from hook\n", prog->name);

    switch (prog->type) {
    case BPF_PROG_TYPE_KPROBE:
        prog->kprobe.symbol = NULL;
        prog->kprobe.addr = NULL;
        break;
    case BPF_PROG_TYPE_TRACEPOINT:
        prog->tracepoint.event_id = 0;
        break;
    }

    return 0;
}

/*
 * Unload/free an eBPF program.
 */
int bpf_prog_unload(int prog_fd)
{
    if (!bpf_progs_initialized)
        return -ENOSYS;

    int idx = prog_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_PROGRAMS || !bpf_progs[idx].in_use)
        return -EINVAL;

    struct bpf_prog *prog = &bpf_progs[idx];

    if (prog->refcnt > 0)
        prog->refcnt--;

    if (prog->refcnt == 0) {
        /* Free resources */
        if (prog->insns) kfree(prog->insns);
        if (prog->jit_code) kfree(prog->jit_code);

        kprintf("[bpf_prog] Unloaded program '%s'\n", prog->name);
        memset(prog, 0, sizeof(*prog));
    }

    return 0;
}

/*
 * Execute an eBPF program using the interpreter.
 * Returns the value of R0 after execution.
 *
 * @prog_fd:  Program file descriptor.
 * @args:     Up to 5 arguments passed in R1-R5.
 */
uint64_t bpf_prog_run(int prog_fd, uint64_t *args)
{
    if (!bpf_progs_initialized)
        return 0;

    int idx = prog_fd - 1;
    if (idx < 0 || idx >= BPF_MAX_PROGRAMS || !bpf_progs[idx].in_use)
        return 0;

    struct bpf_prog *prog = &bpf_progs[idx];

    /* Set up registers: R1-R5 from args, R6-R9 zero, R10 = stack pointer */
    uint64_t regs[11];
    memset(regs, 0, sizeof(regs));

    for (int i = 0; i < 5 && i < 5; i++) {
        regs[BPF_REG_1 + i] = (args && i < 5) ? args[i] : 0;
    }

    /* Run interpreter */
    return bpf_interp_run(prog->insns, prog->insn_count, regs);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Simple eBPF interpreter
 * ═══════════════════════════════════════════════════════════════════════ */

/* Register numbers */
#define BPF_REG_0   0
#define BPF_REG_1   1
#define BPF_REG_2   2
#define BPF_REG_3   3
#define BPF_REG_4   4
#define BPF_REG_5   5
#define BPF_REG_6   6
#define BPF_REG_7   7
#define BPF_REG_8   8
#define BPF_REG_9   9
#define BPF_REG_10  10

/* Opcode classes */
#define BPF_ALU    0x04
#define BPF_JMP    0x05
#define BPF_ALU64  0x07

/* ALU operations */
#define BPF_ADD    0x00
#define BPF_SUB    0x10
#define BPF_MUL    0x20
#define BPF_DIV    0x30
#define BPF_OR     0x40
#define BPF_AND    0x50
#define BPF_LSH    0x60
#define BPF_RSH    0x70
#define BPF_NEG    0x80
#define BPF_MOD    0x90
#define BPF_XOR    0xa0
#define BPF_MOV    0xb0
#define BPF_ARSH   0xc0

/* JMP operations */
#define BPF_JA     0x00
#define BPF_JEQ    0x10
#define BPF_JGT    0x20
#define BPF_JGE    0x30
#define BPF_JSET   0x40
#define BPF_JNE    0x50
#define BPF_JSGT   0x60
#define BPF_JSGE   0x70
#define BPF_CALL   0x80
#define BPF_EXIT   0x90

/* Helper function call */
static uint64_t bpf_helper_call(int id, uint64_t r1, uint64_t r2,
                                 uint64_t r3, uint64_t r4, uint64_t r5);

static uint64_t bpf_interp_run(const struct bpf_insn *insns, int insn_count,
                                uint64_t *regs)
{
    uint64_t stack[64];  /* 64 * 8 = 512 byte stack */
    uint64_t *r = regs;
    r[BPF_REG_10] = (uint64_t)(uintptr_t)(stack + 64);  /* R10 = top of stack */

    int pc = 0;
    while (pc >= 0 && pc < insn_count) {
        const struct bpf_insn *insn = &insns[pc];
        uint8_t code = insn->code;
        uint8_t cls = code & 0x07;
        uint8_t dst = insn->dst_reg;
        uint8_t src = insn->src_reg;
        int16_t off = insn->off;
        int32_t imm = insn->imm;

        switch (cls) {
        case BPF_ALU:
        case BPF_ALU64: {
            uint64_t src_val = (code & 0x08) ? r[src] : (uint64_t)(int64_t)imm;
            uint64_t dst_val = r[dst];
            uint64_t res = 0;

            switch (code & 0xf0) {
            case BPF_ADD: res = dst_val + src_val; break;
            case BPF_SUB: res = dst_val - src_val; break;
            case BPF_MUL: res = dst_val * src_val; break;
            case BPF_DIV: res = (src_val != 0) ? dst_val / src_val : 0; break;
            case BPF_OR:  res = dst_val | src_val; break;
            case BPF_AND: res = dst_val & src_val; break;
            case BPF_LSH: res = dst_val << (src_val & 63); break;
            case BPF_RSH: res = dst_val >> (src_val & 63); break;
            case BPF_NEG: res = -dst_val; break;
            case BPF_MOD: res = (src_val != 0) ? dst_val % src_val : 0; break;
            case BPF_XOR: res = dst_val ^ src_val; break;
            case BPF_MOV: res = src_val; break;
            case BPF_ARSH: res = (int64_t)dst_val >> (src_val & 63); break;
            default:
                kprintf("[bpf_interp] Unknown ALU op 0x%x at pc=%d\n",
                        code & 0xf0, pc);
                return 0;
            }

            if (cls == BPF_ALU)
                r[dst] = (uint64_t)(uint32_t)res;
            else
                r[dst] = res;
            pc++;
            break;
        }

        case BPF_JMP: {
            uint64_t src_val = (code & 0x08) ? r[src] : (uint64_t)(int64_t)imm;
            uint64_t dst_val = r[dst];
            int jump = 0;

            switch (code & 0xf0) {
            case BPF_JA:
                jump = 1;
                break;
            case BPF_JEQ:  jump = (dst_val == src_val); break;
            case BPF_JGT:  jump = (dst_val > src_val); break;
            case BPF_JGE:  jump = (dst_val >= src_val); break;
            case BPF_JSET: jump = (dst_val & src_val) != 0; break;
            case BPF_JNE:  jump = (dst_val != src_val); break;
            case BPF_JSGT: jump = (int64_t)dst_val > (int64_t)src_val; break;
            case BPF_JSGE: jump = (int64_t)dst_val >= (int64_t)src_val; break;
            case BPF_CALL:
                r[BPF_REG_0] = bpf_helper_call((int)imm,
                                                r[BPF_REG_1], r[BPF_REG_2],
                                                r[BPF_REG_3], r[BPF_REG_4],
                                                r[BPF_REG_5]);
                pc++;
                continue;
            case BPF_EXIT:
                return r[BPF_REG_0];
            default:
                kprintf("[bpf_interp] Unknown JMP op 0x%x at pc=%d\n",
                        code & 0xf0, pc);
                return 0;
            }

            if (jump)
                pc += off + 1;
            else
                pc++;
            break;
        }

        case BPF_LD: {
            /* Pseudo-load for map addresses: not implemented in interpreter */
            pc++;
            break;
        }

        case BPF_LDX: {
            /* Load from memory: r[dst] = *(type *)(r[src] + off) */
            uint8_t *addr = (uint8_t *)r[src] + off;
            uint64_t val = 0;

            switch (code & 0x18) {
            case BPF_W:
                val = *(uint32_t *)addr;
                break;
            case BPF_H:
                val = *(uint16_t *)addr;
                break;
            case BPF_B:
                val = *(uint8_t *)addr;
                break;
            case BPF_DW:
                val = *(uint64_t *)addr;
                break;
            }
            r[dst] = val;
            pc++;
            break;
        }

        case BPF_ST: {
            /* Store immediate: *(type *)(r[dst] + off) = imm */
            uint8_t *addr = (uint8_t *)r[dst] + off;
            switch (code & 0x18) {
            case BPF_W:  *(uint32_t *)addr = (uint32_t)imm; break;
            case BPF_H:  *(uint16_t *)addr = (uint16_t)imm; break;
            case BPF_B:  *(uint8_t *)addr = (uint8_t)imm; break;
            case BPF_DW: *(uint64_t *)addr = (uint64_t)(int64_t)imm; break;
            }
            pc++;
            break;
        }

        case BPF_STX: {
            /* Store register: *(type *)(r[dst] + off) = r[src] */
            uint8_t *addr = (uint8_t *)r[dst] + off;
            switch (code & 0x18) {
            case BPF_W:  *(uint32_t *)addr = (uint32_t)r[src]; break;
            case BPF_H:  *(uint16_t *)addr = (uint16_t)r[src]; break;
            case BPF_B:  *(uint8_t *)addr = (uint8_t)r[src]; break;
            case BPF_DW: *(uint64_t *)addr = r[src]; break;
            }
            pc++;
            break;
        }

        default:
            kprintf("[bpf_interp] Unknown instruction class 0x%x at pc=%d\n",
                    cls, pc);
            return 0;
        }
    }

    return r[BPF_REG_0];
}

static uint64_t bpf_helper_call(int id, uint64_t r1, uint64_t r2,
                                 uint64_t r3, uint64_t r4, uint64_t r5)
{
    /* Forward to helpers registered in bpf_helpers module */
    extern uint64_t bpf_helper_call_ext(int id, uint64_t r1, uint64_t r2,
                                         uint64_t r3, uint64_t r4, uint64_t r5);
    return bpf_helper_call_ext(id, r1, r2, r3, r4, r5);
}
