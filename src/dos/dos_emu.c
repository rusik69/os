/*
 * dos_emu.c - minimal x86-16 real-mode emulator core
 *
 * Provides a fetch-decode-execute loop for 16-bit real-mode x86 instructions
 * inside a 64-bit long-mode kernel process.
 *
 * Memory is a flat 1 MB array (DOS_CONV_MEM_SIZE).  Segment:offset linear
 * address = (segment << 4) + offset.
 *
 * Interrupt dispatch (used by INT 0xCD):
 *   dos_handle_int(state, int_num)  -- defined in dos_ints.c
 *     int 0x10  -- video services (AH subfunction)
 *     int 0x16  -- keyboard services (AH subfunction)
 *     int 0x20  -- exit program
 *     int 0x21  -- DOS services; implemented in dos_int21.c:
 *                   dos_handle_int21(state) dispatches on AH.
 *
 * The loop yields to the scheduler every ~1000 instructions.
 */

#include "dos.h"
#include "string.h"
#include "scheduler.h"
#include "printf.h"
#include "timer.h"

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */
/* Public API functions defined in this file */
uint8_t  dos_mem_readb(struct dos_cpu_state *state, uint32_t addr);
uint16_t dos_mem_readw(struct dos_cpu_state *state, uint32_t addr);
void     dos_mem_writeb(struct dos_cpu_state *state, uint32_t addr, uint8_t val);
void     dos_mem_writew(struct dos_cpu_state *state, uint32_t addr, uint16_t val);
uint32_t dos_ptr(struct dos_cpu_state *state, uint16_t seg, uint16_t off);
uint8_t  dos_read_seg_b(struct dos_cpu_state *state, uint16_t seg, uint16_t off);
uint16_t dos_read_seg_w(struct dos_cpu_state *state, uint16_t seg, uint16_t off);
void     dos_write_seg_b(struct dos_cpu_state *state, uint16_t seg, uint16_t off, uint8_t val);
void     dos_write_seg_w(struct dos_cpu_state *state, uint16_t seg, uint16_t off, uint16_t val);

/* Interrupt dispatch defined in dos_ints.c */
extern void dos_handle_int(struct dos_cpu_state *state, uint8_t int_num);
extern void dos_handle_int21(struct dos_cpu_state *state);

/* ------------------------------------------------------------------ */
/* Instruction-fetch helpers (advance CS:IP)                           */
/* ------------------------------------------------------------------ */
static uint8_t fetch_byte(struct dos_cpu_state *s)
{
    uint32_t addr = dos_ptr(s, s->cs, s->ip);
    s->ip++;
    return dos_mem_readb(s, addr);
}

static uint16_t fetch_word(struct dos_cpu_state *s)
{
    uint32_t addr = dos_ptr(s, s->cs, s->ip);
    uint16_t v = dos_mem_readw(s, addr);
    s->ip += 2;
    return v;
}

/* ------------------------------------------------------------------ */
/* Register access helpers                                             */
/* ------------------------------------------------------------------ */

/* Return a pointer to a 16-bit general register (0-7: AX, CX, DX, BX, SP, BP, SI, DI) */
static uint16_t *reg16_ptr(struct dos_cpu_state *s, int r)
{
    switch (r) {
    case 0: return &s->ax;
    case 1: return &s->cx;
    case 2: return &s->dx;
    case 3: return &s->bx;
    case 4: return &s->sp;
    case 5: return &s->bp;
    case 6: return &s->si;
    case 7: return &s->di;
    }
    return &s->ax;
}

/* Return a pointer to an 8-bit register (0-7: AL, CL, DL, BL, AH, CH, DH, BH) */
static uint8_t *reg8_ptr(struct dos_cpu_state *s, int r)
{
    switch (r) {
    case 0: return (uint8_t *)&s->ax;
    case 1: return (uint8_t *)&s->cx;
    case 2: return (uint8_t *)&s->dx;
    case 3: return (uint8_t *)&s->bx;
    case 4: return ((uint8_t *)&s->ax) + 1;
    case 5: return ((uint8_t *)&s->cx) + 1;
    case 6: return ((uint8_t *)&s->dx) + 1;
    case 7: return ((uint8_t *)&s->bx) + 1;
    }
    return (uint8_t *)&s->ax;
}

/* Return a pointer to a segment register (0-3: ES, CS, SS, DS) */
static uint16_t *sreg_ptr(struct dos_cpu_state *s, int r)
{
    switch (r) {
    case 0: return &s->es;
    case 1: return &s->cs;
    case 2: return &s->ss;
    case 3: return &s->ds;
    }
    return &s->ds;
}

/* ------------------------------------------------------------------ */
/* Flag helpers                                                        */
/* ------------------------------------------------------------------ */
static void set_pf(struct dos_cpu_state *s, uint8_t v)
{
    static const uint8_t parity[256] = {
        1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
        0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
        0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
        1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
        0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
        1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
        1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
        0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
    };
    s->flags &= ~DOS_FLAG_PF;
    if (parity[v]) s->flags |= DOS_FLAG_PF;
}

/* Set flags for logical operations: AND, OR, XOR, TEST */
static void set_flags_logic(struct dos_cpu_state *s, uint16_t result, int is_byte)
{
    uint16_t mask = is_byte ? 0xFF : 0xFFFF;
    uint16_t r = result & mask;
    s->flags &= ~(DOS_FLAG_OF | DOS_FLAG_CF | DOS_FLAG_SF | DOS_FLAG_ZF | DOS_FLAG_AF | DOS_FLAG_PF);
    s->flags |= DOS_FLAG_OF; /* OF=0 */
    s->flags |= DOS_FLAG_CF; /* CF=0 */
    if (r & (is_byte ? 0x80 : 0x8000))
        s->flags |= DOS_FLAG_SF;
    if (r == 0)
        s->flags |= DOS_FLAG_ZF;
    set_pf(s, r & 0xFF);
}

/* Set flags for addition: ADD, ADC */
static void set_flags_add(struct dos_cpu_state *s, uint16_t a, uint16_t b, uint16_t result, int is_byte, int carry_in)
{
    uint16_t mask = is_byte ? 0xFF : 0xFFFF;
    uint16_t sign_bit = is_byte ? 0x80 : 0x8000;
    a &= mask; b &= mask; result &= mask;
    uint32_t wide = (uint32_t)a + (uint32_t)b + carry_in;
    s->flags &= ~(DOS_FLAG_OF | DOS_FLAG_CF | DOS_FLAG_SF | DOS_FLAG_ZF | DOS_FLAG_AF | DOS_FLAG_PF);
    if (wide > mask)
        s->flags |= DOS_FLAG_CF;
    if (((~(a ^ b) & (a ^ result)) & sign_bit))
        s->flags |= DOS_FLAG_OF;
    if (result & sign_bit)
        s->flags |= DOS_FLAG_SF;
    if (result == 0)
        s->flags |= DOS_FLAG_ZF;
    if (((a & 0xF) + (b & 0xF) + carry_in) > 0xF)
        s->flags |= DOS_FLAG_AF;
    set_pf(s, result & 0xFF);
}

/* Set flags for subtraction: SUB, SBB, CMP */
static void set_flags_sub(struct dos_cpu_state *s, uint16_t a, uint16_t b, uint16_t result, int is_byte, int borrow_in)
{
    uint16_t mask = is_byte ? 0xFF : 0xFFFF;
    uint16_t sign_bit = is_byte ? 0x80 : 0x8000;
    a &= mask; b &= mask; result &= mask;
    s->flags &= ~(DOS_FLAG_OF | DOS_FLAG_CF | DOS_FLAG_SF | DOS_FLAG_ZF | DOS_FLAG_AF | DOS_FLAG_PF);
    if ((uint32_t)a < (uint32_t)b + borrow_in)
        s->flags |= DOS_FLAG_CF;
    if (((a ^ b) & (a ^ result) & sign_bit))
        s->flags |= DOS_FLAG_OF;
    if (result & sign_bit)
        s->flags |= DOS_FLAG_SF;
    if (result == 0)
        s->flags |= DOS_FLAG_ZF;
    if (((a & 0xF) < (b & 0xF) + borrow_in))
        s->flags |= DOS_FLAG_AF;
    set_pf(s, result & 0xFF);
}

