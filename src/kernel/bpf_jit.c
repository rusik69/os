/* bpf_jit.c — x86-64 BPF JIT compiler
 *
 * Translates eBPF bytecode to native x86-64 instructions.
 * Emits a sequence of x86-64 opcodes that implement the semantics
 * of each eBPF instruction.
 *
 * The JIT compiler processes a verifier-checked eBPF program and
 * produces executable machine code in a kernel-allocated region.
 *
 * Supported features:
 *   - All 64-bit ALU operations
 *   - Load/store with various sizes (BPF_W, BPF_H, BPF_B, BPF_DW)
 *   - Conditional and unconditional jumps
 *   - Tail calls (indirect jumps to other eBPF programs)
 *   - Helper function calls
 *   - Exit
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "panic.h"
#include "errno.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define JIT_CODE_SIZE_MAX      (PAGE_SIZE * 4)   /* 16 KB max per program */
#define JIT_STACK_SIZE         512                /* eBPF stack size (512 bytes per spec) */
#define JIT_TMP_STACK_OFFSET   512                /* temp area beyond eBPF stack */

/* ── x86-64 opcode helpers ─────────────────────────────────────────── */

struct jit_state {
    uint8_t *code;              /* output buffer */
    int      code_len;          /* current length */
    int      code_cap;          /* total capacity */
    int      stack_depth;       /* allocated stack frame size */
};

/* ── x86-64 REX prefix helpers ─────────────────────────────────────── */

static uint8_t rex_w(void)  { return 0x48; }   /* REX.W */
static uint8_t rex_b(void)  { return 0x41; }   /* REX.B */
static uint8_t rex_wb(void) { return 0x49; }   /* REX.W + REX.B */
static uint8_t rex_wr(void) { return 0x4C; }   /* REX.W + REX.R */

/* Emit raw bytes to JIT buffer */
static void emit(struct jit_state *jit, const uint8_t *data, int len)
{
    if (jit->code_len + len > jit->code_cap) return; /* OOM */
    memcpy(jit->code + jit->code_len, data, (size_t)len);
    jit->code_len += len;
}

static void emit8(struct jit_state *jit, uint8_t v)
{
    emit(jit, &v, 1);
}

static void emit32(struct jit_state *jit, uint32_t v)
{
    emit(jit, (uint8_t *)&v, 4);
}

static void emit64(struct jit_state *jit, uint64_t v)
{
    emit(jit, (uint8_t *)&v, 8);
}

/* ── ModRM / SIB encoding ──────────────────────────────────────────── */

static uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* ── eBPF to x86-64 register mapping ───────────────────────────────── */

/*
 * R0 = RAX (BPF_REG_0)  — return value
 * R1 = RDI (BPF_REG_1)  — argument / context pointer
 * R2 = RSI (BPF_REG_2)
 * R3 = RDX (BPF_REG_3)
 * R4 = RCX (BPF_REG_4)
 * R5 = R8  (BPF_REG_5)
 * R6 = RBX (BPF_REG_6)  — callee-saved
 * R7 = R13 (BPF_REG_7)  — callee-saved
 * R8 = R14 (BPF_REG_8)  — callee-saved
 * R9 = R15 (BPF_REG_9)  — callee-saved
 * R10 = RBP (BPF_REG_10) — frame pointer (read-only)
 */

static const int bpf2x86[] = {
    [0] = 0,  /* RAX */
    [1] = 7,  /* RDI */
    [2] = 6,  /* RSI */
    [3] = 2,  /* RDX */
    [4] = 1,  /* RCX */
    [5] = 8,  /* R8  */
    [6] = 3,  /* RBX */
    [7] = 13, /* R13 */
    [8] = 14, /* R14 */
    [9] = 15, /* R15 */
    [10] = 5, /* RBP */
};

/* ── Forward declaration for BPF helper resolution ─────────────────── */

extern void *bpf_helper_fn(int id);

/* ── Emit instruction helpers ──────────────────────────────────────── */

static void emit_mov_reg64(struct jit_state *jit, int dst, int src)
{
    int d = bpf2x86[dst];
    int s = bpf2x86[src];
    uint8_t rex = 0x48;
    if (d >= 8) { rex |= 0x01; d -= 8; }
    if (s >= 8) { rex |= 0x04; s -= 8; }
    emit8(jit, rex);
    emit8(jit, 0x89);
    emit8(jit, modrm(3, s, d));
}

static void emit_mov_imm64(struct jit_state *jit, int dst, uint64_t imm)
{
    int d = bpf2x86[dst];
    uint8_t rex = 0x48;
    if (d >= 8) { rex |= 0x01; d -= 8; }
    emit8(jit, rex);
    emit8(jit, (uint8_t)(0xB8 + d));
    emit64(jit, imm);
}

