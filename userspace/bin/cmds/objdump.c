/*
 * objdump — ELF64 reader + basic x86-64 disassembler.
 *
 * Prints ELF header/sections and disassembles executable (.text) sections.
 * Decodes the common integer instruction set (REX/operand-size/address-size
 * prefixes, ModRM+SIB addressing, RIP-relative operands, jcc/call/ret,
 * group 1/2/3/5, movzx/movsx/setcc/cmov, xadd/cmpxchg, string ops, and a
 * few SSE moves).  Unrecognised opcodes fall back to `.byte 0xNN` so
 * decoding never fails.
 *
 * AT&T syntax, matching GNU objdump's default.
 *
 * Usage: objdump <elf-file>
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

/* ── ELF64 structures (userspace libc has no <elf.h>) ───────────────── */
struct elf64_header {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int e_version;
    unsigned long long e_entry;
    unsigned long long e_phoff;
    unsigned long long e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
} __attribute__((packed));

struct elf64_shdr {
    unsigned int sh_name;
    unsigned int sh_type;
    unsigned long long sh_flags;
    unsigned long long sh_addr;
    unsigned long long sh_offset;
    unsigned long long sh_size;
    unsigned int sh_link;
    unsigned int sh_info;
    unsigned long long sh_addralign;
    unsigned long long sh_entsize;
} __attribute__((packed));

#define SHF_EXECINSTR 0x0004

/* ── Disassembler state ─────────────────────────────────────────────── */

struct dctx {
    const unsigned char *p;
    const unsigned char *end;
    unsigned long va; /* virtual address of p */
    int rex_w, rex_r, rex_x, rex_b;
    int had_rex;
    int opsize; /* 16, 32 or 64 */
};

struct dout {
    char *s;
    int cap;
    int len;
};

static void dpush(struct dout *o, const char *str) {
    while (*str && o->len < o->cap - 1)
        o->s[o->len++] = *str++;
}