/* Set flags for INC/DEC (affect OF,SF,ZF,AF,PF but NOT CF) */
static void set_flags_incdec(struct dos_cpu_state *s, uint16_t result, int is_byte, int is_inc)
{
    uint16_t mask = is_byte ? 0xFF : 0xFFFF;
    uint16_t sign_bit = is_byte ? 0x80 : 0x8000;
    result &= mask;
    s->flags &= ~(DOS_FLAG_OF | DOS_FLAG_SF | DOS_FLAG_ZF | DOS_FLAG_AF | DOS_FLAG_PF);
    if (is_inc) {
        if (result == sign_bit && (result & sign_bit))
            s->flags |= DOS_FLAG_OF;
    } else {
        if (result == (sign_bit - 1))
            s->flags |= DOS_FLAG_OF;
    }
    if (result & sign_bit)
        s->flags |= DOS_FLAG_SF;
    if (result == 0)
        s->flags |= DOS_FLAG_ZF;
    if ((result & 0xF) == (is_inc ? 0 : 0xF))
        s->flags |= DOS_FLAG_AF;
    set_pf(s, result & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Stack helpers                                                       */
/* ------------------------------------------------------------------ */
static void push_word(struct dos_cpu_state *s, uint16_t v)
{
    s->sp -= 2;
    dos_write_seg_w(s, s->ss, s->sp, v);
}

static uint16_t pop_word(struct dos_cpu_state *s)
{
    uint16_t v = dos_read_seg_w(s, s->ss, s->sp);
    s->sp += 2;
    return v;
}

/* ------------------------------------------------------------------ */
/* ModRM byte decoder                                                  */
/* ------------------------------------------------------------------ */
struct modrm {
    int    is_reg;       /* 1 = register direct (mod=11) */
    uint8_t reg;         /* register operand (bits 5-3) */
    int    rm_reg;       /* register number for r/m field (only if is_reg) */
    uint32_t addr;       /* linear address (if not register direct) */
    uint16_t disp;       /* displacement, zero-extended */
};

static struct modrm decode_modrm(struct dos_cpu_state *s, int word_mode __attribute__((unused)))
{
    struct modrm m;
    uint8_t modrm = fetch_byte(s);
    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;
    m.reg = (modrm >> 3) & 7;

    if (mod == 3) {
        m.is_reg = 1;
        m.rm_reg = rm;
        return m;
    }

    m.is_reg = 0;

    /* Compute offset (effective address) */
    uint16_t off = 0;
    if (mod == 0 && rm == 6) {
        off = fetch_word(s);
    } else {
        switch (rm) {
        case 0: off = s->bx + s->si; break;
        case 1: off = s->bx + s->di; break;
        case 2: off = s->bp + s->si; break;
        case 3: off = s->bp + s->di; break;
        case 4: off = s->si; break;
        case 5: off = s->di; break;
        case 6: off = s->bp; break;
        case 7: off = s->bx; break;
        }
        if (mod == 1)
            off += (int8_t)fetch_byte(s);
        else if (mod == 2)
            off += fetch_word(s);
    }
    m.addr = off;
    m.disp = off;
    return m;
}

/* ------------------------------------------------------------------ */
/* Helper: read r/m operand (byte or word)                             */
/* ------------------------------------------------------------------ */
static uint16_t read_rm(struct dos_cpu_state *s, struct modrm *m, int word_mode)
{
    if (m->is_reg) {
        if (word_mode)
            return *reg16_ptr(s, m->rm_reg);
        else
            return *reg8_ptr(s, m->rm_reg);
    }
    if (word_mode)
        return dos_mem_readw(s, m->addr);
    else
        return dos_mem_readb(s, m->addr);
}

/* Helper: write r/m operand */
static void write_rm(struct dos_cpu_state *s, struct modrm *m, int word_mode, uint16_t val)
{
    if (m->is_reg) {
        if (word_mode)
            *reg16_ptr(s, m->rm_reg) = val;
        else
            *reg8_ptr(s, m->rm_reg) = (uint8_t)val;
        return;
    }
    if (word_mode)
        dos_mem_writew(s, m->addr, val);
    else
        dos_mem_writeb(s, m->addr, (uint8_t)val);
}

/* Helper: read reg operand */
static uint16_t read_reg(struct dos_cpu_state *s, struct modrm *m, int word_mode)
{
    if (word_mode)
        return *reg16_ptr(s, m->reg);
    else
        return *reg8_ptr(s, m->reg);
}

/* Helper: write reg operand */
static void write_reg(struct dos_cpu_state *s, struct modrm *m, int word_mode, uint16_t val)
{
    if (word_mode)
        *reg16_ptr(s, m->reg) = val;
    else
        *reg8_ptr(s, m->reg) = (uint8_t)val;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void dos_emu_init(struct dos_cpu_state *state)
{
    state->ax = 0;
    state->bx = 0;
    state->cx = 0;
    state->dx = 0;
    state->si = 0;
    state->di = 0;
    state->bp = 0;
    state->sp = 0xFFFE;
    state->cs = 0;
    state->ds = 0;
    state->es = 0;
    state->ss = 0;
    state->ip = 0;
    state->flags = 0x0002; /* bit 1 always set */
    state->running = 0;
    state->memory = 0;
    state->file_handle_count = 0;
    __builtin_memset(state->file_handles, 0, sizeof(state->file_handles));
}

void dos_emu_stop(struct dos_cpu_state *state)
{
    state->running = 0;
}

/* ------------------------------------------------------------------ */
/* Memory access helpers                                               */
/* ------------------------------------------------------------------ */
uint8_t dos_mem_readb(struct dos_cpu_state *state, uint32_t addr)
{
    if (addr >= DOS_CONV_MEM_SIZE || !state->memory)
        return 0;
    return state->memory[addr];
}

uint16_t dos_mem_readw(struct dos_cpu_state *state, uint32_t addr)
{
    if (addr + 1 >= DOS_CONV_MEM_SIZE || !state->memory)
        return 0;
    return (uint16_t)state->memory[addr]
         | ((uint16_t)state->memory[addr + 1] << 8);
}

void dos_mem_writeb(struct dos_cpu_state *state, uint32_t addr, uint8_t val)
{
    if (addr >= DOS_CONV_MEM_SIZE || !state->memory)
        return;
    state->memory[addr] = val;
}

void dos_mem_writew(struct dos_cpu_state *state, uint32_t addr, uint16_t val)
{
    if (addr + 1 >= DOS_CONV_MEM_SIZE || !state->memory)
        return;
    state->memory[addr]     = (uint8_t)(val);
    state->memory[addr + 1] = (uint8_t)(val >> 8);
}

/* ------------------------------------------------------------------ */
/* Segment:offset helpers                                              */
/* ------------------------------------------------------------------ */
uint32_t dos_ptr(struct dos_cpu_state *state __attribute__((unused)), uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + off;
}

uint8_t dos_read_seg_b(struct dos_cpu_state *state, uint16_t seg, uint16_t off)
{
    return dos_mem_readb(state, dos_ptr(state, seg, off));
}

uint16_t dos_read_seg_w(struct dos_cpu_state *state, uint16_t seg, uint16_t off)
{
    return dos_mem_readw(state, dos_ptr(state, seg, off));
}

void dos_write_seg_b(struct dos_cpu_state *state, uint16_t seg, uint16_t off, uint8_t val)
{
    dos_mem_writeb(state, dos_ptr(state, seg, off), val);
}

void dos_write_seg_w(struct dos_cpu_state *state, uint16_t seg, uint16_t off, uint16_t val)
{
    dos_mem_writew(state, dos_ptr(state, seg, off), val);
}

/* ------------------------------------------------------------------ */
/* Main emulation loop                                                 */
/* ------------------------------------------------------------------ */
void dos_emu_run(struct dos_cpu_state *state)
{
    if (!state->memory)
        return;

    state->running = 1;
    int insn_count = 0;
    int total_insns = 0;
    uint64_t start_tick = timer_get_ticks();

    while (state->running) {
        uint8_t rep_prefix = 0;
        uint8_t opcode = fetch_byte(state);

    again:
        switch (opcode) {

        /* ---- 0x00-0x05: ADD ---- */
        case 0x00: { /* ADD r/m8, r8 */
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = (uint8_t)read_reg(state, &m, 0);
            uint8_t r = a + b;
            write_rm(state, &m, 0, r);
            set_flags_add(state, a, b, r, 1, 0);
            break;
        }
        case 0x01: { /* ADD r/m16, r16 */
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = read_reg(state, &m, 1);
            uint16_t r = a + b;
            write_rm(state, &m, 1, r);
            set_flags_add(state, a, b, r, 0, 0);
            break;
        }
        case 0x02: { /* ADD r8, r/m8 */
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_reg(state, &m, 0);
            uint8_t b = (uint8_t)read_rm(state, &m, 0);
            uint8_t r = a + b;
            write_reg(state, &m, 0, r);
            set_flags_add(state, a, b, r, 1, 0);
            break;
        }
        case 0x03: { /* ADD r16, r/m16 */
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_reg(state, &m, 1);
            uint16_t b = read_rm(state, &m, 1);
            uint16_t r = a + b;
            write_reg(state, &m, 1, r);
            set_flags_add(state, a, b, r, 0, 0);
            break;
        }
        case 0x04: { /* ADD AL, imm8 */
            uint8_t a = (uint8_t)state->ax;
            uint8_t b = fetch_byte(state);
            uint8_t r = a + b;
            state->ax = (state->ax & 0xFF00) | r;
            set_flags_add(state, a, b, r, 1, 0);
            break;
        }
        case 0x05: { /* ADD AX, imm16 */
            uint16_t a = state->ax;
            uint16_t b = fetch_word(state);
            uint16_t r = a + b;
            state->ax = r;
            set_flags_add(state, a, b, r, 0, 0);
            break;
        }

        /* ---- Segment register PUSH/POP ---- */
        case 0x06: push_word(state, state->es); break;
        case 0x0E: push_word(state, state->cs); break;
        case 0x16: push_word(state, state->ss); break;
        case 0x1E: push_word(state, state->ds); break;
        case 0x07: state->es = pop_word(state); break;
        case 0x17: state->ss = pop_word(state); break;
        case 0x1F: state->ds = pop_word(state); break;

        /* ---- 0x08-0x0D: OR ---- */
        case 0x08: { /* OR r/m8, r8 */
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = (uint8_t)read_reg(state, &m, 0);
            uint8_t r = a | b;
            write_rm(state, &m, 0, r);
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x09: { /* OR r/m16, r16 */
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = read_reg(state, &m, 1);
            uint16_t r = a | b;
            write_rm(state, &m, 1, r);
            set_flags_logic(state, r, 0);
            break;
        }
        case 0x0A: { /* OR r8, r/m8 */
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_reg(state, &m, 0);
            uint8_t b = (uint8_t)read_rm(state, &m, 0);
            uint8_t r = a | b;
            write_reg(state, &m, 0, r);
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x0B: { /* OR r16, r/m16 */
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_reg(state, &m, 1);
            uint16_t b = read_rm(state, &m, 1);
            uint16_t r = a | b;
            write_reg(state, &m, 1, r);
            set_flags_logic(state, r, 0);
            break;
        }
        case 0x0C: {
            uint8_t r = (uint8_t)state->ax | fetch_byte(state);
            state->ax = (state->ax & 0xFF00) | r;
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x0D: {
            uint16_t r = state->ax | fetch_word(state);
            state->ax = r;
            set_flags_logic(state, r, 0);
            break;
        }

        /* ---- 0x10-0x15: ADC ---- */
        case 0x10: { /* ADC r/m8, r8 */
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = (uint8_t)read_reg(state, &m, 0);
            int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t r = a + b + ci;
            write_rm(state, &m, 0, r);
            set_flags_add(state, a, b, r, 1, ci);
            break;
        }
        case 0x11: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = read_reg(state, &m, 1);
            int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t r = a + b + ci;
            write_rm(state, &m, 1, r);
            set_flags_add(state, a, b, r, 0, ci);
            break;
        }
        case 0x12: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_reg(state, &m, 0);
            uint8_t b = (uint8_t)read_rm(state, &m, 0);
            int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t r = a + b + ci;
            write_reg(state, &m, 0, r);
            set_flags_add(state, a, b, r, 1, ci);
            break;
        }
        case 0x13: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_reg(state, &m, 1);
            uint16_t b = read_rm(state, &m, 1);
            int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t r = a + b + ci;
            write_reg(state, &m, 1, r);
            set_flags_add(state, a, b, r, 0, ci);
            break;
        }
        case 0x14: {
            int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t a = (uint8_t)state->ax;
            uint8_t b = fetch_byte(state);
            uint8_t r = a + b + ci;
            state->ax = (state->ax & 0xFF00) | r;
            set_flags_add(state, a, b, r, 1, ci);
            break;
        }
        case 0x15: {
            int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t a = state->ax;
            uint16_t b = fetch_word(state);
            uint16_t r = a + b + ci;
            state->ax = r;
            set_flags_add(state, a, b, r, 0, ci);
            break;
        }

        /* ---- 0x18-0x1D: SBB ---- */
        case 0x18: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = (uint8_t)read_reg(state, &m, 0);
            int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t r = a - b - bi;
            write_rm(state, &m, 0, r);
            set_flags_sub(state, a, b, r, 1, bi);
            break;
        }
        case 0x19: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = read_reg(state, &m, 1);
            int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t r = a - b - bi;
            write_rm(state, &m, 1, r);
            set_flags_sub(state, a, b, r, 0, bi);
            break;
        }
        case 0x1A: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_reg(state, &m, 0);
            uint8_t b = (uint8_t)read_rm(state, &m, 0);
            int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t r = a - b - bi;
            write_reg(state, &m, 0, r);
            set_flags_sub(state, a, b, r, 1, bi);
            break;
        }
        case 0x1B: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_reg(state, &m, 1);
            uint16_t b = read_rm(state, &m, 1);
            int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t r = a - b - bi;
            write_reg(state, &m, 1, r);
            set_flags_sub(state, a, b, r, 0, bi);
            break;
        }
        case 0x1C: {
            int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t a = (uint8_t)state->ax;
            uint8_t b = fetch_byte(state);
            uint8_t r = a - b - bi;
            state->ax = (state->ax & 0xFF00) | r;
            set_flags_sub(state, a, b, r, 1, bi);
            break;
        }
        case 0x1D: {
            int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t a = state->ax;
            uint16_t b = fetch_word(state);
            uint16_t r = a - b - bi;
            state->ax = r;
            set_flags_sub(state, a, b, r, 0, bi);
            break;
        }

        /* ---- 0x20-0x25: AND ---- */
        case 0x20: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t r = (uint8_t)read_rm(state, &m, 0) & (uint8_t)read_reg(state, &m, 0);
            write_rm(state, &m, 0, r);
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x21: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t r = read_rm(state, &m, 1) & read_reg(state, &m, 1);
            write_rm(state, &m, 1, r);
            set_flags_logic(state, r, 0);
            break;
        }
        case 0x22: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t r = (uint8_t)read_reg(state, &m, 0) & (uint8_t)read_rm(state, &m, 0);
            write_reg(state, &m, 0, r);
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x23: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t r = read_reg(state, &m, 1) & read_rm(state, &m, 1);
            write_reg(state, &m, 1, r);
            set_flags_logic(state, r, 0);
            break;
        }
        case 0x24: {
            uint8_t r = (uint8_t)state->ax & fetch_byte(state);
            state->ax = (state->ax & 0xFF00) | r;
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x25: {
            uint16_t r = state->ax & fetch_word(state);
            state->ax = r;
            set_flags_logic(state, r, 0);
            break;
        }

        /* ---- 0x27: DAA, 0x2F: DAS, 0x37: AAA, 0x3F: AAS (stubs) ---- */
        case 0x27: /* DAA */ break;
        case 0x2F: /* DAS */ break;
        case 0x37: /* AAA */ break;
        case 0x3F: /* AAS */ break;

        /* ---- 0x28-0x2D: SUB ---- */
        case 0x28: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = (uint8_t)read_reg(state, &m, 0);
            uint8_t r = a - b;
            write_rm(state, &m, 0, r);
            set_flags_sub(state, a, b, r, 1, 0);
            break;
        }
        case 0x29: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = read_reg(state, &m, 1);
            uint16_t r = a - b;
            write_rm(state, &m, 1, r);
            set_flags_sub(state, a, b, r, 0, 0);
            break;
        }
        case 0x2A: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_reg(state, &m, 0);
            uint8_t b = (uint8_t)read_rm(state, &m, 0);
            uint8_t r = a - b;
            write_reg(state, &m, 0, r);
            set_flags_sub(state, a, b, r, 1, 0);
            break;
        }
        case 0x2B: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_reg(state, &m, 1);
            uint16_t b = read_rm(state, &m, 1);
            uint16_t r = a - b;
            write_reg(state, &m, 1, r);
            set_flags_sub(state, a, b, r, 0, 0);
            break;
        }
        case 0x2C: {
            uint8_t a = (uint8_t)state->ax;
            uint8_t b = fetch_byte(state);
            uint8_t r = a - b;
            state->ax = (state->ax & 0xFF00) | r;
            set_flags_sub(state, a, b, r, 1, 0);
            break;
        }
        case 0x2D: {
            uint16_t a = state->ax;
            uint16_t b = fetch_word(state);
            uint16_t r = a - b;
            state->ax = r;
            set_flags_sub(state, a, b, r, 0, 0);
            break;
        }

        /* ---- 0x30-0x35: XOR ---- */
        case 0x30: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t r = (uint8_t)read_rm(state, &m, 0) ^ (uint8_t)read_reg(state, &m, 0);
            write_rm(state, &m, 0, r);
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x31: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t r = read_rm(state, &m, 1) ^ read_reg(state, &m, 1);
            write_rm(state, &m, 1, r);
            set_flags_logic(state, r, 0);
            break;
        }
        case 0x32: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t r = (uint8_t)read_reg(state, &m, 0) ^ (uint8_t)read_rm(state, &m, 0);
            write_reg(state, &m, 0, r);
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x33: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t r = read_reg(state, &m, 1) ^ read_rm(state, &m, 1);
            write_reg(state, &m, 1, r);
            set_flags_logic(state, r, 0);
            break;
        }
        case 0x34: {
            uint8_t r = (uint8_t)state->ax ^ fetch_byte(state);
            state->ax = (state->ax & 0xFF00) | r;
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x35: {
            uint16_t r = state->ax ^ fetch_word(state);
            state->ax = r;
            set_flags_logic(state, r, 0);
            break;
        }

        /* ---- 0x38-0x3D: CMP ---- */
        case 0x38: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = (uint8_t)read_reg(state, &m, 0);
            uint8_t r = a - b;
            set_flags_sub(state, a, b, r, 1, 0);
            break;
        }
        case 0x39: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = read_reg(state, &m, 1);
            uint16_t r = a - b;
            set_flags_sub(state, a, b, r, 0, 0);
            break;
        }
        case 0x3A: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_reg(state, &m, 0);
            uint8_t b = (uint8_t)read_rm(state, &m, 0);
            uint8_t r = a - b;
            set_flags_sub(state, a, b, r, 1, 0);
            break;
        }
        case 0x3B: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_reg(state, &m, 1);
            uint16_t b = read_rm(state, &m, 1);
            uint16_t r = a - b;
            set_flags_sub(state, a, b, r, 0, 0);
            break;
        }
        case 0x3C: {
            uint8_t a = (uint8_t)state->ax;
            uint8_t b = fetch_byte(state);
            uint8_t r = a - b;
            set_flags_sub(state, a, b, r, 1, 0);
            break;
        }
        case 0x3D: {
            uint16_t a = state->ax;
            uint16_t b = fetch_word(state);
            uint16_t r = a - b;
            set_flags_sub(state, a, b, r, 0, 0);
            break;
        }

        /* ---- 0x40-0x47: INC r16 ---- */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {
            int r = opcode - 0x40;
            uint16_t v = *reg16_ptr(state, r);
            uint16_t newv = v + 1;
            *reg16_ptr(state, r) = newv;
            set_flags_incdec(state, newv, 0, 1);
            break;
        }

        /* ---- 0x48-0x4F: DEC r16 ---- */
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            int r = opcode - 0x48;
            uint16_t v = *reg16_ptr(state, r);
            uint16_t newv = v - 1;
            *reg16_ptr(state, r) = newv;
            set_flags_incdec(state, newv, 0, 0);
            break;
        }

        /* ---- 0x50-0x57: PUSH r16 ---- */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: {
            int r = opcode - 0x50;
            uint16_t v = *reg16_ptr(state, r);
            if (r == 4) v = state->sp; /* PUSH SP pushes SP before decrement */
            push_word(state, v);
            break;
        }

        /* ---- 0x58-0x5F: POP r16 ---- */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            int r = opcode - 0x58;
            *reg16_ptr(state, r) = pop_word(state);
            break;
        }

        /* ---- 0x70-0x7F: Conditional jumps ---- */
        case 0x70: { /* JO */
            int8_t disp = (int8_t)fetch_byte(state);
            if (state->flags & DOS_FLAG_OF) state->ip += disp;
            break;
        }
        case 0x71: { /* JNO */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!(state->flags & DOS_FLAG_OF)) state->ip += disp;
            break;
        }
        case 0x72: { /* JB/JNAE/JC */
            int8_t disp = (int8_t)fetch_byte(state);
            if (state->flags & DOS_FLAG_CF) state->ip += disp;
            break;
        }
        case 0x73: { /* JNB/JAE/JNC */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!(state->flags & DOS_FLAG_CF)) state->ip += disp;
            break;
        }
        case 0x74: { /* JE/JZ */
            int8_t disp = (int8_t)fetch_byte(state);
            if (state->flags & DOS_FLAG_ZF) state->ip += disp;
            break;
        }
        case 0x75: { /* JNE/JNZ */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!(state->flags & DOS_FLAG_ZF)) state->ip += disp;
            break;
        }
        case 0x76: { /* JBE/JNA */
            int8_t disp = (int8_t)fetch_byte(state);
            if ((state->flags & DOS_FLAG_CF) || (state->flags & DOS_FLAG_ZF)) state->ip += disp;
            break;
        }
        case 0x77: { /* JA/JNBE */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!(state->flags & DOS_FLAG_CF) && !(state->flags & DOS_FLAG_ZF)) state->ip += disp;
            break;
        }
        case 0x78: { /* JS */
            int8_t disp = (int8_t)fetch_byte(state);
            if (state->flags & DOS_FLAG_SF) state->ip += disp;
            break;
        }
        case 0x79: { /* JNS */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!(state->flags & DOS_FLAG_SF)) state->ip += disp;
            break;
        }
        case 0x7A: { /* JP/JPE */
            int8_t disp = (int8_t)fetch_byte(state);
            if (state->flags & DOS_FLAG_PF) state->ip += disp;
            break;
        }
        case 0x7B: { /* JNP/JPO */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!(state->flags & DOS_FLAG_PF)) state->ip += disp;
            break;
        }
        case 0x7C: { /* JL/JNGE */
            int8_t disp = (int8_t)fetch_byte(state);
            if ((state->flags & DOS_FLAG_SF) && !(state->flags & DOS_FLAG_OF)) goto jmp_taken;
            if (!(state->flags & DOS_FLAG_SF) && (state->flags & DOS_FLAG_OF)) goto jmp_taken;
            break;
            jmp_taken: state->ip += disp;
            break;
        }
        case 0x7D: { /* JGE/JNL */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!((state->flags & DOS_FLAG_SF) ^ !!(state->flags & DOS_FLAG_OF))) state->ip += disp;
            break;
        }
        case 0x7E: { /* JLE/JNG */
            int8_t disp = (int8_t)fetch_byte(state);
            if ((state->flags & DOS_FLAG_ZF) || ((state->flags & DOS_FLAG_SF) ^ !!(state->flags & DOS_FLAG_OF))) state->ip += disp;
            break;
        }
        case 0x7F: { /* JG/JNLE */
            int8_t disp = (int8_t)fetch_byte(state);
            if (!(state->flags & DOS_FLAG_ZF) && !((state->flags & DOS_FLAG_SF) ^ !!(state->flags & DOS_FLAG_OF))) state->ip += disp;
            break;
        }

        /* ---- 0x80-0x83: Group 1 (immediate ALU) ---- */
        case 0x80:
        case 0x82: { /* r/m8, imm8 */
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = fetch_byte(state);
            uint8_t r;
            switch (m.reg) {
            case 0: r = a + b; write_rm(state, &m, 0, r); set_flags_add(state, a, b, r, 1, 0); break;
            case 1: r = a | b; write_rm(state, &m, 0, r); set_flags_logic(state, r, 1); break;
            case 2: { int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0; r = a + b + ci; write_rm(state, &m, 0, r); set_flags_add(state, a, b, r, 1, ci); break; }
            case 3: { int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0; r = a - b - bi; write_rm(state, &m, 0, r); set_flags_sub(state, a, b, r, 1, bi); break; }
            case 4: r = a & b; write_rm(state, &m, 0, r); set_flags_logic(state, r, 1); break;
            case 5: r = a - b; write_rm(state, &m, 0, r); set_flags_sub(state, a, b, r, 1, 0); break;
            case 6: r = a ^ b; write_rm(state, &m, 0, r); set_flags_logic(state, r, 1); break;
            case 7: r = a - b; set_flags_sub(state, a, b, r, 1, 0); break;
            }
            break;
        }
        case 0x81: { /* r/m16, imm16 */
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = fetch_word(state);
            uint16_t r;
            switch (m.reg) {
            case 0: r = a + b; write_rm(state, &m, 1, r); set_flags_add(state, a, b, r, 0, 0); break;
            case 1: r = a | b; write_rm(state, &m, 1, r); set_flags_logic(state, r, 0); break;
            case 2: { int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0; r = a + b + ci; write_rm(state, &m, 1, r); set_flags_add(state, a, b, r, 0, ci); break; }
            case 3: { int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0; r = a - b - bi; write_rm(state, &m, 1, r); set_flags_sub(state, a, b, r, 0, bi); break; }
            case 4: r = a & b; write_rm(state, &m, 1, r); set_flags_logic(state, r, 0); break;
            case 5: r = a - b; write_rm(state, &m, 1, r); set_flags_sub(state, a, b, r, 0, 0); break;
            case 6: r = a ^ b; write_rm(state, &m, 1, r); set_flags_logic(state, r, 0); break;
            case 7: r = a - b; set_flags_sub(state, a, b, r, 0, 0); break;
            }
            break;
        }
        case 0x83: { /* r/m16, imm8 sign-extended */
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            int16_t b = (int8_t)fetch_byte(state);
            uint16_t ub = (uint16_t)b;
            uint16_t r;
            switch (m.reg) {
            case 0: r = a + ub; write_rm(state, &m, 1, r); set_flags_add(state, a, ub, r, 0, 0); break;
            case 1: r = a | ub; write_rm(state, &m, 1, r); set_flags_logic(state, r, 0); break;
            case 2: { int ci = (state->flags & DOS_FLAG_CF) ? 1 : 0; r = a + ub + ci; write_rm(state, &m, 1, r); set_flags_add(state, a, ub, r, 0, ci); break; }
            case 3: { int bi = (state->flags & DOS_FLAG_CF) ? 1 : 0; r = a - ub - bi; write_rm(state, &m, 1, r); set_flags_sub(state, a, ub, r, 0, bi); break; }
            case 4: r = a & ub; write_rm(state, &m, 1, r); set_flags_logic(state, r, 0); break;
            case 5: r = a - ub; write_rm(state, &m, 1, r); set_flags_sub(state, a, ub, r, 0, 0); break;
            case 6: r = a ^ ub; write_rm(state, &m, 1, r); set_flags_logic(state, r, 0); break;
            case 7: r = a - ub; set_flags_sub(state, a, ub, r, 0, 0); break;
            }
            break;
        }

        /* ---- 0x84-0x85: TEST ---- */
        case 0x84: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t r = (uint8_t)read_rm(state, &m, 0) & (uint8_t)read_reg(state, &m, 0);
            set_flags_logic(state, r, 1);
            break;
        }
        case 0x85: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t r = read_rm(state, &m, 1) & read_reg(state, &m, 1);
            set_flags_logic(state, r, 0);
            break;
        }

        /* ---- 0x86-0x87: XCHG ---- */
        case 0x86: {
            struct modrm m = decode_modrm(state, 0);
            uint8_t a = (uint8_t)read_rm(state, &m, 0);
            uint8_t b = (uint8_t)read_reg(state, &m, 0);
            write_rm(state, &m, 0, b);
            write_reg(state, &m, 0, a);
            break;
        }
        case 0x87: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t a = read_rm(state, &m, 1);
            uint16_t b = read_reg(state, &m, 1);
            write_rm(state, &m, 1, b);
            write_reg(state, &m, 1, a);
            break;
        }

        /* ---- 0x88-0x8B: MOV register/memory ---- */
        case 0x88: { /* MOV r/m8, r8 */
            struct modrm m = decode_modrm(state, 0);
            write_rm(state, &m, 0, read_reg(state, &m, 0));
            break;
        }
        case 0x89: { /* MOV r/m16, r16 */
            struct modrm m = decode_modrm(state, 1);
            write_rm(state, &m, 1, read_reg(state, &m, 1));
            break;
        }
        case 0x8A: { /* MOV r8, r/m8 */
            struct modrm m = decode_modrm(state, 0);
            write_reg(state, &m, 0, read_rm(state, &m, 0));
            break;
        }
        case 0x8B: { /* MOV r16, r/m16 */
            struct modrm m = decode_modrm(state, 1);
            write_reg(state, &m, 1, read_rm(state, &m, 1));
            break;
        }

        /* ---- 0x8C: MOV r/m16, sreg ---- */
        case 0x8C: {
            struct modrm m = decode_modrm(state, 1);
            write_rm(state, &m, 1, *sreg_ptr(state, m.reg));
            break;
        }

        /* ---- 0x8E: MOV sreg, r/m16 ---- */
        case 0x8E: {
            struct modrm m = decode_modrm(state, 1);
            uint16_t v = read_rm(state, &m, 1);
            if (m.reg == 1) { state->cs = v; break; }
            *sreg_ptr(state, m.reg) = v;
            break;
        }

        /* ---- 0x90-0x97: XCHG AX, r16 (0x90 = NOP) ---- */
        case 0x90: break; /* NOP (XCHG AX, AX) */
        case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: {
            int r = opcode - 0x90;
            uint16_t tmp = state->ax;
            state->ax = *reg16_ptr(state, r);
            *reg16_ptr(state, r) = tmp;
            break;
        }

        /* ---- 0x98: CBW ---- */
        case 0x98:
            state->ax = (int16_t)(int8_t)(state->ax & 0xFF);
            break;

        /* ---- 0x99: CWD ---- */
        case 0x99:
            if (state->ax & 0x8000) {
                state->dx = 0xFFFF;
            } else {
                state->dx = 0;
            }
            break;

        /* ---- 0x9C: PUSHF ---- */
        case 0x9C:
            push_word(state, state->flags);
            break;

        /* ---- 0x9D: POPF ---- */
        case 0x9D:
            state->flags = pop_word(state);
            break;

        /* ---- 0x9E: SAHF ---- */
        case 0x9E:
            state->flags = (state->flags & 0xFF00) | ((uint8_t)state->ax);
            break;

        /* ---- 0x9F: LAHF ---- */
        case 0x9F:
            state->ax = (state->ax & 0x00FF) | ((uint16_t)(state->flags & 0xFF) << 8);
            break;

        /* ---- 0xA0-0xA3: MOV with direct address ---- */
        case 0xA0: { /* MOV AL, moffs8 */
            uint16_t off = fetch_word(state);
            state->ax = (state->ax & 0xFF00) | dos_read_seg_b(state, state->ds, off);
            break;
        }
        case 0xA1: { /* MOV AX, moffs16 */
            uint16_t off = fetch_word(state);
            state->ax = dos_read_seg_w(state, state->ds, off);
            break;
        }
        case 0xA2: { /* MOV moffs8, AL */
            uint16_t off = fetch_word(state);
            dos_write_seg_b(state, state->ds, off, (uint8_t)state->ax);
            break;
        }
        case 0xA3: { /* MOV moffs16, AX */
            uint16_t off = fetch_word(state);
            dos_write_seg_w(state, state->ds, off, state->ax);
            break;
        }

        /* ---- 0xA4-0xA5: MOVS ---- */
        case 0xA4: { /* MOVSB */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                uint8_t v = dos_read_seg_b(state, state->ds, state->si);
                dos_write_seg_b(state, state->es, state->di, v);
                if (state->flags & DOS_FLAG_DF) { state->si--; state->di--; }
                else { state->si++; state->di++; }
            }
            if (rep_prefix) state->cx = 0;
            rep_prefix = 0;
            break;
        }
        case 0xA5: { /* MOVSW */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                uint16_t v = dos_read_seg_w(state, state->ds, state->si);
                dos_write_seg_w(state, state->es, state->di, v);
                if (state->flags & DOS_FLAG_DF) { state->si -= 2; state->di -= 2; }
                else { state->si += 2; state->di += 2; }
            }
            if (rep_prefix) state->cx = 0;
            rep_prefix = 0;
            break;
        }

        /* ---- 0xAA-0xAB: STOS ---- */
        case 0xAA: { /* STOSB */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                dos_write_seg_b(state, state->es, state->di, (uint8_t)state->ax);
                if (state->flags & DOS_FLAG_DF) state->di--;
                else state->di++;
            }
            if (rep_prefix) state->cx = 0;
            rep_prefix = 0;
            break;
        }
        case 0xAB: { /* STOSW */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                dos_write_seg_w(state, state->es, state->di, state->ax);
                if (state->flags & DOS_FLAG_DF) state->di -= 2;
                else state->di += 2;
            }
            if (rep_prefix) state->cx = 0;
            rep_prefix = 0;
            break;
        }

        /* ---- 0xAC-0xAD: LODS ---- */
        case 0xAC: { /* LODSB */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                state->ax = (state->ax & 0xFF00) | dos_read_seg_b(state, state->ds, state->si);
                if (state->flags & DOS_FLAG_DF) state->si--;
                else state->si++;
            }
            if (rep_prefix) state->cx = 0;
            rep_prefix = 0;
            break;
        }
        case 0xAD: { /* LODSW */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                state->ax = dos_read_seg_w(state, state->ds, state->si);
                if (state->flags & DOS_FLAG_DF) state->si -= 2;
                else state->si += 2;
            }
            if (rep_prefix) state->cx = 0;
            rep_prefix = 0;
            break;
        }

        /* ---- 0xAE-0xAF: SCAS ---- */
        case 0xAE: { /* SCASB */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                uint8_t a = (uint8_t)state->ax;
                uint8_t b = dos_read_seg_b(state, state->es, state->di);
                uint8_t r = a - b;
                set_flags_sub(state, a, b, r, 1, 0);
                if (state->flags & DOS_FLAG_DF) state->di--;
                else state->di++;
                if (rep_prefix) {
                    state->cx--;
                    if (rep_prefix == 1 && (state->flags & DOS_FLAG_ZF)) break; /* REPNE: stop if ZF=1 */
                    if (rep_prefix == 2 && !(state->flags & DOS_FLAG_ZF)) break; /* REPE: stop if ZF=0 */
                    count = state->cx;
                }
            }
            rep_prefix = 0;
            break;
        }
        case 0xAF: { /* SCASW */
            uint16_t count = rep_prefix ? state->cx : 1;
            while (count--) {
                uint16_t a = state->ax;
                uint16_t b = dos_read_seg_w(state, state->es, state->di);
                uint16_t r = a - b;
                set_flags_sub(state, a, b, r, 0, 0);
                if (state->flags & DOS_FLAG_DF) state->di -= 2;
                else state->di += 2;
                if (rep_prefix) {
                    state->cx--;
                    if (rep_prefix == 1 && (state->flags & DOS_FLAG_ZF)) break;
                    if (rep_prefix == 2 && !(state->flags & DOS_FLAG_ZF)) break;
                    count = state->cx;
                }
            }
            rep_prefix = 0;
            break;
        }

        /* ---- 0xB0-0xB7: MOV r8, imm8 ---- */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            *reg8_ptr(state, opcode - 0xB0) = fetch_byte(state);
            break;
        }

        /* ---- 0xB8-0xBF: MOV r16, imm16 ---- */
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            *reg16_ptr(state, opcode - 0xB8) = fetch_word(state);
            break;
        }

        /* ---- 0xC3: RET near ---- */
        case 0xC3:
            state->ip = pop_word(state);
            break;

        /* ---- 0xCB: RETF ---- */
        case 0xCB:
            state->ip = pop_word(state);
            state->cs = pop_word(state);
            break;

        /* ---- 0xC6: MOV r/m8, imm8 ---- */
        case 0xC6: {
            struct modrm m = decode_modrm(state, 0);
            if (m.reg != 0) break; /* only /0 is MOV */
            write_rm(state, &m, 0, fetch_byte(state));
            break;
        }

        /* ---- 0xC7: MOV r/m16, imm16 ---- */
        case 0xC7: {
            struct modrm m = decode_modrm(state, 1);
            if (m.reg != 0) break;
            write_rm(state, &m, 1, fetch_word(state));
            break;
        }

        /* ---- 0xCD: INT imm8 ---- */
        case 0xCD: {
            uint8_t int_num = fetch_byte(state);
            dos_handle_int(state, int_num);
            break;
        }

        /* ---- 0xD0-0xD3: shift/rotate group ---- */
        case 0xD0: { /* r/m8, 1 */
            struct modrm m = decode_modrm(state, 0);
            uint8_t v = (uint8_t)read_rm(state, &m, 0);
            int old_cf = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t r = 0;
            switch (m.reg) {
            case 0: { /* ROL */
                int cf = (v >> 7) & 1;
                r = (v << 1) | cf;
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r & 0x80) ^ (cf << 7)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 1: { /* ROR */
                int cf = v & 1;
                r = (v >> 1) | (cf << 7);
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if (((r >> 7) ^ ((v >> 6) & 1))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 2: { /* RCL */
                uint16_t tmp = (old_cf << 8) | v;
                int cf = (tmp >> 7) & 1;
                r = (uint8_t)((tmp << 1) | (tmp >> 8));
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r & 0x80) ^ (cf << 7)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 3: { /* RCR */
                uint16_t tmp = (old_cf << 8) | v;
                int cf = tmp & 1;
                r = (uint8_t)((tmp >> 1) | (cf << 7));
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r >> 7) ^ ((v >> 7) & 1)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 4: { /* SHL/SAL */
                int cf = (v >> 7) & 1;
                r = v << 1;
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                state->flags &= ~DOS_FLAG_OF;
                if ((r & 0x80) ^ (cf << 7)) state->flags |= DOS_FLAG_OF;
                break;
            }
            case 5: { /* SHR */
                int cf = v & 1;
                r = v >> 1;
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                state->flags &= ~DOS_FLAG_OF;
                if (v & 0x80) state->flags |= DOS_FLAG_OF;
                break;
            }
            case 6: { /* SAR */
                int cf = v & 1;
                r = (v >> 1) | (v & 0x80);
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                state->flags &= ~DOS_FLAG_OF;
                if ((r & 0x80) != (v & 0x80)) state->flags |= DOS_FLAG_OF;
                /* Actually OF for SAR is always 0 (defined as 0 after SAR by 1) */
                state->flags &= ~DOS_FLAG_OF;
                break;
            }
            }
            write_rm(state, &m, 0, r);
            break;
        }
        case 0xD1: { /* r/m16, 1 */
            struct modrm m = decode_modrm(state, 1);
            uint16_t v = read_rm(state, &m, 1);
            int old_cf = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t r = 0;
            switch (m.reg) {
            case 0: { /* ROL */
                int cf = (v >> 15) & 1;
                r = (v << 1) | cf;
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r >> 15) ^ cf) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 1: { /* ROR */
                int cf = v & 1;
                r = (v >> 1) | (cf << 15);
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r >> 15) ^ ((v >> 14) & 1)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 2: { /* RCL */
                uint32_t tmp = (old_cf << 16) | v;
                int cf = (tmp >> 15) & 1;
                r = (uint16_t)((tmp << 1) | (tmp >> 16));
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r >> 15) ^ cf) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 3: { /* RCR */
                uint32_t tmp = (old_cf << 16) | v;
                int cf = tmp & 1;
                r = (uint16_t)((tmp >> 1) | (cf << 15));
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r >> 15) ^ ((v >> 15) & 1)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 4: { /* SHL/SAL */
                int cf = (v >> 15) & 1;
                r = v << 1;
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if ((r >> 15) ^ cf) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 5: { /* SHR */
                int cf = v & 1;
                r = v >> 1;
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                if (v & 0x8000) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                break;
            }
            case 6: { /* SAR */
                int cf = v & 1;
                r = (v >> 1) | (v & 0x8000);
                if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                state->flags &= ~DOS_FLAG_OF;
                break;
            }
            }
            write_rm(state, &m, 1, r);
            break;
        }
        case 0xD2: { /* r/m8, CL */
            struct modrm m = decode_modrm(state, 0);
            uint8_t v = (uint8_t)read_rm(state, &m, 0);
            int count = (uint8_t)state->cx & 0x1F;
            int old_cf = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint8_t r = v;
            if (count > 0) {
                switch (m.reg) {
                case 0: {
                    int cf;
                    for (int i = 0; i < count; i++) {
                        cf = (r >> 7) & 1;
                        r = (r << 1) | cf;
                    }
                    if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r & 0x80) ^ (cf << 7))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 1: {
                    int cf;
                    for (int i = 0; i < count; i++) {
                        cf = r & 1;
                        r = (r >> 1) | (cf << 7);
                    }
                    if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r >> 7) ^ ((v >> 6) & 1))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 2: {
                    uint16_t tmp;
                    if (count > 9) count = 9;
                    for (int i = 0; i < count; i++) {
                        tmp = (old_cf << 8) | r;
                        old_cf = (tmp >> 7) & 1;
                        r = (uint8_t)((tmp << 1) | (tmp >> 8));
                    }
                    if (old_cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r & 0x80) ^ (old_cf << 7))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 3: {
                    uint16_t tmp;
                    if (count > 9) count = 9;
                    for (int i = 0; i < count; i++) {
                        tmp = (old_cf << 8) | r;
                        old_cf = tmp & 1;
                        r = (uint8_t)((tmp >> 1) | (old_cf << 7));
                    }
                    if (old_cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r >> 7) ^ ((v >> 7) & 1))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 4: {
                    for (int i = 0; i < count; i++) {
                        state->flags &= ~DOS_FLAG_CF;
                        if (r & 0x80) state->flags |= DOS_FLAG_CF;
                        r <<= 1;
                    }
                    if (count == 1 && ((r & 0x80) ^ ((v & 0x80) ? 1 : 0))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 5: {
                    for (int i = 0; i < count; i++) {
                        state->flags &= ~DOS_FLAG_CF;
                        if (r & 1) state->flags |= DOS_FLAG_CF;
                        r >>= 1;
                    }
                    if (count == 1 && (v & 0x80)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 6: {
                    for (int i = 0; i < count; i++) {
                        state->flags &= ~DOS_FLAG_CF;
                        if (r & 1) state->flags |= DOS_FLAG_CF;
                        r = (r >> 1) | (r & 0x80);
                    }
                    state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                }
            }
            write_rm(state, &m, 0, r);
            break;
        }
        case 0xD3: { /* r/m16, CL */
            struct modrm m = decode_modrm(state, 1);
            uint16_t v = read_rm(state, &m, 1);
            int count = (uint8_t)state->cx & 0x1F;
            int old_cf = (state->flags & DOS_FLAG_CF) ? 1 : 0;
            uint16_t r = v;
            if (count > 0) {
                switch (m.reg) {
                case 0: {
                    int cf;
                    for (int i = 0; i < count; i++) {
                        cf = (r >> 15) & 1;
                        r = (r << 1) | cf;
                    }
                    if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r >> 15) ^ cf)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 1: {
                    int cf;
                    for (int i = 0; i < count; i++) {
                        cf = r & 1;
                        r = (r >> 1) | (cf << 15);
                    }
                    if (cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r >> 15) ^ ((v >> 14) & 1))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 2: {
                    uint32_t tmp;
                    if (count > 17) count = 17;
                    for (int i = 0; i < count; i++) {
                        tmp = (old_cf << 16) | r;
                        old_cf = (tmp >> 15) & 1;
                        r = (uint16_t)((tmp << 1) | (tmp >> 16));
                    }
                    if (old_cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r >> 15) ^ old_cf)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 3: {
                    uint32_t tmp;
                    if (count > 17) count = 17;
                    for (int i = 0; i < count; i++) {
                        tmp = (old_cf << 16) | r;
                        old_cf = tmp & 1;
                        r = (uint16_t)((tmp >> 1) | (old_cf << 15));
                    }
                    if (old_cf) state->flags |= DOS_FLAG_CF; else state->flags &= ~DOS_FLAG_CF;
                    if (count == 1 && ((r >> 15) ^ ((v >> 15) & 1))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 4: {
                    for (int i = 0; i < count; i++) {
                        state->flags &= ~DOS_FLAG_CF;
                        if (r & 0x8000) state->flags |= DOS_FLAG_CF;
                        r <<= 1;
                    }
                    if (count == 1 && ((r & 0x8000) ^ (v & 0x8000))) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 5: {
                    for (int i = 0; i < count; i++) {
                        state->flags &= ~DOS_FLAG_CF;
                        if (r & 1) state->flags |= DOS_FLAG_CF;
                        r >>= 1;
                    }
                    if (count == 1 && (v & 0x8000)) state->flags |= DOS_FLAG_OF; else state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                case 6: {
                    for (int i = 0; i < count; i++) {
                        state->flags &= ~DOS_FLAG_CF;
                        if (r & 1) state->flags |= DOS_FLAG_CF;
                        r = (r >> 1) | (r & 0x8000);
                    }
                    state->flags &= ~DOS_FLAG_OF;
                    break;
                }
                }
            }
            write_rm(state, &m, 1, r);
            break;
        }

        /* ---- 0xE0-0xE3: LOOP/LOOPE/LOOPNE/JCXZ ---- */
        case 0xE0: { /* LOOPNE/LOOPNZ */
            int8_t disp = (int8_t)fetch_byte(state);
            state->cx--;
            if (state->cx != 0 && !(state->flags & DOS_FLAG_ZF))
                state->ip += disp;
            break;
        }
        case 0xE1: { /* LOOPE/LOOPZ */
            int8_t disp = (int8_t)fetch_byte(state);
            state->cx--;
            if (state->cx != 0 && (state->flags & DOS_FLAG_ZF))
                state->ip += disp;
            break;
        }
        case 0xE2: { /* LOOP */
            int8_t disp = (int8_t)fetch_byte(state);
            state->cx--;
            if (state->cx != 0)
                state->ip += disp;
            break;
        }
        case 0xE3: { /* JCXZ */
            int8_t disp = (int8_t)fetch_byte(state);
            if (state->cx == 0)
                state->ip += disp;
            break;
        }

        /* ---- 0xE8: CALL near ---- */
        case 0xE8: {
            int16_t disp = fetch_word(state);
            push_word(state, state->ip);
            state->ip += disp;
            break;
        }

        /* ---- 0xE9: JMP near ---- */
        case 0xE9: {
            state->ip += fetch_word(state);
            break;
        }

        /* ---- 0xEB: JMP short ---- */
        case 0xEB: {
            int8_t disp = (int8_t)fetch_byte(state);
            state->ip += disp;
            break;
        }

        /* ---- 0xEC: IN AL, DX (stub) ---- */
        case 0xEC:
            break;

        /* ---- 0xEE: OUT DX, AL (stub) ---- */
        case 0xEE:
            break;

        /* ---- 0xF2, 0xF3: REP prefixes ---- */
        case 0xF2:
            rep_prefix = 1;
            opcode = fetch_byte(state);
            goto again;
        case 0xF3:
            rep_prefix = 2;
            opcode = fetch_byte(state);
            goto again;

        /* ---- 0xF6-0xF7: Group 3 / Group 5 ---- */
        case 0xF6: { /* r/m8 operation */
            struct modrm m = decode_modrm(state, 0);
            uint8_t v = (uint8_t)read_rm(state, &m, 0);
            switch (m.reg) {
            case 0: { /* TEST r/m8, imm8 */
                uint8_t imm = fetch_byte(state);
                uint8_t r = v & imm;
                set_flags_logic(state, r, 1);
                break;
            }
            case 2: { /* NOT r/m8 */
                write_rm(state, &m, 0, (uint8_t)(~v));
                break;
            }
            case 3: { /* NEG r/m8 */
                uint8_t r = (uint8_t)(-(int16_t)v);
                write_rm(state, &m, 0, r);
                state->flags &= ~(DOS_FLAG_CF | DOS_FLAG_OF | DOS_FLAG_SF | DOS_FLAG_ZF | DOS_FLAG_AF | DOS_FLAG_PF);
                if (v != 0) state->flags |= DOS_FLAG_CF;
                if (v == 0x80) state->flags |= DOS_FLAG_OF;
                if (r & 0x80) state->flags |= DOS_FLAG_SF;
                if (r == 0) state->flags |= DOS_FLAG_ZF;
                if ((r & 0xF) != 0) state->flags |= DOS_FLAG_AF; /* AF is set if borrow from bit 4, which is ~(a&0xF)+1 > 0xF */
                set_pf(state, r);
                break;
            }
            case 4: { /* MUL r/m8 (unsigned) */
                uint16_t result = (uint16_t)(uint8_t)state->ax * (uint16_t)v;
                state->ax = result;
                state->flags &= ~(DOS_FLAG_CF | DOS_FLAG_OF);
                if (result & 0xFF00) state->flags |= DOS_FLAG_CF | DOS_FLAG_OF;
                break;
            }
            case 5: { /* IMUL r/m8 (signed) */
                int16_t result = (int16_t)(int8_t)(state->ax & 0xFF) * (int16_t)(int8_t)v;
                state->ax = (uint16_t)result;
                state->flags &= ~(DOS_FLAG_CF | DOS_FLAG_OF);
                int16_t check = (int16_t)(int8_t)(result & 0xFF);
                if (result != check) state->flags |= DOS_FLAG_CF | DOS_FLAG_OF;
                break;
            }
            case 6: { /* DIV r/m8 (unsigned) */
                if (v == 0) { dos_handle_int(state, 0); break; }
                uint16_t quotient = (uint16_t)state->ax / (uint16_t)v;
                if (quotient > 0xFF) { dos_handle_int(state, 0); break; }
                uint8_t remainder = (uint16_t)state->ax % (uint16_t)v;
                state->ax = ((uint16_t)remainder << 8) | (uint8_t)quotient;
                break;
            }
            case 7: { /* IDIV r/m8 (signed) */
                if (v == 0) { dos_handle_int(state, 0); break; }
                int16_t dividend = (int16_t)state->ax;
                int8_t divisor = (int8_t)v;
                int8_t quotient = dividend / divisor;
                int8_t remainder = dividend % divisor;
                state->ax = ((uint16_t)(uint8_t)remainder << 8) | (uint8_t)quotient;
                break;
            }
            }
            break;
        }
        case 0xF7: { /* r/m16 operation */
            struct modrm m = decode_modrm(state, 1);
            uint16_t v = read_rm(state, &m, 1);
            switch (m.reg) {
            case 0: { /* TEST r/m16, imm16 */
                uint16_t imm = fetch_word(state);
                uint16_t r = v & imm;
                set_flags_logic(state, r, 0);
                break;
            }
            case 2: { /* NOT r/m16 */
                write_rm(state, &m, 1, ~v);
                break;
            }
            case 3: { /* NEG r/m16 */
                uint16_t r = (uint16_t)(-(int32_t)v);
                write_rm(state, &m, 1, r);
                state->flags &= ~(DOS_FLAG_CF | DOS_FLAG_OF | DOS_FLAG_SF | DOS_FLAG_ZF | DOS_FLAG_AF | DOS_FLAG_PF);
                if (v != 0) state->flags |= DOS_FLAG_CF;
                if (v == 0x8000) state->flags |= DOS_FLAG_OF;
                if (r & 0x8000) state->flags |= DOS_FLAG_SF;
                if (r == 0) state->flags |= DOS_FLAG_ZF;
                if ((r & 0xF) != 0) state->flags |= DOS_FLAG_AF;
                set_pf(state, r & 0xFF);
                break;
            }
            case 4: { /* MUL r/m16 (unsigned) */
                uint32_t result = (uint32_t)state->ax * (uint32_t)v;
                state->dx = (uint16_t)(result >> 16);
                state->ax = (uint16_t)result;
                state->flags &= ~(DOS_FLAG_CF | DOS_FLAG_OF);
                if (state->dx != 0) state->flags |= DOS_FLAG_CF | DOS_FLAG_OF;
                break;
            }
            case 5: { /* IMUL r/m16 (signed) */
                int32_t result = (int32_t)(int16_t)state->ax * (int32_t)(int16_t)v;
                state->dx = (uint16_t)(result >> 16);
                state->ax = (uint16_t)result;
                state->flags &= ~(DOS_FLAG_CF | DOS_FLAG_OF);
                int32_t signext = (int32_t)(int16_t)(result & 0xFFFF);
                if (result != signext) state->flags |= DOS_FLAG_CF | DOS_FLAG_OF;
                break;
            }
            case 6: { /* DIV r/m16 (unsigned) */
                if (v == 0) { dos_handle_int(state, 0); break; }
                uint32_t dividend = ((uint32_t)state->dx << 16) | (uint32_t)state->ax;
                uint32_t quotient = dividend / (uint32_t)v;
                if (quotient > 0xFFFF) { dos_handle_int(state, 0); break; }
                uint16_t remainder = (uint16_t)(dividend % (uint32_t)v);
                state->ax = (uint16_t)quotient;
                state->dx = remainder;
                break;
            }
            case 7: { /* IDIV r/m16 (signed) */
                if (v == 0) { dos_handle_int(state, 0); break; }
                int32_t dividend = ((int32_t)state->dx << 16) | (int32_t)(int16_t)state->ax;
                int16_t divisor = (int16_t)v;
                int16_t quotient = dividend / divisor;
                int16_t remainder = dividend % divisor;
                state->ax = (uint16_t)quotient;
                state->dx = (uint16_t)remainder;
                break;
            }
            }
            break;
        }

        /* ---- 0xF8-0xFD: Flag instructions ---- */
        case 0xF8: /* CLC */
            state->flags &= ~DOS_FLAG_CF;
            break;
        case 0xF9: /* STC */
            state->flags |= DOS_FLAG_CF;
            break;
        case 0xFA: /* CLI */
            state->flags &= ~DOS_FLAG_IF;
            break;
        case 0xFB: /* STI */
            state->flags |= DOS_FLAG_IF;
            break;
        case 0xFC: /* CLD */
            state->flags &= ~DOS_FLAG_DF;
            break;
        case 0xFD: /* STD */
            state->flags |= DOS_FLAG_DF;
            break;

        default:
            break;
        }

        insn_count++;
        total_insns++;
        if (insn_count >= 1000) {
            scheduler_yield();
            insn_count = 0;
        }
        /* Hard limit to prevent hangs in case of emulation bugs */
        if (total_insns >= 1000000) {
            state->running = 0;
        }
        /* Time-based limit: 3 seconds real time (300 ticks at 100 Hz) */
        if ((timer_get_ticks() - start_tick) > 300) {
            state->running = 0;
        }
    }
}
