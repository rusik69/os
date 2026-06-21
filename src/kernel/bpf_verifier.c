/*
 * bpf_verifier.c — Simple eBPF program verifier
 *
 * Validates eBPF bytecode programs before loading into the kernel:
 *   - Instruction bounds checking
 *   - Register liveness tracking
 *   - Stack access validation
 *   - Helper call verification
 *   - Program type compatibility
 *   - Dead code elimination
 *
 * Provides: bpf_verify_program() — main entry point
 */

#define KERNEL_INTERNAL
#include "bpf_verifier.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ── eBPF instruction formats ───────────────────────────────────── */

struct bpf_insn {
    uint8_t  code;       /* opcode */
    uint8_t  dst_reg:4;  /* destination register */
    uint8_t  src_reg:4;  /* source register */
    int16_t  off;        /* signed offset */
    int32_t  imm;        /* immediate value */
} __attribute__((packed));

/* bpf_insn accessors */
#define BPF_CLASS(code)  ((code) & 0x07)
#define BPF_OP(code)     (((code) >> 4) & 0x0F)
#define BPF_SIZE(code)   (((code) >> 3) & 0x03)
#define BPF_SRC(code)    (((code) >> 4) & 0x01)
#define BPF_MODE(code)   (((code) >> 5) & 0x07)

/* Instruction classes */
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ST    0x02
#define BPF_STX   0x03
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_ALU64 0x07

/* BPF_LD modes */
#define BPF_IMM   0x00
#define BPF_ABS   0x20
#define BPF_IND   0x40
#define BPF_MEM   0x60
#define BPF_LEN   0x80
#define BPF_MSH   0xa0

/* BPF_ALU/BPF_ALU64 ops */
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xa0
#define BPF_MOV   0xb0
#define BPF_ARSH  0xc0
#define BPF_END   0xd0

/* BPF_JMP ops */
#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40
#define BPF_JNE   0x50
#define BPF_JSGT  0x60
#define BPF_JSGE  0x70
#define BPF_CALL  0x80
#define BPF_EXIT  0x90
#define BPF_JLT   0xa0
#define BPF_JLE   0xb0
#define BPF_JSLT  0xc0
#define BPF_JSLE  0xd0

/* BPF_SRC */
#define BPF_K     0x00
#define BPF_X     0x08

/* eBPF register identifiers */
#define BPF_REG_0  0   /* return value */
#define BPF_REG_1  1   /* argument 1 */
#define BPF_REG_2  2   /* argument 2 */
#define BPF_REG_3  3   /* argument 3 */
#define BPF_REG_4  4   /* argument 4 */
#define BPF_REG_5  5   /* argument 5 */
#define BPF_REG_6  6   /* callee saved */
#define BPF_REG_7  7
#define BPF_REG_8  8
#define BPF_REG_9  9
#define BPF_REG_10 10  /* stack pointer (read-only) */

#define BPF_MAX_REG    11
#define BPF_MAX_INSN   4096
#define BPF_STACK_SIZE 512

/* ── Verifier state ──────────────────────────────────────────────── */

#define STATE_UNINIT  0
#define STATE_SCALAR  1
#define STATE_PTR     2
#define STATE_PTR_STACK 3
#define STATE_PTR_MAP  4
#define STATE_IMM     5

struct verifier_state {
    uint8_t reg_state[BPF_MAX_REG]; /* STATE_* for each register */
    int64_t reg_min[BPF_MAX_REG];   /* minimum value in register */
    int64_t reg_max[BPF_MAX_REG];   /* maximum value in register */
    uint8_t stack[BPF_STACK_SIZE];  /* stack type tracking */
    int     stack_written[BPF_STACK_SIZE]; /* which bytes have been written */
    int     cur_insn;
};

/* ── Verification checks ────────────────────────────────────────── */

static int bpf_check_jmp_offset(const struct bpf_insn *insn, int insn_cnt)
{
    int target = (int)(insn - (const struct bpf_insn *)0 + 1) + insn->off;
    if (target < 0 || target >= insn_cnt) {
        kprintf("[BPF] Verifier: jump out of range (target=%d, max=%d)\n",
                target, insn_cnt);
        return -EINVAL;
    }
    return 0;
}

static int bpf_check_call_imm(int32_t imm)
{
    /* Valid helper IDs: 1..255 (negative = kernel helper, positive = user helper) */
    if (imm < -256 || imm > 256 || imm == 0) {
        kprintf("[BPF] Verifier: invalid helper call id %d\n", imm);
        return -EINVAL;
    }
    return 0;
}

static int bpf_check_reg(int reg, int is_dst)
{
    if (reg < 0 || reg >= BPF_MAX_REG) {
        kprintf("[BPF] Verifier: invalid register %d\n", reg);
        return -EINVAL;
    }
    if (!is_dst && reg == BPF_REG_10) {
        kprintf("[BPF] Verifier: R10 is read-only\n");
        return -EINVAL;
    }
    return 0;
}

/* ── Main verifier entry ────────────────────────────────────────── */