static void dhex(struct dout *o, unsigned long long v) {
    char rev[24];
    int pos = 0;
    if (v == 0) {
        rev[pos++] = '0';
    }
    while (v > 0 && pos < 22) {
        int nib = (int)(v & 0xF);
        rev[pos++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
        v >>= 4;
    }
    if (o->len > o->cap - (pos + 3))
        return;
    o->s[o->len++] = '0';
    o->s[o->len++] = 'x';
    for (int i = pos - 1; i >= 0; i--)
        o->s[o->len++] = rev[i];
}

static void ddec(struct dout *o, long long v) {
    char rev[24];
    int pos = 0, neg = 0;
    unsigned long long uv;
    if (v < 0) {
        neg = 1;
        uv = (unsigned long long)(-v);
    } else
        uv = (unsigned long long)v;
    if (uv == 0)
        rev[pos++] = '0';
    while (uv > 0 && pos < 22) {
        rev[pos++] = '0' + (int)(uv % 10);
        uv /= 10;
    }
    if (o->len > o->cap - (pos + 2))
        return;
    if (neg)
        o->s[o->len++] = '-';
    for (int i = pos - 1; i >= 0; i--)
        o->s[o->len++] = rev[i];
}

static int cur_rex8; /* set before each instruction: REX present -> low 8 regs are spl..dil */

static const char *regname(int idx, int size) {
    static const char *const v64[16] = {"%rax", "%rcx", "%rdx", "%rbx", "%rsp", "%rbp",
                                        "%rsi", "%rdi", "%r8",  "%r9",  "%r10", "%r11",
                                        "%r12", "%r13", "%r14", "%r15"};
    static const char *const v32[16] = {"%eax",  "%ecx",  "%edx",  "%ebx", "%esp",  "%ebp",
                                        "%esi",  "%edi",  "%r8d",  "%r9d", "%r10d", "%r11d",
                                        "%r12d", "%r13d", "%r14d", "%r15d"};
    static const char *const v16[16] = {"%ax",   "%cx",   "%dx",   "%bx",  "%sp",   "%bp",
                                        "%si",   "%di",   "%r8w",  "%r9w", "%r10w", "%r11w",
                                        "%r12w", "%r13w", "%r14w", "%r15w"};
    static const char *const r8[8] = {"%al", "%cl", "%dl", "%bl", "%ah", "%ch", "%dh", "%bh"};
    static const char *const r8r[16] = {"%al",   "%cl",   "%dl",   "%bl",  "%spl",  "%bpl",
                                        "%sil",  "%dil",  "%r8b",  "%r9b", "%r10b", "%r11b",
                                        "%r12b", "%r13b", "%r14b", "%r15b"};
    if (idx < 0)
        return "%??";
    if (size == 64)
        return v64[idx & 15];
    if (size == 32)
        return v32[idx & 15];
    if (size == 16)
        return v16[idx & 15];
    if (cur_rex8)
        return r8r[idx & 15];
    return idx < 8 ? r8[idx] : r8r[idx & 15];
}

static const char *xmmname(int idx) {
    static char buf[10];
    static const char *const tab[16] = {"%xmm0",  "%xmm1",  "%xmm2",  "%xmm3", "%xmm4",  "%xmm5",
                                        "%xmm6",  "%xmm7",  "%xmm8",  "%xmm9", "%xmm10", "%xmm11",
                                        "%xmm12", "%xmm13", "%xmm14", "%xmm15"};
    const char *s = tab[idx & 15];
    int i = 0;
    while (s[i]) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = '\0';
    return buf;
}

static int b8(struct dctx *c, unsigned char *v) {
    if (c->p >= c->end)
        return -1;
    *v = *c->p++;
    c->va++;
    return 0;
}
static int i32(struct dctx *c, unsigned long *v) {
    if (c->p + 4 > c->end)
        return -1;
    *v = (unsigned long)c->p[0] | ((unsigned long)c->p[1] << 8) | ((unsigned long)c->p[2] << 16) |
         ((unsigned long)c->p[3] << 24);
    c->p += 4;
    c->va += 4;
    return 0;
}
static int i64(struct dctx *c, unsigned long long *v) {
    if (c->p + 8 > c->end)
        return -1;
    unsigned long long r = 0;
    for (int i = 0; i < 8; i++)
        r |= (unsigned long long)c->p[i] << (8 * i);
    *v = r;
    c->p += 8;
    c->va += 8;
    return 0;
}
static int r8v(struct dctx *c, long *v) {
    unsigned char b;
    if (b8(c, &b) < 0)
        return -1;
    *v = (b & 0x80) ? (long)(b - 0x100) : (long)b;
    return 0;
}
static int r32v(struct dctx *c, long *v) {
    unsigned long u;
    if (i32(c, &u) < 0)
        return -1;
    *v = (u & 0x80000000UL) ? (long)(u | ~0xFFFFFFFFUL) : (long)u;
    return 0;
}

/* Append a memory operand from a ModRM byte (mod != 3). rm is modrm&7. */
static int modrm_mem(struct dctx *c, struct dout *o, int mod, int rm) {
    long disp = 0;
    int have_disp = 0;
    int base = -1, index = -1, scale = 1;

    /* Non-SIB RIP-relative */
    if (mod == 0 && rm == 5) {
        long off;
        if (r32v(c, &off) < 0)
            return -1;
        if (off)
            ddec(o, off);
        dpush(o, "(%rip)");
        return 0;
    }

    if (rm == 4) { /* SIB byte precedes disp */
        unsigned char sib;
        if (c->p >= c->end)
            return -1;
        sib = *c->p++;
        c->va++;
        scale = 1 << (sib >> 6);
        index = ((sib >> 3) & 7) | (c->rex_x ? 8 : 0);
        int ibase = (sib & 7) | (c->rex_b ? 8 : 0);
        if (ibase == 5 && mod == 0) {
            base = -1; /* disp32 absolute */
        } else
            base = ibase;
    } else {
        base = rm | (c->rex_b ? 8 : 0);
    }

    /* displacement */
    if (mod == 1) {
        unsigned char nb;
        if (c->p >= c->end)
            return -1;
        nb = *c->p++;
        c->va++;
        disp = (nb & 0x80) ? (long)(nb - 0x100) : (long)nb;
        have_disp = 1;
    } else if (mod == 2) {
        unsigned long u;
        if (i32(c, &u) < 0)
            return -1;
        disp = (u & 0x80000000UL) ? (long)(u | ~0xFFFFFFFFUL) : (long)u;
        have_disp = 1;
    } else if (base < 0 && rm == 4) { /* [index*scale] + disp32 */
        unsigned long u;
        if (i32(c, &u) < 0)
            return -1;
        disp = (long)u;
        have_disp = 1;
    }

    if (have_disp && disp != 0)
        ddec(o, disp);
    dpush(o, "(");
    int need = 0;
    if (base >= 0) {
        dpush(o, regname(base, 64));
        need = 1;
    }
    if (index >= 0 && index != 4) {
        if (need)
            dpush(o, ",");
        if (scale != 1) {
            ddec(o, scale);
            dpush(o, "*");
        }
        dpush(o, regname(index, 64));
    }
    dpush(o, ")");
    return 0;
}

/*
 * Decode ONE ModRM byte into two operand buffers: oreg (the reg field)
 * and orm (the r/m field, register or memory). at&t_order selects which
 * operand prints first (0 = reg first, 1 = rm first).
 */
struct operands {
    char a[64], b[64];
    int atmt_a_is_reg;
};

static int decode_mr_ex(struct dctx *c, struct operands *op, int reg_size, int rm_size, int xmm,
                        unsigned char mr);

static int decode_mr(struct dctx *c, struct operands *op, int reg_size, int rm_size, int xmm) {
    unsigned char mr;
    if (b8(c, &mr) < 0)
        return -1;
    return decode_mr_ex(c, op, reg_size, rm_size, xmm, mr);
}

/* Decode a ModRM byte that was already read (mr). reg field registered in
 * op->a, r/m field in op->b. */
static int decode_mr_ex(struct dctx *c, struct operands *op, int reg_size, int rm_size, int xmm,
                        unsigned char mr) {
    int mod = mr >> 6, rm = mr & 7;
    int reg = ((mr >> 3) & 7) | (c->rex_r ? 8 : 0);

    struct dout da;
    da.s = op->a;
    da.cap = 64;
    da.len = 0;
    struct dout db;
    db.s = op->b;
    db.cap = 64;
    db.len = 0;

    dpush(&da, xmm ? xmmname(reg) : regname(reg, reg_size));

    if (mod == 3) {
        dpush(&db,
              xmm ? xmmname(rm | (c->rex_b ? 8 : 0)) : regname(rm | (c->rex_b ? 8 : 0), rm_size));
    } else if (modrm_mem(c, &db, mod, rm) < 0) {
        return -1;
    }
    da.s[da.len] = '\0';
    db.s[db.len] = '\0';
    return 0;
}

/* print one or both operands; kind: 0=reg,rm  1=rm,reg */
static void emit_op(struct dout *o, struct operands *op, int kind) {
    if (kind == 0) {
        dpush(o, op->a);
        dpush(o, ", ");
        dpush(o, op->b);
    } else {
        dpush(o, op->b);
        dpush(o, ", ");
        dpush(o, op->a);
    }
}

/* ── group tables ───────────────────────────────────────────────────── */
static const char *const grp1[8] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
static const char *const grp2[8] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};
static const char *const grp3[8] = {"test", "test", "not", "neg", "mul", "imul", "div", "idiv"};
static const char *const cc16[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                     "s", "ns", "p", "np", "l", "ge", "le", "g"};

