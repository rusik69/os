/*
 * bpf_verifier.c — eBPF verifier: type-checking and safety validation
 *
 * Implements a basic eBPF bytecode verifier that checks:
 *   - Instruction boundaries (no out-of-bounds jumps)
 *   - Register usage (R0-R10, with R10 as read-only frame pointer)
 *   - Stack access bounds
 *   - Basic type checking for ALU and memory operations
 *
 * This is a simplified verifier suitable for a kernel eBPF implementation.
 * It does NOT implement all Linux verifier features (no path analysis,
 * no value tracking, etc.) but provides enough safety for basic
 * tracing and monitoring programs.
 *
 * Item 132 — eBPF verifier type-checking
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* ── eBPF instruction format ────────────────────────────────────────── */

struct bpf_insn {
    uint8_t  code;       /* opcode */
    uint8_t  dst_reg:4;  /* destination register */
    uint8_t  src_reg:4;  /* source register */
    int16_t  off;        /* signed offset */
    int32_t  imm;        /* signed immediate constant */
} __attribute__((packed));

/* ── eBPF opcode classes ────────────────────────────────────────────── */

#define BPF_LD     0x00
#define BPF_LDX    0x01
#define BPF_ST     0x02
#define BPF_STX    0x03
#define BPF_ALU    0x04
#define BPF_JMP    0x05
#define BPF_ALU64  0x07

/* BPF_LD / BPF_LDX / BPF_ST / BPF_STX size modifiers */
#define BPF_W      0x00  /* 32-bit */
#define BPF_H      0x08  /* 16-bit */
#define BPF_B      0x10  /* 8-bit */
#define BPF_DW     0x18  /* 64-bit */

/* BPF_ALU / BPF_ALU64 operations */
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
#define BPF_END    0xd0

/* BPF_JMP operations */
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
#define BPF_JLT    0xa0
#define BPF_JLE    0xb0
#define BPF_JSLT   0xc0
#define BPF_JSLE   0xd0

/* ── Register numbers ───────────────────────────────────────────────── */

#define BPF_REG_0   0  /* Return value */
#define BPF_REG_1   1  /* Argument 1 */
#define BPF_REG_2   2  /* Argument 2 */
#define BPF_REG_3   3  /* Argument 3 */
#define BPF_REG_4   4  /* Argument 4 */
#define BPF_REG_5   5  /* Argument 5 */
#define BPF_REG_6   6  /* Callee-saved */
#define BPF_REG_7   7  /* Callee-saved */
#define BPF_REG_8   8  /* Callee-saved */
#define BPF_REG_9   9  /* Callee-saved */
#define BPF_REG_10  10 /* Read-only frame pointer */

#define BPF_MAX_REG 11

/* ── Stack size ─────────────────────────────────────────────────────── */

#define BPF_STACK_SIZE 512

/* ── Helper function IDs ────────────────────────────────────────────── */

#define BPF_FUNC_map_lookup_elem  1
#define BPF_FUNC_map_update_elem  2
#define BPF_FUNC_map_delete_elem  3
#define BPF_FUNC_ktime_get_ns     4
#define BPF_FUNC_trace_printk     5
#define BPF_FUNC_get_smp_processor_id 6
#define BPF_FUNC_get_current_pid_tgid 7

#define BPF_MAX_HELPER_FN 64

/* ── Verifier state ─────────────────────────────────────────────────── */

struct bpf_reg_state {
    int type;     /* 0=uninit, 1=scalar, 2=ptr_to_stack, 3=ptr_to_map, 4=ptr_to_packet */
    int size;     /* For stack pointers: allocated size */
    int value;    /* For scalars: known value (or -1 unknown) */
};

struct bpf_verifier_state {
    struct bpf_reg_state regs[BPF_MAX_REG];
    int stack_used[BPF_STACK_SIZE];  /* Track which bytes are initialized */
    int stack_size;
    int cur_insn_idx;
    int num_insns;
    int error;
    const char *error_msg;
};

/* ── Internal helper ───────────────────────────────────────────────── */

static void verifier_set_error(struct bpf_verifier_state *state, const char *msg)
{
    if (!state->error) {
        state->error = 1;
        state->error_msg = msg;
    }
}

/*
 * Check the validity of a single eBPF instruction.
 * Updates verifier state (register types, stack tracking).
 */