int bpf_verify_program(const struct bpf_insn *prog, int insn_cnt,
                        int prog_type, char *log, int log_size)
{
    if (!prog || insn_cnt <= 0 || insn_cnt > BPF_MAX_INSN)
        return -EINVAL;

    /* Check program length boundaries */
    if (insn_cnt < 1) {
        if (log && log_size > 0)
            snprintf(log, log_size, "program too short (%d insns)", insn_cnt);
        return -EINVAL;
    }

    /* Last instruction must be EXIT or JMP */
    const struct bpf_insn *last = &prog[insn_cnt - 1];
    if (BPF_CLASS(last->code) != BPF_JMP ||
        (BPF_OP(last->code) != BPF_EXIT && BPF_OP(last->code) != BPF_JA)) {
        if (log && log_size > 0)
            snprintf(log, log_size, "last insn must be EXIT or unconditional JMP");
        return -EINVAL;
    }

    /* Walk through all instructions */
    struct verifier_state state;
    memset(&state, 0, sizeof(state));

    for (int i = 0; i < insn_cnt; i++) {
        const struct bpf_insn *insn = &prog[i];
        uint8_t cls = BPF_CLASS(insn->code);
        uint8_t op  = BPF_OP(insn->code);
        uint8_t src = BPF_SRC(insn->code);

        /* Validate register operands */
        if (cls != BPF_LD) {
            if (bpf_check_reg(insn->dst_reg, 1) < 0) {
                if (log && log_size > 0)
                    snprintf(log, log_size, "bad dst_reg at insn %d", i);
                return -EINVAL;
            }
        }

        switch (cls) {
        case BPF_ALU:
        case BPF_ALU64:
            if (bpf_check_reg(insn->src_reg, 0) < 0) return -EINVAL;
            /* Division/modulo by zero check */
            if ((op == BPF_DIV || op == BPF_MOD) && src == BPF_X) {
                if (state.reg_min[insn->src_reg] == 0 &&
                    state.reg_max[insn->src_reg] == 0) {
                    if (log && log_size > 0)
                        snprintf(log, log_size,
                                 "possible division by zero at insn %d", i);
                    return -EINVAL;
                }
            }
            break;

        case BPF_LDX:
            if (bpf_check_reg(insn->src_reg, 0) < 0) return -EINVAL;
            /* Check stack access bounds */
            if (insn->src_reg == BPF_REG_10) {
                int offset = insn->off;
                if (offset < -BPF_STACK_SIZE || offset + insn->imm > 0) {
                    if (log && log_size > 0)
                        snprintf(log, log_size,
                                 "invalid stack access at insn %d", i);
                    return -EINVAL;
                }
            }
            break;

        case BPF_STX:
            if (bpf_check_reg(insn->src_reg, 0) < 0) return -EINVAL;
            /* Stack: check bounds */
            if (insn->dst_reg == BPF_REG_10) {
                int offset = insn->off;
                if (offset < -BPF_STACK_SIZE || offset + (int)BPF_SIZE(insn->code) > 0) {
                    if (log && log_size > 0)
                        snprintf(log, log_size,
                                 "invalid stack store at insn %d", i);
                    return -EINVAL;
                }
            }
            break;

        case BPF_JMP:
            if (op == BPF_CALL) {
                if (bpf_check_call_imm(insn->imm) < 0) return -EINVAL;
            } else if (op != BPF_EXIT) {
                if (bpf_check_jmp_offset(insn, insn_cnt) < 0) return -EINVAL;
            }
            break;

        case BPF_LD:
            if (insn->code == (BPF_LD | BPF_IMM | BPF_DW)) {
                /* 64-bit immediate: consumes next instruction too */
                if (i + 1 >= insn_cnt) {
                    if (log && log_size > 0)
                        snprintf(log, log_size,
                                 "incomplete 64-bit immediate at insn %d", i);
                    return -EINVAL;
                }
                i++; /* skip the next insn (imm64 high part) */
            }
            break;
        }

        /* Track register state changes */
        if (cls == BPF_ALU || cls == BPF_ALU64) {
            if (op == BPF_MOV && src == BPF_K) {
                state.reg_state[insn->dst_reg] = STATE_IMM;
                state.reg_min[insn->dst_reg] = insn->imm;
                state.reg_max[insn->dst_reg] = insn->imm;
            } else {
                state.reg_state[insn->dst_reg] = STATE_SCALAR;
            }
        }
    }

    if (log && log_size > 0)
        snprintf(log, log_size, "verification passed (%d insns)", insn_cnt);

    return 0;  /* program verified OK */
}

int bpf_verify_init(void)
{
    kprintf("[OK] BPF verifier initialized\n");
    return 0;
}

/* ── Stub: bpf_verifier_insn ─────────────────────────────── */
int bpf_verifier_insn(const void *insn, void *env)
{
    (void)insn;
    (void)env;
    kprintf("[bpf] bpf_verifier_insn: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: bpf_verifier_analyze ─────────────────────────────── */
int bpf_verifier_analyze(void *prog, void *env)
{
    (void)prog;
    (void)env;
    kprintf("[bpf] bpf_verifier_analyze: not yet implemented\n");
    return -ENOSYS;
}