/* decode one instruction into buf (AT&T). returns bytes consumed, 0 on err */
static int disasm_insn(const unsigned char *p, unsigned long len, unsigned long va, char *buf,
                       int buflen) {
    struct dctx c;
    struct dout o;
    unsigned char b, op;
    struct operands opd;
    long off;
    unsigned long u;
    unsigned long long s64;

    o.s = buf;
    o.cap = buflen;
    o.len = 0;
    c.p = p;
    c.end = p + len;
    c.va = va;
    c.rex_w = c.rex_r = c.rex_x = c.rex_b = c.had_rex = 0;
    c.opsize = 32;
    cur_rex8 = 0;

    for (;;) {
        if (c.p >= c.end)
            return 0;
        b = *c.p;
        if (b == 0x66) {
            c.p++;
            c.va++;
            c.opsize = 16;
            continue;
        }
        if (b == 0x67) {
            c.p++;
            c.va++;
            continue;
        }
        if (b >= 0x40 && b <= 0x4F) {
            c.p++;
            c.va++;
            c.rex_w = (b & 8) != 0;
            c.rex_r = (b & 4) != 0;
            c.rex_x = (b & 2) != 0;
            c.rex_b = (b & 1) != 0;
            c.had_rex = 1;
            cur_rex8 = 1;
            if (c.rex_w)
                c.opsize = 64;
            continue;
        }
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
            b == 0xF0 || b == 0xF2 || b == 0xF3) {
            c.p++;
            c.va++;
            continue;
        }
        break;
    }
    if (c.p >= c.end)
        return 0;
    op = *c.p;
    c.p++;
    c.va++;

    /* convenience: decode modrm (same size reg+rm), kind 0 = reg,rm */