static int verifier_check_insn(struct bpf_verifier_state *state,
                                const struct bpf_insn *insn)
{
    uint8_t cls = insn->code & 0x07;
    uint8_t src = insn->src_reg;
    uint8_t dst = insn->dst_reg;
    int16_t off = insn->off;
    int32_t imm = insn->imm;

    /* Check register bounds */
    if (dst >= BPF_MAX_REG || src >= BPF_MAX_REG) {
        verifier_set_error(state, "invalid register number");
        return -EINVAL;
    }

    /* R10 is read-only frame pointer */
    if (dst == BPF_REG_10) {
        verifier_set_error(state, "R10 is read-only");
        return -EINVAL;
    }

    switch (cls) {
    case BPF_ALU:
    case BPF_ALU64:
        /* ALU operations: check destination register */
        if (insn->code == BPF_ALU64 + BPF_NEG || insn->code == BPF_ALU + BPF_NEG) {
            /* Unary negate: no source register needed */
        } else if (insn->code & 0x08) {
            /* BPF_X: src is register */
            if (state->regs[src].type == 0) {
                verifier_set_error(state, "uninitialized source register");
                return -EINVAL;
            }
        }
        /* Mark destination as scalar */
        state->regs[dst].type = 1;  /* scalar */
        state->regs[dst].value = -1;  /* unknown */
        break;

    case BPF_LDX:
        /* Load from memory: dst = *(size *)(src + off) */
        if (state->regs[src].type < 2) {
            verifier_set_error(state, "load from non-pointer");
            return -EINVAL;
        }
        /* Check stack bounds if reading from stack */
        if (state->regs[src].type == 2) { /* ptr_to_stack */
            int size;
            switch (insn->code & 0x18) {
                case BPF_W:  size = 4; break;
                case BPF_H:  size = 2; break;
                case BPF_B:  size = 1; break;
                case BPF_DW: size = 8; break;
                default:     size = 4;
            }
            if (off < 0 || off + size > BPF_STACK_SIZE) {
                verifier_set_error(state, "stack out of bounds");
                return -EINVAL;
            }
            /* Mark stack bytes as initialized */
            for (int i = off; i < off + size && i < BPF_STACK_SIZE; i++)
                state->stack_used[i] = 1;
        }
        state->regs[dst].type = 1;  /* scalar */
        break;

    case BPF_ST:
        /* Store immediate: *(size *)(dst + off) = imm */
        if (state->regs[dst].type < 2) {
            verifier_set_error(state, "store to non-pointer");
            return -EINVAL;
        }
        if (state->regs[dst].type == 2) { /* ptr_to_stack */
            if (off < 0 || off + 4 > BPF_STACK_SIZE) {
                verifier_set_error(state, "stack out of bounds");
                return -EINVAL;
            }
        }
        break;

    case BPF_STX:
        /* Store register: *(size *)(dst + off) = src */
        if (state->regs[dst].type < 2) {
            verifier_set_error(state, "store to non-pointer");
            return -EINVAL;
        }
        if (state->regs[src].type == 0) {
            verifier_set_error(state, "store uninitialized value");
            return -EINVAL;
        }
        if (state->regs[dst].type == 2) { /* ptr_to_stack */
            if (off < 0 || off + 4 > BPF_STACK_SIZE) {
                verifier_set_error(state, "stack out of bounds");
                return -EINVAL;
            }
        }
        break;

    case BPF_LD:
        /* Special load instructions (e.g., pseudo map fd) */
        /* Pseudo-instructions for map loading: code = 0x18, imm = 1 */
        break;

    case BPF_JMP:
        switch (insn->code & 0xf0) {
        case BPF_JA:
            /* Unconditional jump */
            if (state->cur_insn_idx + off + 1 < 0 ||
                state->cur_insn_idx + off + 1 >= state->num_insns) {
                verifier_set_error(state, "jump out of bounds");
                return -EINVAL;
            }
            break;

        case BPF_CALL:
            /* Function call: validate helper function ID */
            if (imm <= 0 || imm > BPF_MAX_HELPER_FN) {
                verifier_set_error(state, "invalid helper function");
                return -EINVAL;
            }
            /* R1-R5 may be clobbered */
            state->regs[1].type = state->regs[2].type = state->regs[3].type =
            state->regs[4].type = state->regs[5].type = 0;
            /* R0 gets return value (scalar) */
            state->regs[0].type = 1;
            break;

        case BPF_EXIT:
            /* Exit: R0 must contain return value */
            if (state->regs[0].type == 0) {
                verifier_set_error(state, "R0 not set before exit");
                return -EINVAL;
            }
            break;

        default:
            /* Conditional jumps */
            if (state->regs[src].type == 0 && (insn->code & 0x08)) {
                verifier_set_error(state, "uninitialized register in jump");
                return -EINVAL;
            }
            /* Check both targets */
            if (state->cur_insn_idx + off + 1 < 0 ||
                state->cur_insn_idx + off + 1 >= state->num_insns) {
                verifier_set_error(state, "jump out of bounds");
                return -EINVAL;
            }
            if (state->cur_insn_idx + 1 >= state->num_insns) {
                verifier_set_error(state, "fallthrough out of bounds");
                return -EINVAL;
            }
            break;
        }
        break;

    default:
        verifier_set_error(state, "unknown instruction class");
        return -EINVAL;
    }

    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

/*
 * Verify an eBPF program.
 *
 * @prog:      Pointer to eBPF bytecode.
 * @prog_len:  Number of instructions.
 * @log_buf:   Optional buffer for verifier log messages.
 * @log_size:  Size of log buffer.
 *
 * Returns 0 on success, negative on verification failure.
 */
int bpf_verifier_verify(const struct bpf_insn *prog, int prog_len,
                         char *log_buf, int log_size)
{
    struct bpf_verifier_state state;
    int ret = 0;

    memset(&state, 0, sizeof(state));
    state.num_insns = prog_len;

    if (!prog || prog_len <= 0 || prog_len > 4096) {
        if (log_buf && log_size > 0)
            snprintf(log_buf, log_size, "Invalid program length: %d\n", prog_len);
        kprintf("[bpf_verifier] Invalid program: len=%d\n", prog_len);
        return -EINVAL;
    }

    /* Initialize R1-R5 as readable (arguments from caller) */
    for (int i = BPF_REG_1; i <= BPF_REG_5; i++) {
        state.regs[i].type = 1;  /* scalar (could be pointer in real verifier) */
    }

    /* Walk through all instructions */
    for (state.cur_insn_idx = 0; state.cur_insn_idx < prog_len; ) {
        const struct bpf_insn *insn = &prog[state.cur_insn_idx];

        ret = verifier_check_insn(&state, insn);
        if (ret < 0)
            break;

        /* Handle jump offset for next instruction */
        if ((insn->code & 0x07) == BPF_JMP) {
            if ((insn->code & 0xf0) == BPF_JA) {
                state.cur_insn_idx += insn->off + 1;
                continue;
            }
            if ((insn->code & 0xf0) == BPF_EXIT) {
                state.cur_insn_idx++;
                continue;
            }
            /* Conditional jump: follow both paths is complex; for simplicity
             * we just continue linearly (checks both targets above). */
        }

        state.cur_insn_idx++;
    }

    if (ret < 0) {
        if (log_buf && log_size > 0) {
            snprintf(log_buf, log_size,
                     "Verifier error at insn %d: %s\n",
                     state.cur_insn_idx,
                     state.error_msg ? state.error_msg : "unknown");
        }
        kprintf("[bpf_verifier] FAIL at insn %d: %s\n",
                state.cur_insn_idx,
                state.error_msg ? state.error_msg : "unknown");
        return ret;
    }

    kprintf("[bpf_verifier] Program verified: %d instructions\n", prog_len);
    return 0;
}

/*
 * Print verifier state for debugging.
 */
void bpf_verifier_dump_state(const struct bpf_insn *prog, int prog_len)
{
    kprintf("eBPF Program Dump (%d insns):\n", prog_len);
    for (int i = 0; i < prog_len; i++) {
        kprintf("  [%d] code=0x%02x dst=R%d src=R%d off=%d imm=%d\n",
                i,
                prog[i].code,
                prog[i].dst_reg,
                prog[i].src_reg,
                prog[i].off,
                prog[i].imm);
    }
}

void bpf_verifier_init(void)
{
    kprintf("[bpf_verifier] eBPF verifier initialized\n");
}