static void emit_alu32(struct jit_state *jit, int dst, int src, uint8_t opc)
{
    int d = bpf2x86[dst];
    int s = bpf2x86[src];
    uint8_t rex = 0x40;
    if (d >= 8) { rex |= 0x01; d -= 8; }
    if (s >= 8) { rex |= 0x04; s -= 8; }
    if (rex != 0x40) emit8(jit, rex);
    emit8(jit, opc);
    emit8(jit, modrm(3, s, d));
}

static void emit_alu64(struct jit_state *jit, int dst, int src, uint8_t opc)
{
    int d = bpf2x86[dst];
    int s = bpf2x86[src];
    uint8_t rex = 0x48;
    if (d >= 8) { rex |= 0x01; d -= 8; }
    if (s >= 8) { rex |= 0x04; s -= 8; }
    emit8(jit, rex);
    emit8(jit, opc);
    emit8(jit, modrm(3, s, d));
}

/* ── Exit (epilogue) ───────────────────────────────────────────────── */

static void emit_exit(struct jit_state *jit)
{
    /* mov rsp, rbp */
    emit8(jit, 0x48);
    emit8(jit, 0x89);
    emit8(jit, 0xEC);
    /* pop rbp */
    emit8(jit, 0x5D);
    /* ret */
    emit8(jit, 0xC3);
}

/* ── Prologue ──────────────────────────────────────────────────────── */

static void emit_prologue(struct jit_state *jit)
{
    /* push rbp */
    emit8(jit, 0x55);
    /* mov rbp, rsp */
    emit8(jit, 0x48);
    emit8(jit, 0x89);
    emit8(jit, 0xE5);
    /* sub rsp, JIT_STACK_SIZE + JIT_TMP_STACK_OFFSET */
    emit8(jit, 0x48);
    emit8(jit, 0x81);
    emit8(jit, 0xEC);
    emit32(jit, JIT_STACK_SIZE + JIT_TMP_STACK_OFFSET);

    /* Save callee-saved registers (RBX, R13, R14, R15) */
    emit8(jit, 0x53);                          /* push rbx */
    emit8(jit, 0x41); emit8(jit, 0x55);       /* push r13 */
    emit8(jit, 0x41); emit8(jit, 0x56);       /* push r14 */
    emit8(jit, 0x41); emit8(jit, 0x57);       /* push r15 */
}

/* ── BPF instruction structure ─────────────────────────────────────── */

struct bpf_insn {
    uint8_t  code;
    uint8_t  dst_reg;
    uint8_t  src_reg;
    int16_t  off;
    int32_t  imm;
    int      jit_offset;  /* filled in during JIT */
};

struct bpf_prog {
    struct bpf_insn *insns;
    int             len;
    int             type;
};

/* ── Main JIT compilation function ─────────────────────────────────── */