#define MR2(sz, kind) \
    do { \
        if (decode_mr(&c, &opd, (sz), (sz), 0) < 0) \
            return 0; \
        emit_op(&o, &opd, (kind)); \
    } while (0)

    if (op == 0x0F) {
        unsigned char op2;
        if (b8(&c, &op2) < 0)
            return 0;
        switch (op2) {
        case 0x05:
            dpush(&o, "syscall");
            goto done;
        case 0x06:
            dpush(&o, "clts");
            goto done;
        case 0x07:
            dpush(&o, "sysret");
            goto done;
        case 0x08:
            dpush(&o, "invd");
            goto done;
        case 0x09:
            dpush(&o, "wbinvd");
            goto done;
        case 0x0B:
            dpush(&o, "ud2");
            goto done;
        case 0x31:
            dpush(&o, "rdtsc");
            goto done;
        case 0x32:
            dpush(&o, "rdmsr");
            goto done;
        case 0xA2:
            dpush(&o, "cpuid");
            goto done;
        case 0x1F:
            dpush(&o, "nopl ");
            if (decode_mr(&c, &opd, 32, 32, 0) < 0)
                return 0;
            dpush(&o, opd.b);
            goto done;
        case 0x1E: { /* endbr64 / endbr32 (CET) */
            unsigned char xx;
            if (b8(&c, &xx) < 0)
                return 0;
            dpush(&o, c.opsize == 16 ? "endbr64" : "endbr64");
            goto done;
        }
        /* 0F 40-4F cmovcc */
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
            dpush(&o, "cmov");
            dpush(&o, cc16[op2 - 0x40]);
            dpush(&o, " ");
            MR2(c.opsize, 1);
            goto done;
        /* 0F 80-8F jcc rel32 */
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
            if (r32v(&c, &off) < 0)
                return 0;
            dpush(&o, "j");
            dpush(&o, cc16[op2 - 0x80]);
            dpush(&o, " ");
            dhex(&o, va + (unsigned long)(c.p - p) + (unsigned long)off);
            goto done;
        /* 0F 90-9F setcc r/m8 */
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97:
        case 0x98:
        case 0x99:
        case 0x9A:
        case 0x9B:
        case 0x9C:
        case 0x9D:
        case 0x9E:
        case 0x9F:
            dpush(&o, "set");
            dpush(&o, cc16[op2 - 0x90]);
            dpush(&o, " ");
            if (decode_mr(&c, &opd, 8, 8, 0) < 0)
                return 0;
            dpush(&o, opd.b);
            goto done;
        case 0xB0:
        case 0xB1: /* cmpxchg rm,r (reg src) */
            dpush(&o, "cmpxchg ");
            if (decode_mr(&c, &opd, op2 == 0xB0 ? 8 : c.opsize, op2 == 0xB0 ? 8 : c.opsize, 0) < 0)
                return 0;
            emit_op(&o, &opd, 0);
            goto done;
        case 0xB6:
        case 0xB7: /* movzx r, r/m8|16 -> AT&T: movzx src, dst */
            dpush(&o, "movz");
            dpush(&o, op2 == 0xB6 ? "bw " : "wl ");
            if (decode_mr(&c, &opd, c.opsize, op2 == 0xB6 ? 8 : 16, 0) < 0)
                return 0;
            emit_op(&o, &opd, 1);
            goto done;
        case 0xBE:
        case 0xBF: /* movsx r, r/m8|16 -> AT&T: movsx src, dst */
            dpush(&o, "movs");
            dpush(&o, op2 == 0xBE ? "bw " : "wl ");
            if (decode_mr(&c, &opd, c.opsize, op2 == 0xBE ? 8 : 16, 0) < 0)
                return 0;
            emit_op(&o, &opd, 1);
            goto done;
        case 0xB8: /* popcnt (F3) */
            dpush(&o, "popcnt ");
            MR2(c.opsize, 0);
            goto done;
        case 0xBC: /* bsf */
            dpush(&o, "bsf ");
            MR2(c.opsize, 0);
            goto done;
        case 0xBD: /* bsr */
            dpush(&o, "bsr ");
            MR2(c.opsize, 0);
            goto done;
        case 0xAF: /* imul r, r/m */
            dpush(&o, "imul ");
            MR2(c.opsize, 0);
            goto done;
        case 0xC0:
        case 0xC1: /* xadd rm,r (reg src) */
            dpush(&o, "xadd ");
            if (decode_mr(&c, &opd, op2 == 0xC0 ? 8 : c.opsize, op2 == 0xC0 ? 8 : c.opsize, 0) < 0)
                return 0;
            emit_op(&o, &opd, 0);
            goto done;
        /* SSE moves */
        case 0x10:
        case 0x11: /* movups */
        case 0x28:
        case 0x29: { /* movaps */
            const char *m = (op2 == 0x28 || op2 == 0x29) ? "movaps" : "movups";
            int load = (op2 == 0x10 || op2 == 0x28);
            dpush(&o, m);
            dpush(&o, " ");
            if (decode_mr(&c, &opd, 32, 32, 1) < 0)
                return 0;
            emit_op(&o, &opd, load ? 1 : 0);
            goto done;
        }
        case 0x6F:
        case 0x7F: { /* movdqa(66)/movdqu(F3) */
            int load = (op2 == 0x6F);
            dpush(&o, c.opsize == 16 ? "movdqa " : "movdqu ");
            if (decode_mr(&c, &opd, 32, 32, 1) < 0)
                return 0;
            emit_op(&o, &opd, load ? 1 : 0);
            goto done;
        }
        default:
            dpush(&o, ".byte ");
            dhex(&o, 0x0F);
            dpush(&o, ", ");
            dhex(&o, op2);
            goto done;
        }
    }

    switch (op) {
    /* add/or/adc/sbb/and/sub/xor/cmp — 00-3B, rm,r then r,rm */
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x38:
    case 0x39:
    case 0x3A:
    case 0x3B: {
        int g = (op >> 3) & 7;
        int lo = op & 3;
        int sz = (lo == 0 || lo == 2) ? 8 : c.opsize;
        /* AT&T prints src,dst.  rm,r form dest=rm => reg,rm.  r,rm dest=reg
         * => rm,reg.  cmp (no destination) always prints rm,reg. */
        int kind = (lo <= 1) ? 0 : 1;
        if (g == 7)
            kind = 1;
        dpush(&o, grp1[g]);
        dpush(&o, " ");
        if (decode_mr(&c, &opd, sz, sz, 0) < 0)
            return 0;
        emit_op(&o, &opd, kind);
        break;
    }
    /* al/eax,imm */
    case 0x04:
    case 0x0C:
    case 0x14:
    case 0x1C:
    case 0x24:
    case 0x2C:
    case 0x34:
    case 0x3C: {
        int g = (op >> 3) & 7;
        unsigned char ib;
        if (b8(&c, &ib) < 0)
            return 0;
        dpush(&o, grp1[g]);
        dpush(&o, " $");
        ddec(&o, (long)(char)ib);
        dpush(&o, ", %al");
        break;
    }
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
        dpush(&o, "push ");
        dpush(&o, regname((op - 0x50) | (c.rex_b ? 8 : 0), 64));
        break;
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
        dpush(&o, "pop ");
        dpush(&o, regname((op - 0x58) | (c.rex_b ? 8 : 0), 64));
        break;
    case 0x63: /* movsxd r64, r/m32 */
        dpush(&o, "movslq ");
        if (decode_mr(&c, &opd, c.opsize, 32, 0) < 0)
            return 0;
        emit_op(&o, &opd, 1);
        break;
    case 0x68:
        if (i32(&c, &u) < 0)
            return 0;
        dpush(&o, "push $");
        dhex(&o, u);
        break;
    case 0x6A: {
        unsigned char ib;
        if (b8(&c, &ib) < 0)
            return 0;
        dpush(&o, "push $");
        ddec(&o, (long)(char)ib);
        break;
    }
    case 0x69: /* imul r, r/m, imm32 */
        dpush(&o, "imul ");
        if (decode_mr(&c, &opd, c.opsize, c.opsize, 0) < 0)
            return 0;
        emit_op(&o, &opd, 0);
        dpush(&o, ", $");
        if (i32(&c, &u) < 0)
            return 0;
        dhex(&o, u);
        break;
    case 0x6B: { /* imul r, r/m, imm8 */
        unsigned char ib;
        dpush(&o, "imul ");
        if (decode_mr(&c, &opd, c.opsize, c.opsize, 0) < 0)
            return 0;
        emit_op(&o, &opd, 0);
        dpush(&o, ", $");
        if (b8(&c, &ib) < 0)
            return 0;
        ddec(&o, (long)(char)ib);
        break;
    }
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F:
        if (r8v(&c, &off) < 0)
            return 0;
        dpush(&o, "j");
        dpush(&o, cc16[op - 0x70]);
        dpush(&o, " ");
        dhex(&o, va + (unsigned long)(c.p - p) + (unsigned long)off);
        break;
    /* group 1 imm */
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83: {
        unsigned char mr;
        if (c.p >= c.end)
            return 0;
        mr = *c.p;
        c.p++;
        c.va++;
        int sz = (op == 0x80 || op == 0x82) ? 8 : c.opsize;
        int regf = (mr >> 3) & 7;
        struct operands o2;
        if (decode_mr_ex(&c, &o2, sz, sz, 0, mr) < 0)
            return 0;
        dpush(&o, grp1[regf]);
        dpush(&o, " $");
        if (op == 0x83 || op == 0x82) { /* imm8 sign-extended */
            unsigned char ib;
            if (b8(&c, &ib) < 0)
                return 0;
            dhex(&o, (unsigned long long)ib);
        } else if (sz == 8) {
            unsigned char ib;
            if (b8(&c, &ib) < 0)
                return 0;
            dhex(&o, (unsigned long long)ib);
        } else {
            if (i32(&c, &u) < 0)
                return 0;
            dhex(&o, u);
        }
        dpush(&o, ", ");
        dpush(&o, o2.b);
        break;
    }
    case 0x84:
    case 0x85: /* test rm,r */
        dpush(&o, "test ");
        if (decode_mr(&c, &opd, op == 0x84 ? 8 : c.opsize, op == 0x84 ? 8 : c.opsize, 0) < 0)
            return 0;
        emit_op(&o, &opd, 1);
        break;
    case 0x86:
    case 0x87: /* xchg r,rm (reg first) */
        dpush(&o, "xchg ");
        if (decode_mr(&c, &opd, op == 0x86 ? 8 : c.opsize, op == 0x86 ? 8 : c.opsize, 0) < 0)
            return 0;
        emit_op(&o, &opd, 0);
        break;
    case 0x88:
    case 0x89: /* mov rm,r -> AT&T mov r, rm */
        dpush(&o, "mov ");
        if (decode_mr(&c, &opd, op == 0x88 ? 8 : c.opsize, op == 0x88 ? 8 : c.opsize, 0) < 0)
            return 0;
        emit_op(&o, &opd, 0);
        break;
    case 0x8A:
    case 0x8B: /* mov r,rm -> AT&T mov rm, r */
        dpush(&o, "mov ");
        if (decode_mr(&c, &opd, op == 0x8A ? 8 : c.opsize, op == 0x8A ? 8 : c.opsize, 0) < 0)
            return 0;
        emit_op(&o, &opd, 1);
        break;
    case 0x8D: /* lea r,[m] -> AT&T: lea [m], r */
        dpush(&o, "lea ");
        if (decode_mr(&c, &opd, c.opsize, c.opsize, 0) < 0)
            return 0;
        emit_op(&o, &opd, 1);
        break;
    case 0x8F: /* pop rm */
        dpush(&o, "pop ");
        if (decode_mr(&c, &opd, 64, 64, 0) < 0)
            return 0;
        emit_op(&o, &opd, 1);
        break;
    case 0x90:
        dpush(&o, "nop");
        break;
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
        dpush(&o, "xchg ");
        dpush(&o, regname((op - 0x90) | (c.rex_b ? 8 : 0), c.opsize));
        dpush(&o, ", ");
        dpush(&o, regname(0, c.opsize));
        break;
    case 0x98:
        dpush(&o, c.rex_w ? "cdqe" : (c.opsize == 16 ? "cwde" : "cdqe"));
        break;
    case 0x99:
        dpush(&o, c.rex_w ? "cqo" : (c.opsize == 16 ? "cwd" : "cdq"));
        break;
    case 0x9B:
        dpush(&o, "fwait");
        break;
    case 0x9C:
        dpush(&o, "pushfq");
        break;
    case 0x9D:
        dpush(&o, "popfq");
        break;
    case 0x9E:
        dpush(&o, "sahf");
        break;
    case 0x9F:
        dpush(&o, "lahf");
        break;
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3: /* moffs forms */
        dpush(&o, "mov ");
        if (op == 0xA1 || op == 0xA3)
            dpush(&o, regname(0, c.opsize));
        else
            dpush(&o, "%al");
        dpush(&o, ", moffs"); /* simplified moffs */
        ;
        break;
    case 0xA4:
        dpush(&o, "movsb");
        break;
    case 0xA5:
        dpush(&o, c.opsize == 16 ? "movsw" : (c.rex_w ? "movsq" : "movsl"));
        break;
    case 0xA6:
        dpush(&o, "cmpsb");
        break;
    case 0xA7:
        dpush(&o, c.rex_w ? "cmpsq" : "cmpsl");
        break;
    case 0xA8: {
        unsigned char ib;
        if (b8(&c, &ib) < 0)
            return 0;
        dpush(&o, "test $");
        dhex(&o, (unsigned long long)ib);
        dpush(&o, ", %al");
        break;
    }
    case 0xA9:
        if (i32(&c, &u) < 0)
            return 0;
        dpush(&o, "test $");
        dhex(&o, u);
        dpush(&o, ", ");
        dpush(&o, regname(0, c.opsize));
        break;
    case 0xAA:
        dpush(&o, "stosb");
        break;
    case 0xAB:
        dpush(&o, c.rex_w ? "stosq" : "stosl");
        break;
    case 0xAC:
        dpush(&o, "lodsb");
        break;
    case 0xAD:
        dpush(&o, c.rex_w ? "lodsq" : "lodsl");
        break;
    case 0xAE:
        dpush(&o, "scasb");
        break;
    case 0xAF:
        dpush(&o, c.rex_w ? "scasq" : "scasl");
        break;
    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB6:
    case 0xB7: {
        unsigned char ib;
        if (b8(&c, &ib) < 0)
            return 0;
        dpush(&o, "mov $");
        dhex(&o, (unsigned long long)ib);
        dpush(&o, ", ");
        dpush(&o, regname((op - 0xB0) | (c.rex_b ? 8 : 0), 8));
        break;
    }
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0xBD:
    case 0xBE:
    case 0xBF: {
        int idx = (op - 0xB8) | (c.rex_b ? 8 : 0);
        if (c.rex_w) {
            if (i64(&c, &s64) < 0)
                return 0;
            dpush(&o, "mov $");
            dhex(&o, s64);
            dpush(&o, ", ");
            dpush(&o, regname(idx, 64));
        } else {
            if (i32(&c, &u) < 0)
                return 0;
            dpush(&o, "mov $");
            dhex(&o, u);
            dpush(&o, ", ");
            dpush(&o, regname(idx, c.opsize));
        }
        break;
    }
    case 0xC0:
    case 0xC1: { /* group 2 imm8 */
        unsigned char mr, ib;
        if (c.p >= c.end)
            return 0;
        mr = *c.p;
        c.p++;
        c.va++;
        int sz = (op == 0xC0) ? 8 : c.opsize;
        int regf = (mr >> 3) & 7;
        struct operands o2;
        if (decode_mr_ex(&c, &o2, sz, sz, 0, mr) < 0)
            return 0;
        dpush(&o, grp2[regf]);
        dpush(&o, " $");
        if (b8(&c, &ib) < 0)
            return 0;
        dhex(&o, (unsigned long long)ib);
        dpush(&o, ", ");
        dpush(&o, o2.b);
        break;
    }
    case 0xC2:
        if (i32(&c, &u) < 0)
            return 0;
        dpush(&o, "ret $");
        dhex(&o, u & 0xFFFF);
        break;
    case 0xC3:
        dpush(&o, "ret");
        break;
    case 0xC6:
    case 0xC7: { /* mov rm, imm */
        unsigned char mr;
        if (c.p >= c.end)
            return 0;
        mr = *c.p;
        c.p++;
        c.va++;
        int sz = (op == 0xC6) ? 8 : c.opsize;
        struct operands o2;
        if (decode_mr_ex(&c, &o2, sz, sz, 0, mr) < 0)
            return 0;
        dpush(&o, "mov $");
        if (sz == 8) {
            unsigned char ib;
            if (b8(&c, &ib) < 0)
                return 0;
            dhex(&o, (unsigned long long)ib);
        } else {
            if (i32(&c, &u) < 0)
                return 0;
            dhex(&o, u);
        }
        dpush(&o, ", ");
        dpush(&o, o2.b);
        break;
    }
    case 0xC9:
        dpush(&o, "leave");
        break;
    case 0xCB:
        dpush(&o, "retf");
        break;
    case 0xCC:
        dpush(&o, "int3");
        break;
    case 0xCD: {
        unsigned char ib;
        if (b8(&c, &ib) < 0)
            return 0;
        dpush(&o, "int $");
        dhex(&o, (unsigned long long)ib);
        break;
    }
    case 0xCF:
        dpush(&o, "iretq");
        break;
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3: {
        unsigned char mr;
        if (c.p >= c.end)
            return 0;
        mr = *c.p;
        c.p++;
        c.va++;
        int sz = (op == 0xD0 || op == 0xD2) ? 8 : c.opsize;
        int regf = (mr >> 3) & 7;
        int bycl = (op == 0xD2 || op == 0xD3);
        dpush(&o, grp2[regf]);
        dpush(&o, " ");
        if ((mr >> 6) == 3)
            dpush(&o, regname((mr & 7) | (c.rex_b ? 8 : 0), sz));
        else if (modrm_mem(&c, &o, mr >> 6, mr & 7) < 0)
            return 0;
        if (bycl)
            dpush(&o, ", %cl"); /* shift by CL */
        break;                  /* by 1: no explicit operand */
    }
    case 0xE0:
    case 0xE1:
    case 0xE2:
    case 0xE3:
        if (r8v(&c, &off) < 0)
            return 0;
        dpush(&o, op == 0xE0 ? "loopne " : op == 0xE1 ? "loope " : op == 0xE2 ? "loop " : "jrcxz ");
        dhex(&o, va + (unsigned long)(c.p - p) + (unsigned long)off);
        break;
    case 0xE8:
        if (r32v(&c, &off) < 0)
            return 0;
        dpush(&o, "call ");
        dhex(&o, va + (unsigned long)(c.p - p) + (unsigned long)off);
        break;
    case 0xE9:
        if (r32v(&c, &off) < 0)
            return 0;
        dpush(&o, "jmp ");
        dhex(&o, va + (unsigned long)(c.p - p) + (unsigned long)off);
        break;
    case 0xEB:
        if (r8v(&c, &off) < 0)
            return 0;
        dpush(&o, "jmp ");
        dhex(&o, va + (unsigned long)(c.p - p) + (unsigned long)off);
        break;
    case 0xF4:
        dpush(&o, "hlt");
        break;
    case 0xF5:
        dpush(&o, "cmc");
        break;
    case 0xF8:
        dpush(&o, "clc");
        break;
    case 0xF9:
        dpush(&o, "stc");
        break;
    case 0xFA:
        dpush(&o, "cli");
        break;
    case 0xFB:
        dpush(&o, "sti");
        break;
    case 0xFC:
        dpush(&o, "cld");
        break;
    case 0xFD:
        dpush(&o, "std");
        break;
    case 0xF6:
    case 0xF7: { /* group 3 */
        unsigned char mr;
        if (c.p >= c.end)
            return 0;
        mr = *c.p;
        c.p++;
        c.va++;
        int sz = (op == 0xF6) ? 8 : c.opsize;
        int regf = (mr >> 3) & 7;
        if (regf == 0 || regf == 1) {
            struct operands o2;
            if (decode_mr_ex(&c, &o2, sz, sz, 0, mr) < 0)
                return 0;
            dpush(&o, "test $");
            if (sz == 8) {
                unsigned char ib;
                if (b8(&c, &ib) < 0)
                    return 0;
                dhex(&o, (unsigned long long)ib);
            } else {
                if (i32(&c, &u) < 0)
                    return 0;
                dhex(&o, u);
            }
            dpush(&o, ", ");
            dpush(&o, o2.b);
        } else {
            dpush(&o, grp3[regf]);
            dpush(&o, " ");
            if ((mr >> 6) == 3)
                dpush(&o, regname((mr & 7) | (c.rex_b ? 8 : 0), sz));
            else if (modrm_mem(&c, &o, mr >> 6, mr & 7) < 0)
                return 0;
        }
        break;
    }
    case 0xFE: { /* inc/dec r/m8 */
        unsigned char mr;
        if (c.p >= c.end)
            return 0;
        mr = *c.p;
        c.p++;
        c.va++;
        int regf = (mr >> 3) & 7;
        if (regf <= 1) {
            dpush(&o, regf == 0 ? "inc " : "dec ");
            if ((mr >> 6) == 3)
                dpush(&o, regname((mr & 7) | (c.rex_b ? 8 : 0), 8));
            else if (modrm_mem(&c, &o, mr >> 6, mr & 7) < 0)
                return 0;
        } else {
            dpush(&o, ".byte 0xfe");
        }
        break;
    }
    case 0xFF: { /* group 5 */
        unsigned char mr;
        if (c.p >= c.end)
            return 0;
        mr = *c.p;
        c.p++;
        c.va++;
        int regf = (mr >> 3) & 7;
        if (regf == 0 || regf == 1) { /* inc/dec r/m */
            dpush(&o, regf == 0 ? "inc " : "dec ");
            if (decode_mr_ex(&c, &opd, c.opsize, c.opsize, 0, mr) < 0)
                return 0;
            dpush(&o, opd.b);
        } else if (regf == 2 || regf == 4 || regf == 6) {
            dpush(&o, regf == 2 ? "call *" : regf == 4 ? "jmp *" : "push ");
            int sz = regf == 6 ? 64 : c.opsize;
            if (decode_mr_ex(&c, &opd, sz, sz, 0, mr) < 0)
                return 0;
            dpush(&o, opd.b);
        } else {
            dpush(&o, ".byte 0xff");
        }
        break;
    }
    default:
        dpush(&o, ".byte ");
        dhex(&o, op);
        break;
    }
done:
    o.s[o.len] = '\0';
    return (int)(c.p - p);
}

static void show_header(const unsigned char *img) {
    struct elf64_header *eh = (struct elf64_header *)img;
    printf("architecture: x86-64\n");
    printf("entry point:  0x%llx\n", eh->e_entry);
}

static void show_sections(const unsigned char *img, unsigned long size) {
    struct elf64_header *eh = (struct elf64_header *)img;
    if (eh->e_shoff == 0 || eh->e_shnum == 0) {
        puts("objdump: no section headers");
        return;
    }
    puts("Sections:");
    puts("Idx  Name       Size        VMA");
    for (unsigned int i = 0; i < eh->e_shnum; i++) {
        unsigned long shoff = (unsigned long)eh->e_shoff + (unsigned long)i * eh->e_shentsize;
        if (shoff + sizeof(struct elf64_shdr) > size)
            break;
        struct elf64_shdr *sh = (struct elf64_shdr *)(img + shoff);
        const char *name = "";
        if (eh->e_shstrndx < eh->e_shnum) {
            unsigned long so =
                (unsigned long)eh->e_shoff + (unsigned long)eh->e_shstrndx * eh->e_shentsize;
            if (so + sizeof(struct elf64_shdr) <= size) {
                struct elf64_shdr *ss = (struct elf64_shdr *)(img + so);
                unsigned long st = (unsigned long)ss->sh_offset;
                if (st + sh->sh_name < size)
                    name = (const char *)(img + st + sh->sh_name);
            }
        }
        printf("%3u  %-11s %8llu  %llx\n", i, name[0] ? name : "<anon>", sh->sh_size, sh->sh_addr);
    }
}