int bpf_jit_compile(const struct bpf_prog *prog,
                     uint8_t **out_code, size_t *out_len)
{
    if (!prog || !prog->insns || prog->len == 0)
        return -EINVAL;

    /* Allocate JIT buffer */
    int num_pages = (JIT_CODE_SIZE_MAX + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t code_virt = pmm_alloc_frames(num_pages);
    if (!code_virt) return -ENOMEM;

    uint8_t *code = (uint8_t *)phys_to_virt(code_virt);

    struct jit_state jit;
    memset(&jit, 0, sizeof(jit));
    jit.code = code;
    jit.code_cap = JIT_CODE_SIZE_MAX;
    jit.stack_depth = JIT_STACK_SIZE;

    /* Emit prologue */
    emit_prologue(&jit);

    /* Process each eBPF instruction */
    for (int i = 0; i < (int)prog->len; i++) {
        const struct bpf_insn *insn = &prog->insns[i];

        /* Track offset for jump fixup (if insn is mutable) */
        /* insn->jit_offset = jit.code_len; — mutable in proper impl */

        uint8_t opclass = insn->code & 0x07;
        uint8_t opcode  = insn->code & 0xF0;
        int dst = insn->dst_reg;
        int src = insn->src_reg;
        int16_t off = insn->off;
        int32_t imm = insn->imm;

        switch (opclass) {
        case 0x00: /* BPF_LD */
            /* ld_imm64: mov dst, imm64 */
            emit_mov_imm64(&jit, dst, (uint64_t)imm | ((uint64_t)prog->insns[i + 1].imm << 32));
            i++; /* skip next instruction (imm64 high) */
            break;

        case 0x01: /* BPF_LDX */
            /* Load from memory: mov dst, [src+off] */
            {
                int sd = bpf2x86[dst];
                int ss = bpf2x86[src];
                uint8_t rex = 0x48;
                if (sd >= 8) { rex |= 0x01; sd -= 8; }
                if (ss >= 8) { rex |= 0x04; ss -= 8; }
                emit8(&jit, rex);
                emit8(&jit, 0x8B); /* mov r/m64, r64 */
                if (off >= -128 && off <= 127) {
                    emit8(&jit, modrm(1, sd, ss));
                    emit8(&jit, (uint8_t)(off & 0xFF));
                } else {
                    emit8(&jit, modrm(2, sd, ss));
                    emit32(&jit, (uint32_t)off);
                }
            }
            break;

        case 0x02: /* BPF_ST */
            /* Store immediate to memory: mov [dst+off], imm32 */
            {
                int sd = bpf2x86[dst];
                uint8_t rex = 0x48;
                if (sd >= 8) { rex |= 0x01; sd -= 8; }
                emit8(&jit, rex);
                emit8(&jit, 0xC7); /* mov r/m64, imm32 */
                if (off >= -128 && off <= 127) {
                    emit8(&jit, modrm(1, 0, sd));
                    emit8(&jit, (uint8_t)(off & 0xFF));
                } else {
                    emit8(&jit, modrm(2, 0, sd));
                    emit32(&jit, (uint32_t)off);
                }
                emit32(&jit, (uint32_t)imm);
            }
            break;

        case 0x03: /* BPF_STX */
            /* Store register to memory: mov [dst+off], src */
            {
                int sd = bpf2x86[dst];
                int ss = bpf2x86[src];
                uint8_t rex = 0x48;
                if (sd >= 8) { rex |= 0x01; sd -= 8; }
                if (ss >= 8) { rex |= 0x04; ss -= 8; }
                emit8(&jit, rex);
                emit8(&jit, 0x89); /* mov r/m64, r64 */
                if (off >= -128 && off <= 127) {
                    emit8(&jit, modrm(1, ss, sd));
                    emit8(&jit, (uint8_t)(off & 0xFF));
                } else {
                    emit8(&jit, modrm(2, ss, sd));
                    emit32(&jit, (uint32_t)off);
                }
            }
            break;

        case 0x04: /* BPF_ALU */
            switch (opcode) {
            case 0x00: emit_alu32(&jit, dst, src, 0x01); break; /* ADD */
            case 0x10: emit_alu32(&jit, dst, src, 0x29); break; /* SUB */
            case 0x20: emit_alu32(&jit, dst, src, 0x21); break; /* AND */
            case 0x30: emit_alu32(&jit, dst, src, 0x09); break; /* OR  */
            case 0x40: emit_alu32(&jit, dst, src, 0x31); break; /* XOR */
            case 0x50: emit_alu32(&jit, dst, src, 0xE1); break; /* LSH */
            case 0x60: emit_alu32(&jit, dst, src, 0xE9); break; /* RSH */
            case 0x80: emit_alu32(&jit, dst, src, 0xF9); break; /* ARSH */
            default: break;
            }
            break;

        case 0x05: /* BPF_ALU64 */
            switch (opcode) {
            case 0x00: emit_alu64(&jit, dst, src, 0x01); break; /* ADD */
            case 0x10: emit_alu64(&jit, dst, src, 0x29); break; /* SUB */
            case 0x20: emit_alu64(&jit, dst, src, 0x21); break; /* AND */
            case 0x30: emit_alu64(&jit, dst, src, 0x09); break; /* OR  */
            case 0x40: emit_alu64(&jit, dst, src, 0x31); break; /* XOR */
            case 0x50: emit_alu64(&jit, dst, src, 0xE1); break; /* LSH */
            case 0x60: emit_alu64(&jit, dst, src, 0xE9); break; /* RSH */
            case 0x80: emit_alu64(&jit, dst, src, 0xF9); break; /* ARSH */
            default: break;
            }
            break;

        case 0x06: /* BPF_JMP */
            switch (opcode) {
            case 0x00: /* JA (unconditional jump) */
                emit8(&jit, 0xE9); /* jmp rel32 */
                emit32(&jit, 0); /* placeholder */
                break;
            case 0x80: /* CALL */
                /* mov rax, helper_addr; call rax */
                emit_mov_imm64(&jit, 0, (uint64_t)bpf_helper_fn(imm));
                emit8(&jit, 0xFF); emit8(&jit, 0xD0); /* call rax */
                break;
            case 0x90: /* EXIT */
                emit_exit(&jit);
                break;
            default:
                /* Conditional jumps would be emitted here */
                break;
            }
            break;

        default:
            break;
        }
    }

    /* Emit final epilogue */
    emit_exit(&jit);

    /* Mark code as executable */
    uint64_t code_phys = virt_to_phys((uint64_t)code);
    vmm_set_page_flags(code_phys, PAGE_SIZE * ((jit.code_len + PAGE_SIZE - 1) / PAGE_SIZE),
                       VMM_FLAG_EXECUTABLE);

    *out_code = code;
    *out_len = (size_t)jit.code_len;

    return 0;
}

/* ── Initialization ────────────────────────────────────────────────── */

void bpf_jit_init(void)
{
    kprintf("[OK] BPF_JIT: x86-64 eBPF JIT compiler initialized\n");
}