static void disasm(const unsigned char *img, unsigned long size) {
    struct elf64_header *eh = (struct elf64_header *)img;
    if (eh->e_shoff == 0 || eh->e_shnum == 0)
        return;
    for (unsigned int i = 0; i < eh->e_shnum; i++) {
        unsigned long shoff = (unsigned long)eh->e_shoff + (unsigned long)i * eh->e_shentsize;
        if (shoff + sizeof(struct elf64_shdr) > size)
            break;
        struct elf64_shdr *sh = (struct elf64_shdr *)(img + shoff);
        if (!(sh->sh_flags & SHF_EXECINSTR) || sh->sh_size == 0)
            continue;
        if (sh->sh_offset + sh->sh_size > size)
            continue;

        printf("Disassembly of section %u:\n", i);
        const unsigned char *txt = img + sh->sh_offset;
        unsigned long len = (unsigned long)sh->sh_size;
        unsigned long va = (unsigned long)sh->sh_addr;
        unsigned long pos = 0;
        while (pos < len) {
            char line[160];
            unsigned long inslen =
                (unsigned long)disasm_insn(txt + pos, len - pos, va + pos, line, sizeof(line));
            if (inslen == 0)
                inslen = 1;
            if (inslen > len - pos)
                inslen = len - pos;

            printf("\t%lx:\t", va + pos);
            long shown = (long)inslen;
            if (shown > 7)
                shown = 7;
            for (long j = 0; j < shown; j++)
                printf("%02x ", txt[pos + j]);
            long pad = (7 - shown) * 3;
            while (pad > 0) {
                printf(" ");
                pad--;
            }
            if (inslen > 7)
                printf("... ");
            printf("\t%s\n", line);
            pos += inslen;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("usage: objdump <elf-file>");
        return 1;
    }
    int fd = open(argv[1], O_RDONLY, 0);
    if (fd < 0) {
        printf("objdump: %s: cannot open\n", argv[1]);
        return 1;
    }
    long end = lseek(fd, 0, 2); /* SEEK_END=2 */
    if (end < 0) {
        close(fd);
        return 1;
    }
    if (end > 32 * 1024 * 1024) {
        puts("objdump: file too large");
        close(fd);
        return 1;
    }
    lseek(fd, 0, 0);
    unsigned char *img = malloc((unsigned long)end);
    if (!img) {
        close(fd);
        return 1;
    }
    long got = 0;
    while (got < end) {
        long r = read(fd, img + got, (unsigned long)(end - got));
        if (r <= 0)
            break;
        got += r;
    }
    close(fd);

    if ((unsigned long)end < 64 || img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' ||
        img[3] != 'F') {
        printf("objdump: %s: not an ELF file\n", argv[1]);
        free(img);
        return 1;
    }
    struct elf64_header *eh = (struct elf64_header *)img;
    if (img[4] != 2) {
        puts("objdump: not 64-bit");
        free(img);
        return 1;
    }
    if (eh->e_machine != 62) {
        puts("objdump: not x86-64");
        free(img);
        return 1;
    }

    printf("\n%s:\tfile format elf64-x86-64\n", argv[1]);
    show_header(img);
    show_sections(img, (unsigned long)end);
    disasm(img, (unsigned long)end);
    free(img);
    return 0;
}