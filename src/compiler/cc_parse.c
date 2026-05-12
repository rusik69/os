/* cc_parse.c — Recursive-descent parser + x86-64 code emitter */

#include "cc.h"
#include "string.h"
#include "printf.h"

/* ------------------------------------------------------------------ */
/*  Error helpers                                                       */
/* ------------------------------------------------------------------ */

static void cc_error(CompilerState *cc, const char *msg) {
    cc->nerrors++;
    if (!cc->error) {
        cc->error = 1;
        int line = (cc->tok_pos < cc->ntokens) ? cc->tokens[cc->tok_pos].line : 0;
        if (line > 0) {
            /* prefix with line number: "line N: msg" */
            int i = 0;
            /* write "line " */
            const char *pre = "line "; while (*pre && i < 240) cc->errmsg[i++] = *pre++;
            /* write number */
            int tmp[10]; int tn = 0;
            int n = line; if (n <= 0) n = 1;
            do { tmp[tn++] = n % 10; n /= 10; } while (n > 0);
            while (tn > 0) cc->errmsg[i++] = (char)('0' + tmp[--tn]);
            cc->errmsg[i++] = ':'; cc->errmsg[i++] = ' ';
            while (*msg && i < 253) cc->errmsg[i++] = *msg++;
            cc->errmsg[i] = '\0';
        } else {
            strncpy(cc->errmsg, msg, sizeof(cc->errmsg) - 1);
        }
        cc->errmsg[sizeof(cc->errmsg) - 1] = '\0';
    }
}

static void cc_errorf(CompilerState *cc, const char *fmt, const char *arg) {
    if (!cc->error) {
        char buf[200];
        int i = 0, j = 0;
        while (fmt[i] && i < 180) {
            if (fmt[i] == '%' && fmt[i+1] == 's') {
                const char *a = arg;
                while (*a && j < 196) buf[j++] = *a++;
                i += 2;
            } else {
                buf[j++] = fmt[i++];
            }
        }
        buf[j] = '\0';
        cc_error(cc, buf);
    }
}

/* ------------------------------------------------------------------ */
/*  Token stream helpers                                                */
/* ------------------------------------------------------------------ */

static Token *cur(CompilerState *cc) { return &cc->tokens[cc->tok_pos]; }
static Token *peek1(CompilerState *cc) {
    int nx = cc->tok_pos + 1;
    if (nx >= cc->ntokens) nx = cc->ntokens - 1;
    return &cc->tokens[nx];
}
static Token *peek2(CompilerState *cc) {
    int nx = cc->tok_pos + 2;
    if (nx >= cc->ntokens) nx = cc->ntokens - 1;
    return &cc->tokens[nx];
}
static void advance(CompilerState *cc) {
    if (cc->tok_pos < cc->ntokens - 1) cc->tok_pos++;
}
static void expect(CompilerState *cc, TokenType t, const char *msg) {
    if (cur(cc)->type == t) advance(cc);
    else cc_error(cc, msg);
}

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static int emit_rel32_2_local(CompilerState *cc, uint8_t op0, uint8_t op1);

/* ------------------------------------------------------------------ */
/*  Code emission helpers                                              */
/* ------------------------------------------------------------------ */

static void emit1(CompilerState *cc, uint8_t b) {
    if (cc->code_len >= CC_CODE_MAX) { cc_error(cc, "code too large"); return; }
    cc->code[cc->code_len++] = b;
}
static void emit4(CompilerState *cc, uint32_t v) {
    emit1(cc, v & 0xff);
    emit1(cc, (v >> 8) & 0xff);
    emit1(cc, (v >> 16) & 0xff);
    emit1(cc, (v >> 24) & 0xff);
}
static void emit8(CompilerState *cc, uint64_t v) {
    emit4(cc, (uint32_t)(v & 0xffffffff));
    emit4(cc, (uint32_t)(v >> 32));
}
static void patch_rel32(CompilerState *cc, int off, int target) {
    int rel = target - (off + 4);
    cc->code[off]   = (uint8_t)(rel);
    cc->code[off+1] = (uint8_t)(rel >> 8);
    cc->code[off+2] = (uint8_t)(rel >> 16);
    cc->code[off+3] = (uint8_t)(rel >> 24);
}
static int emit_jmp(CompilerState *cc) {
    emit1(cc, 0xE9); int off = cc->code_len; emit4(cc, 0); return off;
}
static int emit_jz(CompilerState *cc) {
    emit1(cc, 0x0F); emit1(cc, 0x84); int off = cc->code_len; emit4(cc, 0); return off;
}
static int emit_jnz(CompilerState *cc) {
    emit1(cc, 0x0F); emit1(cc, 0x85); int off = cc->code_len; emit4(cc, 0); return off;
}
static int emit_call(CompilerState *cc) {
    emit1(cc, 0xE8); int off = cc->code_len; emit4(cc, 0); return off;
}

static void emit_push_rax(CompilerState *cc) { emit1(cc, 0x50); }
static void emit_pop_rcx(CompilerState *cc)  { emit1(cc, 0x59); }
static void emit_pop_rdx(CompilerState *cc)  { emit1(cc, 0x5A); }
static void emit_xor_rax(CompilerState *cc)  { emit1(cc, 0x31); emit1(cc, 0xC0); }

static void emit_mov_rax_imm64(CompilerState *cc, uint64_t v) {
    emit1(cc, 0x48); emit1(cc, 0xB8); emit8(cc, v);
}
static void emit_mov_rax_imm32(CompilerState *cc, int32_t v) {
    emit1(cc, 0x48); emit1(cc, 0xC7); emit1(cc, 0xC0); emit4(cc, (uint32_t)v);
}
/* test rax,rax */
static void emit_test_rax(CompilerState *cc) {
    emit1(cc, 0x48); emit1(cc, 0x85); emit1(cc, 0xC0);
}

/* Load/store 64-bit local */
static void emit_load_local(CompilerState *cc, int off) {
    if (off >= -128 && off <= 127) {
        emit1(cc, 0x48); emit1(cc, 0x8B); emit1(cc, 0x45); emit1(cc, (uint8_t)(int8_t)off);
    } else {
        emit1(cc, 0x48); emit1(cc, 0x8B); emit1(cc, 0x85); emit4(cc, (uint32_t)(int32_t)off);
    }
}
static void emit_store_local(CompilerState *cc, int off) {
    if (off >= -128 && off <= 127) {
        emit1(cc, 0x48); emit1(cc, 0x89); emit1(cc, 0x45); emit1(cc, (uint8_t)(int8_t)off);
    } else {
        emit1(cc, 0x48); emit1(cc, 0x89); emit1(cc, 0x85); emit4(cc, (uint32_t)(int32_t)off);
    }
}

/* Load/store 8-bit (char) local — zero-extends on load */
static void emit_load_local_byte(CompilerState *cc, int off) {
    if (off >= -128 && off <= 127) {
        emit1(cc, 0x0F); emit1(cc, 0xB6); emit1(cc, 0x45); emit1(cc, (uint8_t)(int8_t)off);
    } else {
        emit1(cc, 0x0F); emit1(cc, 0xB6); emit1(cc, 0x85); emit4(cc, (uint32_t)(int32_t)off);
    }
}
static void emit_store_local_byte(CompilerState *cc, int off) {
    if (off >= -128 && off <= 127) {
        emit1(cc, 0x88); emit1(cc, 0x45); emit1(cc, (uint8_t)(int8_t)off);
    } else {
        emit1(cc, 0x88); emit1(cc, 0x85); emit4(cc, (uint32_t)(int32_t)off);
    }
}

/* Global 64-bit via abs addr in rcx */
static void emit_load_global(CompilerState *cc, uint64_t addr) {
    emit1(cc, 0x48); emit1(cc, 0xB9); emit8(cc, addr); /* mov rcx,imm64 */
    emit1(cc, 0x48); emit1(cc, 0x8B); emit1(cc, 0x01); /* mov rax,[rcx] */
}
static void emit_store_global(CompilerState *cc, uint64_t addr) {
    emit1(cc, 0x48); emit1(cc, 0xB9); emit8(cc, addr); /* mov rcx,imm64 */
    emit1(cc, 0x48); emit1(cc, 0x89); emit1(cc, 0x01); /* mov [rcx],rax */
}
/* Global 8-bit (char) */
static void emit_load_global_byte(CompilerState *cc, uint64_t addr) {
    emit1(cc, 0x48); emit1(cc, 0xB9); emit8(cc, addr);
    emit1(cc, 0x0F); emit1(cc, 0xB6); emit1(cc, 0x01); /* movzx rax,byte[rcx] */
}
static void emit_store_global_byte(CompilerState *cc, uint64_t addr) {
    emit1(cc, 0x48); emit1(cc, 0xB9); emit8(cc, addr);
    emit1(cc, 0x88); emit1(cc, 0x01); /* mov byte[rcx],al */
}

/* Load address of local into rax */
static void emit_lea_local(CompilerState *cc, int off) {
    if (off >= -128 && off <= 127) {
        emit1(cc, 0x48); emit1(cc, 0x8D); emit1(cc, 0x45); emit1(cc, (uint8_t)(int8_t)off);
    } else {
        emit1(cc, 0x48); emit1(cc, 0x8D); emit1(cc, 0x85); emit4(cc, (uint32_t)(int32_t)off);
    }
}

/* Deref rax as ptr, load 64-bit */
static void emit_deref_rax(CompilerState *cc) {
    emit1(cc, 0x48); emit1(cc, 0x8B); emit1(cc, 0x00);
}
/* Deref rax as ptr, load 8-bit */
static void emit_deref_rax_byte(CompilerState *cc) {
    emit1(cc, 0x0F); emit1(cc, 0xB6); emit1(cc, 0x00);
}
/* Store rax via ptr in rcx (64-bit) */
static void emit_store_via_rcx(CompilerState *cc) {
    emit1(cc, 0x48); emit1(cc, 0x89); emit1(cc, 0x01);
}
/* Store al via ptr in rcx (8-bit) */
static void emit_store_via_rcx_byte(CompilerState *cc) {
    emit1(cc, 0x88); emit1(cc, 0x01);
}

/* Param register spilling/loading (System V AMD64) */
static void emit_param_to_rax(CompilerState *cc, int n) {
    switch (n) {
    case 0: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xF8); break; /* mov rax,rdi */
    case 1: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xF0); break; /* mov rax,rsi */
    case 2: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0); break; /* mov rax,rdx */
    case 3: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC8); break; /* mov rax,rcx */
    case 4: emit1(cc,0x4C);emit1(cc,0x89);emit1(cc,0xC0); break; /* mov rax,r8  */
    case 5: emit1(cc,0x4C);emit1(cc,0x89);emit1(cc,0xC8); break; /* mov rax,r9  */
    default: cc_error(cc, "too many params"); break;
    }
}

/* ------------------------------------------------------------------ */
/*  Type utilities                                                     */
/* ------------------------------------------------------------------ */

static int type_is_char(const TypeDesc *t) { return t->kind == TY_CHAR && t->ptr_depth == 0; }
static int type_sizeof(CompilerState *cc, const TypeDesc *t) {
    if (t->ptr_depth > 0) return 8;
    switch (t->kind) {
    case TY_CHAR:  return 1;
    case TY_VOID:  return 0;
    case TY_ARRAY:
        /* sizeof(array) = num_elements * 8 (default element size).
           We don't track the base type in TypeDesc for arrays, so assume 8-byte elements.
           char arrays use 1-byte elements but are sized at alloc time via local_frame. */
        return (t->arr_size > 0) ? t->arr_size * 8 : 8;
    case TY_STRUCT:
        if (t->struct_idx >= 0 && t->struct_idx < cc->nstructs)
            return cc->structs[t->struct_idx].total_size;
        return 0;
    default:       return 8;   /* int, long, ptr */
    }
}
static int type_elem_size(CompilerState *cc, const TypeDesc *t) {
    /* element size when subscripting an array/pointer */
    if (t->ptr_depth > 1) return 8; /* pointer to pointer — 8 bytes */
    if (t->ptr_depth == 1) {
        /* pointer to base type */
        TypeDesc elem = *t;
        elem.ptr_depth = 0;
        return type_sizeof(cc, &elem);
    }
    if (t->kind == TY_ARRAY) {
        TypeDesc elem = *t;
        elem.kind = TY_INT;
        return type_sizeof(cc, &elem);
    }
    return 8;
}

/* ------------------------------------------------------------------ */
/*  Data segment helpers                                               */
/* ------------------------------------------------------------------ */

static int data_add_string(CompilerState *cc, const char *s) {
    int off = cc->data_len;
    while (*s) {
        if (cc->data_len >= CC_DATA_MAX) { cc_error(cc, "data section full"); return 0; }
        cc->data[cc->data_len++] = (uint8_t)*s++;
    }
    if (cc->data_len >= CC_DATA_MAX) { cc_error(cc, "data section full"); return 0; }
    cc->data[cc->data_len++] = 0;
    return off;
}
static uint64_t data_vaddr(int off) {
    return CC_LOAD_BASE + CC_DATA_OFFSET + (uint64_t)off;
}

/* ------------------------------------------------------------------ */
/*  Symbol table                                                       */
/* ------------------------------------------------------------------ */

static Symbol *find_local(CompilerState *cc, const char *name) {
    for (int i = cc->nlocals - 1; i >= 0; i--)
        if (strcmp(cc->locals[i].name, name) == 0) return &cc->locals[i];
    return 0;
}
static Symbol *find_global(CompilerState *cc, const char *name) {
    for (int i = 0; i < cc->nglobals; i++)
        if (strcmp(cc->globals[i].name, name) == 0) return &cc->globals[i];
    return 0;
}
static Symbol *find_sym(CompilerState *cc, const char *name) {
    Symbol *s = find_local(cc, name);
    return s ? s : find_global(cc, name);
}
static FuncInfo *find_func(CompilerState *cc, const char *name) {
    for (int i = 0; i < cc->nfuncs; i++)
        if (strcmp(cc->funcs[i].name, name) == 0) return &cc->funcs[i];
    return 0;
}
static FuncInfo *declare_func(CompilerState *cc, const char *name, int nparams) {
    FuncInfo *f = find_func(cc, name);
    if (!f) {
        if (cc->nfuncs >= CC_MAX_FUNCS) { cc_error(cc, "too many functions"); return 0; }
        f = &cc->funcs[cc->nfuncs++];
        strncpy(f->name, name, 31);
        f->nparams = nparams;
        f->code_offset = -1;
        f->defined = 0;
    }
    return f;
}
static StructDef *find_struct(CompilerState *cc, const char *name) {
    for (int i = 0; i < cc->nstructs; i++)
        if (strcmp(cc->structs[i].name, name) == 0) return &cc->structs[i];
    return 0;
}

static void add_builtin_typedef(CompilerState *cc, const char *name, int kind, int ptr_depth) {
    if (cc->ntypedefs >= CC_MAX_TYPEDEFS) return;
    TypedefEntry *te = &cc->typedefs[cc->ntypedefs++];
    memset(te, 0, sizeof(*te));
    strncpy(te->name, name, sizeof(te->name) - 1);
    te->type.kind = kind;
    te->type.ptr_depth = ptr_depth;
    te->type.struct_idx = -1;
    te->type.arr_size = 0;
}

static void cc_seed_builtin_typedefs(CompilerState *cc) {
    /* Common aliases from types.h and standard C headers used in OS sources. */
    add_builtin_typedef(cc, "uint8_t", TY_CHAR, 0);
    add_builtin_typedef(cc, "int8_t", TY_CHAR, 0);
    add_builtin_typedef(cc, "uint16_t", TY_INT, 0);
    add_builtin_typedef(cc, "int16_t", TY_INT, 0);
    add_builtin_typedef(cc, "uint32_t", TY_INT, 0);
    add_builtin_typedef(cc, "int32_t", TY_INT, 0);
    add_builtin_typedef(cc, "uint64_t", TY_INT, 0);
    add_builtin_typedef(cc, "int64_t", TY_INT, 0);
    add_builtin_typedef(cc, "size_t", TY_INT, 0);
    add_builtin_typedef(cc, "ssize_t", TY_INT, 0);
    add_builtin_typedef(cc, "uintptr_t", TY_INT, 0);
    add_builtin_typedef(cc, "intptr_t", TY_INT, 0);
}

/* ------------------------------------------------------------------ */
/*  Type parsing                                                       */
/* ------------------------------------------------------------------ */

static int is_type_kw(TokenType t) {
    return t == TK_INT || t == TK_CHAR || t == TK_VOID ||
           t == TK_UNSIGNED || t == TK_LONG || t == TK_SHORT ||
           t == TK_STRUCT || t == TK_UNION || t == TK_CONST || t == TK_STATIC ||
           t == TK_EXTERN || t == TK_INLINE || t == TK_VOLATILE || t == TK_RESTRICT;
}

/* Returns 1 if cur token starts a type (keyword or typedef name) */
static int starts_type(CompilerState *cc) {
    if (is_type_kw(cur(cc)->type)) return 1;
    /* typedef name */
    if (cur(cc)->type == TK_IDENT) {
        for (int i = 0; i < cc->ntypedefs; i++)
            if (strcmp(cc->typedefs[i].name, cur(cc)->sval) == 0) return 1;
    }
    return 0;
}

/* Parse a type specifier + pointer stars. Fill *td. Returns elem size. */
static void parse_type(CompilerState *cc, TypeDesc *td) {
    td->kind      = TY_INT;
    td->ptr_depth = 0;
    td->struct_idx = -1;
    td->arr_size  = 0;

    /* qualifiers/storage that don't affect type */
    while (cur(cc)->type == TK_CONST || cur(cc)->type == TK_STATIC ||
           cur(cc)->type == TK_EXTERN || cur(cc)->type == TK_INLINE ||
           cur(cc)->type == TK_VOLATILE || cur(cc)->type == TK_RESTRICT)
        advance(cc);

    int is_unsigned = 0;
    if (cur(cc)->type == TK_UNSIGNED) { is_unsigned = 1; advance(cc); }

    if (cur(cc)->type == TK_INT || cur(cc)->type == TK_LONG) {
        td->kind = TY_INT; advance(cc);
        /* skip optional 'int' after 'long' */
        if (cur(cc)->type == TK_INT) advance(cc);
    } else if (cur(cc)->type == TK_SHORT) {
        td->kind = TY_INT; advance(cc);
        if (cur(cc)->type == TK_INT) advance(cc);
    } else if (cur(cc)->type == TK_CHAR) {
        td->kind = TY_CHAR; advance(cc);
    } else if (cur(cc)->type == TK_VOID) {
        td->kind = TY_VOID; advance(cc);
    } else if (cur(cc)->type == TK_STRUCT || cur(cc)->type == TK_UNION) {
        advance(cc);
        td->kind = TY_STRUCT;
        if (cur(cc)->type == TK_IDENT) {
            StructDef *sd = find_struct(cc, cur(cc)->sval);
            if (sd) td->struct_idx = (int)(sd - cc->structs);
            advance(cc);
        }
    } else if (cur(cc)->type == TK_IDENT) {
        /* Could be a typedef name */
        for (int i = 0; i < cc->ntypedefs; i++) {
            if (strcmp(cc->typedefs[i].name, cur(cc)->sval) == 0) {
                *td = cc->typedefs[i].type;
                advance(cc);
                goto done_base;
            }
        }
        /* Not a typedef — leave as TY_INT fallback */
    } else {
        /* Fallback for multi-word like 'unsigned long long' already handled */
    }
    (void)is_unsigned; /* unsigned just changes signedness; we use 64-bit everywhere */
done_base:
    /* consume const after type */
    if (cur(cc)->type == TK_CONST) advance(cc);
    /* pointer stars */
    while (cur(cc)->type == TK_STAR) { td->ptr_depth++; advance(cc); }
}

/* ------------------------------------------------------------------ */
/*  Local variable allocation                                         */
/* ------------------------------------------------------------------ */

static int alloc_local(CompilerState *cc, const char *name, TypeDesc *td, int arr_n) {
    if (cc->nlocals >= CC_MAX_LOCALS) { cc_error(cc, "too many locals"); return 0; }
    int esz = (arr_n > 0) ? arr_n * type_sizeof(cc, td) : type_sizeof(cc, td);
    if (esz < 8) esz = 8; /* always align to at least 8 */
    /* align to esz */
    cc->local_frame += esz;
    int off = -(int)cc->local_frame;
    Symbol *s = &cc->locals[cc->nlocals++];
    strncpy(s->name, name, 31);
    s->type = *td;
    if (arr_n > 0) { s->type.kind = TY_ARRAY; s->type.arr_size = arr_n; }
    s->offset    = off;
    s->is_global = 0;
    s->is_param  = 0;
    s->gaddr     = 0;
    return off;
}

/* ------------------------------------------------------------------ */
/*  Compare + setcc helper                                             */
/* ------------------------------------------------------------------ */

static void emit_cmp_setcc(CompilerState *cc, uint8_t op) {
    emit_pop_rcx(cc);
    emit1(cc,0x48);emit1(cc,0x39);emit1(cc,0xC1); /* cmp rcx,rax */
    emit1(cc,0x0F);emit1(cc,op);emit1(cc,0xC0);    /* setXX al */
    emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xB6);emit1(cc,0xC0); /* movzx rax,al */
}

/* ------------------------------------------------------------------ */
/*  Emit load/store of a symbol by name (sets *td to its type)        */
/* ------------------------------------------------------------------ */

static void emit_sym_load(CompilerState *cc, const char *name, TypeDesc *td_out) {
    Symbol *s = find_sym(cc, name);
    if (!s) { cc_errorf(cc, "undefined: %s", name); return; }
    if (td_out) *td_out = s->type;
    if (s->is_global) {
        if (type_is_char(&s->type))
            emit_load_global_byte(cc, s->gaddr);
        else
            emit_load_global(cc, s->gaddr);
    } else {
        if (type_is_char(&s->type))
            emit_load_local_byte(cc, s->offset);
        else
            emit_load_local(cc, s->offset);
    }
}

static void emit_sym_store(CompilerState *cc, const char *name) {
    Symbol *s = find_sym(cc, name);
    if (!s) { cc_errorf(cc, "undefined: %s", name); return; }
    if (s->is_global) {
        if (type_is_char(&s->type)) emit_store_global_byte(cc, s->gaddr);
        else                        emit_store_global(cc, s->gaddr);
    } else {
        if (type_is_char(&s->type)) emit_store_local_byte(cc, s->offset);
        else                        emit_store_local(cc, s->offset);
    }
}

/* ------------------------------------------------------------------ */
/*  Expression parser                                                  */
/* ------------------------------------------------------------------ */

static void parse_expr(CompilerState *cc);
static void parse_expr_prec(CompilerState *cc, int prec);
static void parse_stmt(CompilerState *cc,
                       int *break_p, int *nbr,
                       int *cont_p,  int *nco);

static void parse_primary(CompilerState *cc) {
    Token *t = cur(cc);
    if (cc->error) return;

    /* integer / char literal */
    if (t->type == TK_INTLIT || t->type == TK_CHARLIT) {
        advance(cc);
        if (t->ival >= -2147483648LL && t->ival <= 2147483647LL)
            emit_mov_rax_imm32(cc, (int32_t)t->ival);
        else
            emit_mov_rax_imm64(cc, (uint64_t)t->ival);
        return;
    }

    /* string literal — concatenate adjacent string literals */
    if (t->type == TK_STRLIT) {
        char merged[256]; int mlen = 0;
        while (cur(cc)->type == TK_STRLIT) {
            const char *sv = cur(cc)->sval;
            advance(cc);
            while (*sv && mlen < 255) merged[mlen++] = *sv++;
        }
        merged[mlen] = '\0';
        int doff = data_add_string(cc, merged);
        emit_mov_rax_imm64(cc, data_vaddr(doff));
        return;
    }

    /* sizeof */
    if (t->type == TK_SIZEOF) {
        advance(cc);
        int sz = 8;
        if (cur(cc)->type == TK_LPAREN) {
            advance(cc);
            if (starts_type(cc)) {
                TypeDesc td; parse_type(cc, &td);
                sz = type_sizeof(cc, &td);
            } else {
                /* sizeof(expr) — check if single identifier for accurate sizing */
                if (cur(cc)->type == TK_IDENT && peek1(cc)->type == TK_RPAREN) {
                    Symbol *ss = find_sym(cc, cur(cc)->sval);
                    if (ss) sz = type_sizeof(cc, &ss->type);
                    advance(cc);
                } else {
                    int depth = 1;
                    while (cur(cc)->type != TK_EOF && depth > 0) {
                        if (cur(cc)->type == TK_LPAREN) depth++;
                        else if (cur(cc)->type == TK_RPAREN) depth--;
                        if (depth > 0) advance(cc);
                    }
                }
            }
            expect(cc, TK_RPAREN, "expected ')' after sizeof");
        } else if (cur(cc)->type == TK_IDENT) {
            Symbol *s = find_sym(cc, cur(cc)->sval);
            if (s) sz = type_sizeof(cc, &s->type);
            advance(cc);
        }
        emit_mov_rax_imm32(cc, sz);
        return;
    }

    /* parenthesised expression or cast */
    if (t->type == TK_LPAREN) {
        advance(cc);
        /* cast: (type)expr */
        if (starts_type(cc)) {
            TypeDesc cast_td;
            parse_type(cc, &cast_td);
            expect(cc, TK_RPAREN, "expected ')' after cast type");
            parse_primary(cc); /* value in rax */
            /* For char cast: truncate to 8-bit */
            if (type_is_char(&cast_td)) {
                emit1(cc,0x0F);emit1(cc,0xB6);emit1(cc,0xC0); /* movzx eax,al */
            }
            return;
        }
        parse_expr(cc);
        expect(cc, TK_RPAREN, "expected ')'");
        return;
    }

    /* unary - */
    if (t->type == TK_MINUS) {
        advance(cc);
        parse_primary(cc);
        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xD8); /* neg rax */
        return;
    }
    /* unary ! */
    if (t->type == TK_NOT) {
        advance(cc);
        parse_primary(cc);
        emit_test_rax(cc);
        emit1(cc,0x0F);emit1(cc,0x94);emit1(cc,0xC0); /* setz al */
        emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xB6);emit1(cc,0xC0);
        return;
    }
    /* unary ~ */
    if (t->type == TK_TILDE) {
        advance(cc);
        parse_primary(cc);
        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xD0); /* not rax */
        return;
    }
    /* address-of &var */
    if (t->type == TK_AMP) {
        advance(cc);
        if (cur(cc)->type != TK_IDENT) { cc_error(cc, "expected lvalue after &"); return; }
        const char *nm = cur(cc)->sval; advance(cc);
        Symbol *s = find_sym(cc, nm);
        if (!s) { cc_errorf(cc, "undefined: %s", nm); return; }
        if (s->is_global)
            emit_mov_rax_imm64(cc, s->gaddr);
        else
            emit_lea_local(cc, s->offset);
        return;
    }
    /* deref *ptr */
    if (t->type == TK_STAR) {
        advance(cc);
        parse_primary(cc);
        /* We don't track type here precisely — assume 64-bit deref */
        emit_deref_rax(cc);
        return;
    }
    /* prefix ++ / -- */
    if (t->type == TK_INC || t->type == TK_DEC) {
        int is_inc = (t->type == TK_INC); advance(cc);
        if (cur(cc)->type != TK_IDENT) { cc_error(cc, "expected lvalue"); return; }
        const char *nm = cur(cc)->sval; advance(cc);
        emit_sym_load(cc, nm, 0);
        if (is_inc) { emit1(cc,0x48);emit1(cc,0xFF);emit1(cc,0xC0); }
        else        { emit1(cc,0x48);emit1(cc,0xFF);emit1(cc,0xC8); }
        emit_sym_store(cc, nm);
        return;
    }

    /* identifier */
    if (t->type == TK_IDENT) {
        char name[64]; strncpy(name, t->sval, 63); name[63]=0;
        advance(cc);

        /* ---- __syscall(nr, a1..a5) builtin ---- */
        if (strcmp(name, "__syscall") == 0) {
            expect(cc, TK_LPAREN, "expected '(' after __syscall");
            /* Collect up to 6 arguments */
            int nargs = 0;
            while (cur(cc)->type != TK_RPAREN && !cc->error) {
                parse_expr(cc);
                emit_push_rax(cc);
                nargs++;
                if (cur(cc)->type == TK_COMMA) advance(cc);
            }
            expect(cc, TK_RPAREN, "expected ')' after __syscall args");
            /* Pop args in reverse into syscall registers:
               SysV: rax=nr, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6 */
            static const uint8_t load_reg[6][3] = {
                {0x48,0x89,0xC7}, /* pop rcx; mov rdi,rcx */
                {0x48,0x89,0xCE}, /* mov rsi,rcx */
                {0x48,0x89,0xCA}, /* mov rdx,rcx */
                {0x49,0x89,0xCA}, /* mov r10,rcx */
                {0x49,0x89,0xC8}, /* mov r8,rcx */
                {0x49,0x89,0xC9}, /* mov r9,rcx */
            };
            /* pop syscall number last (first arg) */
            /* We push left-to-right, so top of stack = last arg */
            for (int ai = nargs - 1; ai >= 0; ai--) {
                emit_pop_rcx(cc); /* rcx = arg[ai] */
                if (ai == 0) {
                    /* syscall number goes in rax */
                    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC8); /* mov rax,rcx */
                } else if (ai - 1 < 6) {
                    int reg_idx = ai - 1; /* a1=0=rdi, a2=1=rsi, ... */
                    emit1(cc, load_reg[reg_idx][0]);
                    emit1(cc, load_reg[reg_idx][1]);
                    emit1(cc, load_reg[reg_idx][2]);
                }
            }
            emit1(cc, 0x0F); emit1(cc, 0x05); /* syscall */
            return;
        }

        /* function call or function pointer call */
        if (cur(cc)->type == TK_LPAREN) {
            /* Check for function pointer variable (not a known function) */
            FuncInfo *f = find_func(cc, name);
            Symbol *fptr = (!f) ? find_sym(cc, name) : 0;
            int is_fptr = (fptr && fptr->type.ptr_depth > 0);

            if (is_fptr) {
                /* Load function pointer early, push on stack */
                if (fptr->is_global) emit_load_global(cc, fptr->gaddr);
                else                 emit_load_local(cc, fptr->offset);
                emit_push_rax(cc);
            }

            advance(cc);
            int nargs = 0;
            while (cur(cc)->type != TK_RPAREN && !cc->error) {
                parse_expr(cc);
                emit_push_rax(cc);
                nargs++;
                if (cur(cc)->type == TK_COMMA) advance(cc);
            }
            expect(cc, TK_RPAREN, "expected ')'");
            /* pop args into SysV regs (in reverse) */
            for (int a = nargs-1; a >= 0; a--) {
                emit_pop_rcx(cc);
                switch (a) {
                case 0: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xCF); break; /* mov rdi,rcx */
                case 1: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xCE); break; /* mov rsi,rcx */
                case 2: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xCA); break; /* mov rdx,rcx */
                case 3: emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC9); break; /* mov rcx,rcx */
                case 4: emit1(cc,0x49);emit1(cc,0x89);emit1(cc,0xC8); break; /* mov r8,rcx  */
                case 5: emit1(cc,0x49);emit1(cc,0x89);emit1(cc,0xC9); break; /* mov r9,rcx  */
                default: break;
                }
            }

            if (is_fptr) {
                /* Indirect call through function pointer */
                emit1(cc, 0x58);                /* pop rax (function ptr) */
                emit1(cc, 0xFF); emit1(cc, 0xD0); /* call rax */
            } else {
                if (!f) f = declare_func(cc, name, nargs);
                if (f && f->defined) {
                    int ro = emit_call(cc);
                    patch_rel32(cc, ro, f->code_offset);
                } else {
                    if (cc->npatches >= CC_MAX_PATCHES) { cc_error(cc, "too many patches"); return; }
                    Patch *p = &cc->patches[cc->npatches++];
                    p->code_off = emit_call(cc);
                    strncpy(p->name, name, 31);
                }
            }
            return;
        }

        /* suffix ++ / -- */
        if (cur(cc)->type == TK_INC || cur(cc)->type == TK_DEC) {
            int is_inc = (cur(cc)->type == TK_INC); advance(cc);
            emit_sym_load(cc, name, 0);
            emit_push_rax(cc);  /* save old value */
            if (is_inc) { emit1(cc,0x48);emit1(cc,0xFF);emit1(cc,0xC0); }
            else        { emit1(cc,0x48);emit1(cc,0xFF);emit1(cc,0xC8); }
            emit_sym_store(cc, name);
            emit_pop_rcx(cc);
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC8); /* mov rax,rcx */
            return;
        }

        /* array subscript */
        if (cur(cc)->type == TK_LBRACKET) {
            advance(cc);
            Symbol *s = find_sym(cc, name);
            if (!s) { cc_errorf(cc, "undefined: %s", name); return; }
            int esz = type_elem_size(cc, &s->type);
            /* base address */
            if (s->is_global)
                emit_mov_rax_imm64(cc, s->gaddr);
            else
                emit_lea_local(cc, s->offset);
            emit_push_rax(cc);
            parse_expr(cc); /* index in rax */
            if (esz > 1) {
                /* imul rax,rax,esz — scale index by element size */
                emit1(cc,0x48);emit1(cc,0x69);emit1(cc,0xC0); emit4(cc,(uint32_t)esz);
            }
            emit_pop_rcx(cc); /* rcx = base */
            emit1(cc,0x48);emit1(cc,0x01);emit1(cc,0xC8); /* add rax,rcx */
            /* load element */
            if (esz == 1) emit_deref_rax_byte(cc);
            else          emit_deref_rax(cc);
            expect(cc, TK_RBRACKET, "expected ']'");
            return;
        }

        /* struct member access: s.field */
        if (cur(cc)->type == TK_DOT) {
            advance(cc);
            Symbol *s = find_sym(cc, name);
            if (!s) { cc_errorf(cc, "undefined: %s", name); return; }
            if (cur(cc)->type != TK_IDENT) { cc_error(cc, "expected field name"); return; }
            const char *field = cur(cc)->sval; advance(cc);
            /* get struct def */
            int sidx = s->type.struct_idx;
            if (sidx < 0 || sidx >= cc->nstructs) { cc_error(cc, "not a struct"); return; }
            StructDef *sd = &cc->structs[sidx];
            for (int fi = 0; fi < sd->nfields; fi++) {
                if (strcmp(sd->fields[fi].name, field) == 0) {
                    int foff = sd->fields[fi].offset;
                    /* compute address of field */
                    if (s->is_global)
                        emit_mov_rax_imm64(cc, s->gaddr + (uint64_t)foff);
                    else
                        emit_lea_local(cc, s->offset + foff);
                    int fsz = type_sizeof(cc, &sd->fields[fi].type);
                    if (fsz == 1) emit_deref_rax_byte(cc);
                    else          emit_deref_rax(cc);
                    return;
                }
            }
            cc_error(cc, "unknown struct field");
            return;
        }

        /* struct member via pointer: p->field */
        if (cur(cc)->type == TK_ARROW) {
            advance(cc);
            Symbol *s = find_sym(cc, name);
            if (!s) { cc_errorf(cc, "undefined: %s", name); return; }
            /* load pointer value */
            if (s->is_global) emit_load_global(cc, s->gaddr);
            else              emit_load_local(cc, s->offset);
            if (cur(cc)->type != TK_IDENT) { cc_error(cc, "expected field name"); return; }
            const char *field = cur(cc)->sval; advance(cc);
            /* find field offset from pointed-to struct */
            int sidx = s->type.struct_idx;
            if (s->type.ptr_depth >= 1 && sidx >= 0 && sidx < cc->nstructs) {
                StructDef *sd = &cc->structs[sidx];
                for (int fi = 0; fi < sd->nfields; fi++) {
                    if (strcmp(sd->fields[fi].name, field) == 0) {
                        int foff = sd->fields[fi].offset;
                        if (foff != 0) {
                            /* rax holds pointer; add field offset */
                            emit1(cc,0x48);emit1(cc,0x05); emit4(cc,(uint32_t)foff); /* add rax,foff */
                        }
                        int fsz = type_sizeof(cc, &sd->fields[fi].type);
                        if (fsz == 1) emit_deref_rax_byte(cc);
                        else          emit_deref_rax(cc);
                        return;
                    }
                }
                cc_error(cc, "unknown field in ->"); return;
            }
            cc_error(cc, "not a struct pointer for ->"); return;
        }

        /* plain variable load */
        TypeDesc td; emit_sym_load(cc, name, &td);
        return;
    }

    cc_error(cc, "unexpected token in expression");
}

/* ------------------------------------------------------------------ */
/*  Assignment detection: IDENT op= or arr[i] op= or *ptr op=         */
/* ------------------------------------------------------------------ */

static int is_assign_op(TokenType t) {
    return t==TK_ASSIGN || t==TK_PLUSEQ || t==TK_MINUSEQ ||
           t==TK_STAREQ || t==TK_SLASHEQ || t==TK_PERCENTEQ ||
           t==TK_AMPEQ || t==TK_PIPEEQ || t==TK_CARETEQ ||
           t==TK_LSHIFTEQ || t==TK_RSHIFTEQ;
}

/* ------------------------------------------------------------------ */
/*  Full expression parser                                             */
/* ------------------------------------------------------------------ */

static void parse_expr_prec(CompilerState *cc, int prec) {
    if (cc->error) return;

    if (prec >= 11) { parse_primary(cc); return; }

    /* Assignment level (prec 0, right-associative) */
    if (prec == 0) {
        /* Peek for assignment: IDENT [subscript] assign-op */
        Token *t = cur(cc);
        /* *ptr = */
        if (t->type == TK_STAR && peek1(cc)->type == TK_IDENT &&
            is_assign_op(peek2(cc)->type)) {
            advance(cc); /* skip * */
            const char *nm = cur(cc)->sval; advance(cc);
            TokenType op = cur(cc)->type; advance(cc);
            Symbol *s = find_sym(cc, nm);
            if (!s) { cc_errorf(cc, "undefined: %s", nm); return; }
            emit_load_local(cc, s->offset); /* ptr val in rax */
            emit_push_rax(cc);              /* push addr */
            parse_expr_prec(cc, 0);         /* rhs in rax */
            emit_pop_rcx(cc);               /* rcx = addr */
            if (op != TK_ASSIGN) {
                /* load old value via rcx, apply op, result in rax */
                emit_push_rax(cc);                             /* push rhs */
                emit1(cc,0x48);emit1(cc,0x8B);emit1(cc,0x01); /* mov rax,[rcx] (old value) */
                emit_pop_rdx(cc);                              /* rdx = rhs */
                switch(op) {
                case TK_PLUSEQ:  emit1(cc,0x48);emit1(cc,0x01);emit1(cc,0xD0); break; /* add rax,rdx */
                case TK_MINUSEQ: emit1(cc,0x48);emit1(cc,0x29);emit1(cc,0xD0); break; /* sub rax,rdx */
                case TK_STAREQ:  emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xAF);emit1(cc,0xC2); break; /* imul rax,rdx */
                case TK_SLASHEQ:
                    emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xD0); /* xchg rax,rdx */
                    emit1(cc,0x48);emit1(cc,0x99);                 /* cqo */
                    emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xFA); /* idiv rdx */
                    break;
                case TK_PERCENTEQ:
                    emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xD0); /* xchg rax,rdx */
                    emit1(cc,0x48);emit1(cc,0x99);
                    emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xFA);
                    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0); /* mov rax,rdx */
                    break;
                case TK_AMPEQ:   emit1(cc,0x48);emit1(cc,0x21);emit1(cc,0xD0); break; /* and rax,rdx */
                case TK_PIPEEQ:  emit1(cc,0x48);emit1(cc,0x09);emit1(cc,0xD0); break; /* or  rax,rdx */
                case TK_CARETEQ: emit1(cc,0x48);emit1(cc,0x31);emit1(cc,0xD0); break; /* xor rax,rdx */
                case TK_LSHIFTEQ:
                    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD1); /* mov rcx,rdx (count) */
                    emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xE0); /* shl rax,cl */
                    break;
                case TK_RSHIFTEQ:
                    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD1);
                    emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xF8); /* sar rax,cl */
                    break;
                default: break;
                }
            }
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0x01); /* mov [rcx],rax */
            return;
        }
        if (t->type == TK_IDENT) {
            char name[64]; strncpy(name, t->sval, 63);
            Token *nx = peek1(cc);
            /* name[i] op= rhs */
            if (nx->type == TK_LBRACKET) {
                /* Can't easily peek further; fall through to expression parsing */
            } else if (nx->type == TK_ARROW || nx->type == TK_DOT) {
                /* name->field = rhs  OR  name.field = rhs */
                int is_arrow = (nx->type == TK_ARROW);
                Token *nx2 = peek2(cc);
                if (nx2->type == TK_IDENT) {
                    /* Save position, check if field is followed by assign op */
                    int sp = cc->tok_pos;
                    advance(cc); advance(cc); advance(cc); /* skip name, ->|., field */
                    if (is_assign_op(cur(cc)->type)) {
                        char fname2[64]; strncpy(fname2, nx2->sval, 63);
                        TokenType op = cur(cc)->type; advance(cc);
                        parse_expr_prec(cc, 0); /* rhs in rax */
                        Symbol *s = find_sym(cc, name);
                        if (!s) { cc_errorf(cc, "undefined: %s", name); return; }
                        int sidx = s->type.struct_idx;
                        if (sidx < 0 || sidx >= cc->nstructs) { cc_error(cc, "not a struct"); return; }
                        StructDef *sd = &cc->structs[sidx];
                        for (int fi = 0; fi < sd->nfields; fi++) {
                            if (strcmp(sd->fields[fi].name, fname2) == 0) {
                                int foff = sd->fields[fi].offset;
                                int ffsz = type_sizeof(cc, &sd->fields[fi].type);
                                /* rax = rhs; compute address of field into rcx */
                                emit_push_rax(cc); /* save rhs */
                                if (is_arrow) {
                                    /* load pointer, add field offset */
                                    if (s->is_global) emit_load_global(cc, s->gaddr);
                                    else              emit_load_local(cc, s->offset);
                                    if (foff != 0) {
                                        emit1(cc,0x48);emit1(cc,0x05); emit4(cc,(uint32_t)foff);
                                    }
                                } else {
                                    /* direct struct: lea of base + offset */
                                    if (s->is_global)
                                        emit_mov_rax_imm64(cc, s->gaddr + (uint64_t)foff);
                                    else
                                        emit_lea_local(cc, s->offset + foff);
                                }
                                emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC1); /* mov rcx,rax (addr) */
                                emit1(cc, 0x58); /* pop rax (rhs) */
                                if (op != TK_ASSIGN) {
                                    /* compound: load old from [rcx], apply op */
                                    emit_push_rax(cc); /* push rhs */
                                    if (ffsz == 1) { emit1(cc,0x0F);emit1(cc,0xB6);emit1(cc,0x01); }
                                    else           { emit1(cc,0x48);emit1(cc,0x8B);emit1(cc,0x01); }
                                    emit_pop_rdx(cc); /* rdx = rhs */
                                    switch(op) {
                                    case TK_PLUSEQ:  emit1(cc,0x48);emit1(cc,0x01);emit1(cc,0xD0); break; /* add rax,rdx */
                                    case TK_MINUSEQ: emit1(cc,0x48);emit1(cc,0x29);emit1(cc,0xD0); break; /* sub rax,rdx */
                                    case TK_STAREQ:  emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xAF);emit1(cc,0xC2); break;
                                    case TK_SLASHEQ:
                                        emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xD0);
                                        emit1(cc,0x48);emit1(cc,0x99);
                                        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xFA);
                                        break;
                                    case TK_PERCENTEQ:
                                        emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xD0);
                                        emit1(cc,0x48);emit1(cc,0x99);
                                        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xFA);
                                        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0);
                                        break;
                                    case TK_AMPEQ:   emit1(cc,0x48);emit1(cc,0x21);emit1(cc,0xD0); break;
                                    case TK_PIPEEQ:  emit1(cc,0x48);emit1(cc,0x09);emit1(cc,0xD0); break;
                                    case TK_CARETEQ: emit1(cc,0x48);emit1(cc,0x31);emit1(cc,0xD0); break;
                                    case TK_LSHIFTEQ:
                                        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD1);
                                        emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xE0);
                                        break;
                                    case TK_RSHIFTEQ:
                                        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD1);
                                        emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xF8);
                                        break;
                                    default: break;
                                    }
                                }
                                if (ffsz == 1) { emit1(cc,0x88);emit1(cc,0x01); }
                                else           { emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0x01); }
                                return;
                            }
                        }
                        cc_error(cc, "unknown struct field");
                        return;
                    }
                    cc->tok_pos = sp; /* not an assignment, restore */
                }
            } else if (is_assign_op(nx->type)) {
                advance(cc); /* skip name */
                TokenType op = cur(cc)->type; advance(cc);
                parse_expr_prec(cc, 0); /* rhs in rax */
                Symbol *s = find_sym(cc, name);
                if (!s) { cc_errorf(cc, "undefined lvalue: %s", name); return; }
                if (op != TK_ASSIGN) {
                    /* compound: load old, apply, store */
                    emit_push_rax(cc);
                    if (s->is_global) {
                        if (type_is_char(&s->type)) emit_load_global_byte(cc, s->gaddr);
                        else                        emit_load_global(cc, s->gaddr);
                    } else {
                        if (type_is_char(&s->type)) emit_load_local_byte(cc, s->offset);
                        else                        emit_load_local(cc, s->offset);
                    }
                    emit_pop_rcx(cc); /* rcx = rhs */
                    switch (op) {
                    case TK_PLUSEQ:    emit1(cc,0x48);emit1(cc,0x01);emit1(cc,0xC8); break;
                    case TK_MINUSEQ:   emit1(cc,0x48);emit1(cc,0x29);emit1(cc,0xC8); break;
                    case TK_STAREQ:    emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xAF);emit1(cc,0xC1); break;
                    case TK_SLASHEQ:
                        emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xC1);
                        emit1(cc,0x48);emit1(cc,0x99);
                        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xF9);
                        break;
                    case TK_PERCENTEQ:
                        emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xC1);
                        emit1(cc,0x48);emit1(cc,0x99);
                        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xF9);
                        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0);
                        break;
                    case TK_AMPEQ:     emit1(cc,0x48);emit1(cc,0x21);emit1(cc,0xC8); break;
                    case TK_PIPEEQ:    emit1(cc,0x48);emit1(cc,0x09);emit1(cc,0xC8); break;
                    case TK_CARETEQ:   emit1(cc,0x48);emit1(cc,0x31);emit1(cc,0xC8); break;
                    case TK_LSHIFTEQ:
                        /* rax=old, rcx=rhs(count); shl rax,cl */
                        emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xE0); break;
                    case TK_RSHIFTEQ:
                        emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xF8); break;
                    default: break;
                    }
                }
                emit_sym_store(cc, name);
                return;
            }
            /* array assign: name[expr] = rhs */
            if (nx->type == TK_LBRACKET) {
                /* Look further ahead: can't easily detect without tracking depth.
                   We'll handle it here by calling emit_lvalue_addr. */
                int saved = cc->tok_pos;
                int is_byte = 0;
                /* save code position too to detect if addr emit works */
                int saved_code = cc->code_len;
                (void)saved; (void)saved_code;
                /* Emit address of lhs into rax, then push */
                advance(cc); advance(cc); /* skip name, [ */
                Symbol *s2 = find_sym(cc, name);
                if (!s2) { cc_errorf(cc, "undefined: %s", name); return; }
                int esz = type_elem_size(cc, &s2->type);
                if (esz == 1) is_byte = 1;
                if (s2->is_global) emit_mov_rax_imm64(cc, s2->gaddr);
                else               emit_lea_local(cc, s2->offset);
                emit_push_rax(cc);
                parse_expr(cc); /* index */
                if (esz > 1) {
                    emit1(cc,0x48);emit1(cc,0x69);emit1(cc,0xC0); emit4(cc,(uint32_t)esz);
                }
                emit_pop_rcx(cc);
                emit1(cc,0x48);emit1(cc,0x01);emit1(cc,0xC8); /* rax = addr */
                expect(cc, TK_RBRACKET, "expected ']'");
                if (!is_assign_op(cur(cc)->type)) { cc_error(cc, "expected assignment"); return; }
                TokenType op = cur(cc)->type; advance(cc);
                emit_push_rax(cc); /* push addr */
                parse_expr_prec(cc, 0); /* rhs in rax */
                emit_pop_rcx(cc); /* rcx = addr */
                if (op != TK_ASSIGN) {
                    emit_push_rax(cc);
                    if (is_byte) { emit1(cc,0x0F);emit1(cc,0xB6);emit1(cc,0x01); } /* movzx rax,byte[rcx] */
                    else         { emit1(cc,0x48);emit1(cc,0x8B);emit1(cc,0x01); } /* mov rax,[rcx] */
                    emit_pop_rdx(cc);
                    switch(op) {
                    case TK_PLUSEQ:  emit1(cc,0x48);emit1(cc,0x01);emit1(cc,0xD0); break;
                    case TK_MINUSEQ: emit1(cc,0x48);emit1(cc,0x29);emit1(cc,0xD0); break;
                    case TK_STAREQ:  emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xAF);emit1(cc,0xC2); break;
                    case TK_SLASHEQ:
                        emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xD0);
                        emit1(cc,0x48);emit1(cc,0x99);
                        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xFA);
                        break;
                    case TK_PERCENTEQ:
                        emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xD0);
                        emit1(cc,0x48);emit1(cc,0x99);
                        emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xFA);
                        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0);
                        break;
                    case TK_AMPEQ:   emit1(cc,0x48);emit1(cc,0x21);emit1(cc,0xD0); break;
                    case TK_PIPEEQ:  emit1(cc,0x48);emit1(cc,0x09);emit1(cc,0xD0); break;
                    case TK_CARETEQ: emit1(cc,0x48);emit1(cc,0x31);emit1(cc,0xD0); break;
                    case TK_LSHIFTEQ:
                        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD1);
                        emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xE0);
                        break;
                    case TK_RSHIFTEQ:
                        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD1);
                        emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xF8);
                        break;
                    default: break;
                    }
                }
                if (is_byte) { emit1(cc,0x88);emit1(cc,0x01); }
                else         { emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0x01); }
                return;
            }
        }
        /* ternary ? : handled at prec 1 after lhs */
        parse_expr_prec(cc, 1);
        /* ternary */
        if (cur(cc)->type == TK_QUESTION) {
            advance(cc);
            emit_test_rax(cc);
            int jz = emit_jz(cc);
            parse_expr(cc);
            expect(cc, TK_COLON, "expected ':' in ternary");
            int jmp = emit_jmp(cc);
            patch_rel32(cc, jz, cc->code_len);
            parse_expr_prec(cc, 0);
            patch_rel32(cc, jmp, cc->code_len);
        }
        return;
    }

    parse_expr_prec(cc, prec + 1);

    while (!cc->error) {
        TokenType op = cur(cc)->type;
        int mp = -1;
        if (prec==1 && op==TK_OR)  mp=1;
        if (prec==2 && op==TK_AND) mp=2;
        if (prec==3 && op==TK_PIPE)  mp=3;
        if (prec==4 && op==TK_CARET) mp=4;
        if (prec==5 && op==TK_AMP)   mp=5;
        if (prec==6 && (op==TK_EQ||op==TK_NEQ)) mp=6;
        if (prec==7 && (op==TK_LT||op==TK_GT||op==TK_LEQ||op==TK_GEQ)) mp=7;
        if (prec==8 && (op==TK_LSHIFT||op==TK_RSHIFT)) mp=8;
        if (prec==9 && (op==TK_PLUS||op==TK_MINUS)) mp=9;
        if (prec==10 && (op==TK_STAR||op==TK_SLASH||op==TK_PERCENT)) mp=10;
        if (mp < 0) break;
        advance(cc);

        /* Short-circuit || */
        if (op == TK_OR) {
            emit_test_rax(cc);
            int jnz = emit_jnz(cc);
            parse_expr_prec(cc, prec+1);
            emit_test_rax(cc);
            patch_rel32(cc, jnz, cc->code_len);
            emit1(cc,0x0F);emit1(cc,0x95);emit1(cc,0xC0); /* setnz al */
            emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xB6);emit1(cc,0xC0);
            return;
        }
        /* Short-circuit && */
        if (op == TK_AND) {
            emit_test_rax(cc);
            int jz = emit_jz(cc);
            parse_expr_prec(cc, prec+1);
            emit_test_rax(cc);
            patch_rel32(cc, jz, cc->code_len);
            emit1(cc,0x0F);emit1(cc,0x95);emit1(cc,0xC0); /* setnz al */
            emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xB6);emit1(cc,0xC0);
            return;
        }

        emit_push_rax(cc);
        parse_expr_prec(cc, prec+1);

        switch (op) {
        case TK_PLUS:
            emit_pop_rcx(cc);
            emit1(cc,0x48);emit1(cc,0x01);emit1(cc,0xC8); /* add rax,rcx */
            break;
        case TK_MINUS:
            emit_pop_rcx(cc);
            emit1(cc,0x48);emit1(cc,0x29);emit1(cc,0xC1); /* sub rcx,rax */
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC8); /* mov rax,rcx */
            break;
        case TK_STAR:
            emit_pop_rcx(cc);
            emit1(cc,0x48);emit1(cc,0x0F);emit1(cc,0xAF);emit1(cc,0xC1); /* imul rax,rcx */
            break;
        case TK_SLASH:
            emit_pop_rcx(cc);
            emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xC1);
            emit1(cc,0x48);emit1(cc,0x99);
            emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xF9); /* idiv rcx */
            break;
        case TK_PERCENT:
            emit_pop_rcx(cc);
            emit1(cc,0x48);emit1(cc,0x87);emit1(cc,0xC1);
            emit1(cc,0x48);emit1(cc,0x99);
            emit1(cc,0x48);emit1(cc,0xF7);emit1(cc,0xF9);
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0); /* mov rax,rdx */
            break;
        case TK_PIPE:
            emit_pop_rcx(cc); emit1(cc,0x48);emit1(cc,0x09);emit1(cc,0xC8); break;
        case TK_AMP:
            emit_pop_rcx(cc); emit1(cc,0x48);emit1(cc,0x21);emit1(cc,0xC8); break;
        case TK_CARET:
            emit_pop_rcx(cc); emit1(cc,0x48);emit1(cc,0x31);emit1(cc,0xC8); break;
        case TK_LSHIFT:
            /* lhs on stack, count in rax */
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC1); /* mov rcx,rax (count) */
            emit_pop_rdx(cc);                               /* rdx = lhs */
            emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xE2); /* shl rdx,cl */
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0); /* mov rax,rdx */
            break;
        case TK_RSHIFT:
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC1);
            emit_pop_rdx(cc);
            emit1(cc,0x48);emit1(cc,0xD3);emit1(cc,0xFA); /* sar rdx,cl */
            emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xD0);
            break;
        case TK_EQ:  emit_cmp_setcc(cc,0x94); break;
        case TK_NEQ: emit_cmp_setcc(cc,0x95); break;
        case TK_LT:  emit_cmp_setcc(cc,0x9C); break;
        case TK_GT:  emit_cmp_setcc(cc,0x9F); break;
        case TK_LEQ: emit_cmp_setcc(cc,0x9E); break;
        case TK_GEQ: emit_cmp_setcc(cc,0x9D); break;
        default: break;
        }
    }
}

static void parse_expr(CompilerState *cc) { parse_expr_prec(cc, 0); }

/* ------------------------------------------------------------------ */
/*  Local variable allocation                                         */
/* ------------------------------------------------------------------ */

static void parse_var_decl(CompilerState *cc, TypeDesc *base_td) {
    /* Parse one or more declarators: int x, *y=0, arr[10]; */
    for (;;) {
        TypeDesc td = *base_td;
        while (cur(cc)->type == TK_STAR) { td.ptr_depth++; advance(cc); }
        if (cur(cc)->type != TK_IDENT) { cc_error(cc, "expected variable name"); return; }
        char vname[64]; strncpy(vname, cur(cc)->sval, 63); advance(cc);

        int arr_n = 0;
        if (cur(cc)->type == TK_LBRACKET) {
            advance(cc);
            if (cur(cc)->type == TK_INTLIT) { arr_n = (int)cur(cc)->ival; advance(cc); }
            expect(cc, TK_RBRACKET, "expected ']'");
        }

        /* Infer array size from initializer if brackets were empty */
        if (arr_n == 0 && cur(cc)->type == TK_ASSIGN) {
            int sp = cc->tok_pos;
            advance(cc); /* skip = */
            if (cur(cc)->type == TK_LBRACE) {
                advance(cc);
                int count = 0; int depth2 = 0;
                while (!(cur(cc)->type == TK_RBRACE && depth2 == 0) &&
                       cur(cc)->type != TK_EOF) {
                    if (cur(cc)->type == TK_LBRACE) depth2++;
                    else if (cur(cc)->type == TK_RBRACE) { depth2--; continue; }
                    if (cur(cc)->type == TK_COMMA && depth2 == 0) { count++; advance(cc); continue; }
                    advance(cc);
                }
                count++; /* last element */
                arr_n = count;
            } else if (cur(cc)->type == TK_STRLIT && td.kind == TY_CHAR && td.ptr_depth == 0) {
                arr_n = strlen(cur(cc)->sval) + 1;
            }
            cc->tok_pos = sp; /* restore */
        }

        alloc_local(cc, vname, &td, arr_n);

        if (cur(cc)->type == TK_ASSIGN) {
            advance(cc);
            Symbol *s = find_local(cc, vname);

            if (cur(cc)->type == TK_LBRACE && s) {
                /* Array initializer: int a[] = {1, 2, 3}; */
                advance(cc); /* skip { */
                int idx = 0;
                int esz = type_sizeof(cc, &td);
                if (esz < 1) esz = 8;
                while (cur(cc)->type != TK_RBRACE && cur(cc)->type != TK_EOF && !cc->error) {
                    parse_expr(cc); /* value in rax */
                    emit_push_rax(cc); /* save value */
                    emit_lea_local(cc, s->offset); /* rax = &arr[0] */
                    if (idx > 0) {
                        emit1(cc,0x48);emit1(cc,0x05); emit4(cc,(uint32_t)(idx*esz));
                    }
                    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC1); /* mov rcx,rax */
                    emit1(cc, 0x58); /* pop rax (value) */
                    if (esz == 1) emit_store_via_rcx_byte(cc);
                    else          emit_store_via_rcx(cc);
                    idx++;
                    if (cur(cc)->type == TK_COMMA) advance(cc);
                }
                expect(cc, TK_RBRACE, "expected '}'");
            } else if (cur(cc)->type == TK_STRLIT && s &&
                       td.kind == TY_CHAR && td.ptr_depth == 0 && arr_n > 0) {
                /* char s[] = "hello"; — copy string bytes to local array */
                const char *str = cur(cc)->sval;
                int slen = strlen(str);
                advance(cc);
                for (int ci = 0; ci <= slen && ci < arr_n; ci++) {
                    char ch = (ci < slen) ? str[ci] : 0;
                    emit_mov_rax_imm32(cc, (int32_t)(uint8_t)ch);
                    emit_store_local_byte(cc, s->offset + ci);
                }
            } else {
                parse_expr(cc);
                if (s) emit_sym_store(cc, vname);
            }
        }

        if (cur(cc)->type == TK_COMMA) { advance(cc); continue; }
        break;
    }
    expect(cc, TK_SEMI, "expected ';'");
}

/* ------------------------------------------------------------------ */
/*  Block and statement parser                                         */
/* ------------------------------------------------------------------ */

static void parse_block(CompilerState *cc,
                        int *break_p, int *nbr,
                        int *cont_p,  int *nco);

static void parse_stmt(CompilerState *cc,
                       int *break_p, int *nbr,
                       int *cont_p,  int *nco) {
    if (cc->error) return;
    Token *t = cur(cc);

    /* block */
    if (t->type == TK_LBRACE) {
        advance(cc);
        int sv = cc->nlocals;
        parse_block(cc, break_p, nbr, cont_p, nco);
        cc->nlocals = sv;
        return;
    }

    /* return */
    if (t->type == TK_RETURN) {
        advance(cc);
        if (cur(cc)->type != TK_SEMI) parse_expr(cc);
        else emit_xor_rax(cc);
        expect(cc, TK_SEMI, "expected ';' after return");
        emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xEC); /* mov rsp,rbp */
        emit1(cc,0x5D);                                 /* pop rbp */
        emit1(cc,0xC3);                                 /* ret */
        return;
    }

    /* if / else */
    if (t->type == TK_IF) {
        advance(cc);
        expect(cc, TK_LPAREN, "expected '('");
        parse_expr(cc);
        expect(cc, TK_RPAREN, "expected ')'");
        emit_test_rax(cc);
        int jz = emit_jz(cc);
        parse_stmt(cc, break_p, nbr, cont_p, nco);
        if (cur(cc)->type == TK_ELSE) {
            advance(cc);
            int jmp = emit_jmp(cc);
            patch_rel32(cc, jz, cc->code_len);
            parse_stmt(cc, break_p, nbr, cont_p, nco);
            patch_rel32(cc, jmp, cc->code_len);
        } else {
            patch_rel32(cc, jz, cc->code_len);
        }
        return;
    }

    /* while */
    if (t->type == TK_WHILE) {
        advance(cc);
        int top = cc->code_len;
        expect(cc, TK_LPAREN, "expected '('");
        parse_expr(cc);
        expect(cc, TK_RPAREN, "expected ')'");
        emit_test_rax(cc);
        int jz = emit_jz(cc);
        int my_br[64]; int nbr2=0;
        int my_co[64]; int nco2=0;
        parse_stmt(cc, my_br, &nbr2, my_co, &nco2);
        /* continue targets: jump to condition re-eval */
        int jmp = emit_jmp(cc);
        patch_rel32(cc, jmp, top);
        int end = cc->code_len;
        patch_rel32(cc, jz, end);
        for (int i=0;i<nbr2;i++) patch_rel32(cc, my_br[i], end);
        for (int i=0;i<nco2;i++) patch_rel32(cc, my_co[i], top);
        return;
    }

    /* do ... while */
    if (t->type == TK_DO) {
        advance(cc);
        int top = cc->code_len;
        int my_br[64]; int nbr2=0;
        int my_co[64]; int nco2=0;
        parse_stmt(cc, my_br, &nbr2, my_co, &nco2);
        expect(cc, TK_WHILE, "expected 'while' after do body");
        expect(cc, TK_LPAREN, "expected '('");
        int cond_top = cc->code_len;
        for (int i=0;i<nco2;i++) patch_rel32(cc, my_co[i], cond_top);
        parse_expr(cc);
        expect(cc, TK_RPAREN, "expected ')'");
        expect(cc, TK_SEMI, "expected ';' after do-while");
        emit_test_rax(cc);
        int jnz = emit_jnz(cc);
        patch_rel32(cc, jnz, top);
        int end = cc->code_len;
        for (int i=0;i<nbr2;i++) patch_rel32(cc, my_br[i], end);
        return;
    }

    /* for */
    if (t->type == TK_FOR) {
        advance(cc);
        expect(cc, TK_LPAREN, "expected '('");
        int sv = cc->nlocals;
        /* init */
        if (cur(cc)->type != TK_SEMI) {
            if (starts_type(cc)) {
                TypeDesc td; parse_type(cc, &td);
                parse_var_decl(cc, &td); /* consumes semi */
            } else {
                parse_expr(cc);
                expect(cc, TK_SEMI, "expected ';'");
            }
        } else advance(cc);
        int top = cc->code_len;
        int jz = 0;
        if (cur(cc)->type != TK_SEMI) {
            parse_expr(cc);
            emit_test_rax(cc);
            jz = emit_jz(cc);
        }
        expect(cc, TK_SEMI, "expected ';'");
        /* save post-tokens */
        int post_start = cc->tok_pos;
        int depth = 0;
        while (!((cur(cc)->type==TK_RPAREN) && depth==0) && cur(cc)->type!=TK_EOF) {
            if (cur(cc)->type==TK_LPAREN) depth++;
            else if (cur(cc)->type==TK_RPAREN) { if (depth>0) depth--; else break; }
            advance(cc);
        }
        int post_end = cc->tok_pos;
        expect(cc, TK_RPAREN, "expected ')'");
        int my_br[64]; int nbr2=0;
        int my_co[64]; int nco2=0;
        parse_stmt(cc, my_br, &nbr2, my_co, &nco2);
        int post_top = cc->code_len;
        for (int i=0;i<nco2;i++) patch_rel32(cc, my_co[i], post_top);
        /* emit post expr */
        int saved_p = cc->tok_pos;
        cc->tok_pos = post_start;
        if (post_start < post_end) parse_expr(cc);
        cc->tok_pos = saved_p;
        int jmp = emit_jmp(cc);
        patch_rel32(cc, jmp, top);
        int end = cc->code_len;
        if (jz) patch_rel32(cc, jz, end);
        for (int i=0;i<nbr2;i++) patch_rel32(cc, my_br[i], end);
        cc->nlocals = sv;
        return;
    }

    /* switch */
    if (t->type == TK_SWITCH) {
        advance(cc);
        expect(cc, TK_LPAREN, "expected '('");
        parse_expr(cc); /* value in rax */
        expect(cc, TK_RPAREN, "expected ')'");
        /* Simple switch: push value, then for each case emit cmp+jne */
        /* We collect all case/default jump targets and patch them */
        /* Strategy: push val; jmp to dispatch table; emit case bodies;
           at end, patch dispatch table.
           Simpler: emit each case as: cmp rax_saved, imm; je case_label */
        /* Save switch value on stack */
        emit_push_rax(cc);
        /* jump to dispatch block */
        int jmp_dispatch = emit_jmp(cc);
        /* case bodies come after dispatch; we note their code offsets */
#define SW_MAX 64
        int64_t case_vals[SW_MAX];
        int case_offs[SW_MAX];
        int ncases = 0;
        int default_off = -1;
        /* After body, jump to end */
        int end_patches[SW_MAX]; int nend = 0;

        expect(cc, TK_LBRACE, "expected '{' after switch(...)");
        int my_br[64]; int nbr2 = 0;
        int my_co[64]; int nco2 = 0;
        while (cur(cc)->type != TK_RBRACE && cur(cc)->type != TK_EOF && !cc->error) {
            if (cur(cc)->type == TK_CASE) {
                advance(cc);
                int64_t v = 0;
                /* Handle optional unary minus for negative case values */
                if (cur(cc)->type == TK_MINUS) {
                    advance(cc);
                    if (cur(cc)->type == TK_INTLIT || cur(cc)->type == TK_CHARLIT) {
                        v = -(cur(cc)->ival); advance(cc);
                    }
                } else if (cur(cc)->type == TK_INTLIT || cur(cc)->type == TK_CHARLIT) {
                    v = cur(cc)->ival; advance(cc);
                }
                expect(cc, TK_COLON, "expected ':' after case");
                if (ncases < SW_MAX) { case_vals[ncases] = v; case_offs[ncases] = cc->code_len; ncases++; }
            } else if (cur(cc)->type == TK_DEFAULT) {
                advance(cc);
                expect(cc, TK_COLON, "expected ':' after default");
                default_off = cc->code_len;
            } else if (cur(cc)->type == TK_RBRACE) {
                break;
            } else {
                parse_stmt(cc, my_br, &nbr2, my_co, &nco2);
                /* implicit fallthrough — no jmp between cases (like real C) */
            }
        }
        if (cur(cc)->type == TK_RBRACE) advance(cc);
        /* jump to end (fall out of switch body before dispatch) */
        if (nend < SW_MAX) end_patches[nend++] = emit_jmp(cc);
        /* dispatch block */
        patch_rel32(cc, jmp_dispatch, cc->code_len);
        /* value still on stack — restore to rcx for comparison */
        emit_pop_rcx(cc); /* rcx = switch value */
        for (int ci = 0; ci < ncases; ci++) {
            /* cmp rcx, imm32; je case_off */
            emit1(cc,0x48);emit1(cc,0x81);emit1(cc,0xF9); emit4(cc,(uint32_t)(int32_t)case_vals[ci]);
            int je_off = emit_rel32_2_local(cc, 0x0F, 0x84);
            patch_rel32(cc, je_off, case_offs[ci]);
        }
        /* default */
        if (default_off >= 0) {
            int jd = emit_jmp(cc);
            patch_rel32(cc, jd, default_off);
        }
        int end = cc->code_len;
        for (int i=0;i<nend;i++) patch_rel32(cc, end_patches[i], end);
        for (int i=0;i<nbr2;i++) patch_rel32(cc, my_br[i], end);
        return;
    }

    /* break */
    if (t->type == TK_BREAK) {
        advance(cc);
        expect(cc, TK_SEMI, "expected ';'");
        if (break_p && *nbr < 64) break_p[(*nbr)++] = emit_jmp(cc);
        return;
    }

    /* continue */
    if (t->type == TK_CONTINUE) {
        advance(cc);
        expect(cc, TK_SEMI, "expected ';'");
        if (cont_p && *nco < 64) cont_p[(*nco)++] = emit_jmp(cc);
        return;
    }

    /* goto — emit jmp and record patch for later resolution */
    if (t->type == TK_GOTO) {
        advance(cc);
        if (cur(cc)->type == TK_IDENT) {
            if (cc->ngoto_patches < CC_MAX_GOTO_PATCHES) {
                GotoPatch *gp = &cc->goto_patches[cc->ngoto_patches++];
                gp->code_off = emit_jmp(cc);
                strncpy(gp->name, cur(cc)->sval, 31);
            }
            advance(cc);
        }
        expect(cc, TK_SEMI, "expected ';'");
        return;
    }

    /* label: ident ':' — record label position for goto resolution */
    if (t->type == TK_IDENT && peek1(cc)->type == TK_COLON) {
        if (cc->nlabels < CC_MAX_LABELS) {
            LabelDef *ld = &cc->labels[cc->nlabels++];
            strncpy(ld->name, t->sval, 31);
            ld->code_offset = cc->code_len;
        }
        advance(cc); advance(cc); /* skip ident and : */
        return;
    }

    /* variable declaration */
    if (starts_type(cc)) {
        TypeDesc td; parse_type(cc, &td);
        parse_var_decl(cc, &td);
        return;
    }

    /* empty statement */
    if (t->type == TK_SEMI) { advance(cc); return; }

    /* expression statement */
    parse_expr(cc);
    expect(cc, TK_SEMI, "expected ';'");
}

/* Helper to emit 2-byte opcode + rel32, returns offset of rel32 field */
static int emit_rel32_2_local(CompilerState *cc, uint8_t op0, uint8_t op1) {
    emit1(cc, op0); emit1(cc, op1);
    int off = cc->code_len;
    emit4(cc, 0);
    return off;
}

static void parse_block(CompilerState *cc,
                        int *break_p, int *nbr,
                        int *cont_p,  int *nco) {
    while (cur(cc)->type != TK_RBRACE && cur(cc)->type != TK_EOF && !cc->error)
        parse_stmt(cc, break_p, nbr, cont_p, nco);
    if (cur(cc)->type == TK_RBRACE) advance(cc);
}

/* ------------------------------------------------------------------ */
/*  Struct definition parser                                           */
/* ------------------------------------------------------------------ */

static void parse_struct_def(CompilerState *cc, int is_union) {
    /* struct/union Name { ... }; */
    if (cur(cc)->type != TK_IDENT) { cc_error(cc, "expected struct/union name"); return; }
    char sname[32]; strncpy(sname, cur(cc)->sval, 31); advance(cc);
    if (cur(cc)->type != TK_LBRACE) { return; } /* forward decl */
    advance(cc);
    if (cc->nstructs >= CC_MAX_STRUCTS) { cc_error(cc, "too many structs"); return; }
    StructDef *sd = &cc->structs[cc->nstructs++];
    strncpy(sd->name, sname, 31);
    sd->nfields = 0;
    sd->total_size = 0;
    sd->is_union = is_union;
    while (cur(cc)->type != TK_RBRACE && cur(cc)->type != TK_EOF && !cc->error) {
        TypeDesc ftd; parse_type(cc, &ftd);
        while (cur(cc)->type == TK_STAR) { ftd.ptr_depth++; advance(cc); }
        while (cur(cc)->type == TK_IDENT) {
            if (sd->nfields >= CC_MAX_FIELDS) { cc_error(cc, "too many struct fields"); break; }
            StructField *f = &sd->fields[sd->nfields++];
            strncpy(f->name, cur(cc)->sval, 31); advance(cc);
            f->type = ftd;
            int fsz = type_sizeof(cc, &ftd);
            if (fsz < 1) fsz = 8;
            if (is_union) {
                f->offset = 0; /* all union fields at offset 0 */
                if (fsz > sd->total_size) sd->total_size = fsz;
            } else {
                    /* Set field offset at current (aligned) position, then advance */
                f->offset = sd->total_size;
                sd->total_size += fsz;
                /* Align total_size to 8 bytes for next field */
                sd->total_size = (sd->total_size + 7) & ~7;
            }
            if (cur(cc)->type == TK_COMMA) { advance(cc); continue; }
            break;
        }
        expect(cc, TK_SEMI, "expected ';' after struct field");
    }
    if (is_union) sd->total_size = (sd->total_size + 7) & ~7; /* align union size */
    expect(cc, TK_RBRACE, "expected '}' to close struct/union");
}

/* ------------------------------------------------------------------ */
/*  Function definition parser                                         */
/* ------------------------------------------------------------------ */

static void parse_function(CompilerState *cc, const char *fname, TypeDesc *ret_td) {
    (void)ret_td;
    int func_start = cc->code_len;
    FuncInfo *finfo = declare_func(cc, fname, 0);
    if (!finfo) return;
    finfo->code_offset = func_start;
    finfo->defined = 1;

    /* resolve forward-call patches */
    for (int i = 0; i < cc->npatches; i++)
        if (strcmp(cc->patches[i].name, fname) == 0)
            patch_rel32(cc, cc->patches[i].code_off, func_start);

    if (strcmp(fname, "main") == 0) cc->main_offset = func_start;

    /* prologue */
    emit1(cc,0x55);                               /* push rbp */
    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xE5); /* mov rbp,rsp */
    int frame_patch = cc->code_len + 3;
    emit1(cc,0x48);emit1(cc,0x81);emit1(cc,0xEC); emit4(cc,0); /* sub rsp,0 */

    cc->nlocals = 0;
    cc->local_frame = 0;
    cc->nlabels = 0;
    cc->ngoto_patches = 0;

    /* parameters */
    expect(cc, TK_LPAREN, "expected '('");
    int nparam = 0;
    while (cur(cc)->type != TK_RPAREN && !cc->error) {
        if (cur(cc)->type == TK_VOID && peek1(cc)->type == TK_RPAREN) { advance(cc); break; }
        if (cur(cc)->type == TK_ELLIPSIS) { advance(cc); break; } /* varargs — ignore */
        TypeDesc ptd; parse_type(cc, &ptd);
        if (cur(cc)->type != TK_IDENT) { cc_error(cc, "expected param name"); return; }
        char pname[64]; strncpy(pname, cur(cc)->sval, 63); advance(cc);
        if (cur(cc)->type == TK_LBRACKET) {
            advance(cc);
            if (cur(cc)->type == TK_INTLIT) advance(cc);
            expect(cc, TK_RBRACKET, "expected ']'");
            ptd.ptr_depth++;
        }
        int off = alloc_local(cc, pname, &ptd, 0);
        emit_param_to_rax(cc, nparam);
        emit_store_local(cc, off);
        cc->locals[cc->nlocals-1].is_param = 1;
        nparam++;
        if (cur(cc)->type == TK_COMMA) advance(cc);
    }
    expect(cc, TK_RPAREN, "expected ')'");
    finfo->nparams = nparam;

    /* body */
    expect(cc, TK_LBRACE, "expected '{'");
    int dummy_br[64]; int dummy_nb=0;
    int dummy_co[64]; int dummy_nc=0;
    parse_block(cc, dummy_br, &dummy_nb, dummy_co, &dummy_nc);

    /* resolve goto patches */
    for (int gi = 0; gi < cc->ngoto_patches; gi++) {
        int found = 0;
        for (int li = 0; li < cc->nlabels; li++) {
            if (strcmp(cc->goto_patches[gi].name, cc->labels[li].name) == 0) {
                patch_rel32(cc, cc->goto_patches[gi].code_off, cc->labels[li].code_offset);
                found = 1;
                break;
            }
        }
        if (!found) cc_errorf(cc, "undefined label: %s", cc->goto_patches[gi].name);
    }

    /* patch frame size (16-byte aligned, min 16) */
    int fsz = (cc->local_frame + 15) & ~15;
    if (fsz < 16) fsz = 16;
    cc->code[frame_patch]   = fsz & 0xff;
    cc->code[frame_patch+1] = (fsz>>8) & 0xff;
    cc->code[frame_patch+2] = (fsz>>16) & 0xff;
    cc->code[frame_patch+3] = (fsz>>24) & 0xff;

    /* default return */
    emit_xor_rax(cc);
    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xEC); /* mov rsp,rbp */
    emit1(cc,0x5D);                                 /* pop rbp */
    emit1(cc,0xC3);                                 /* ret */
}

/* ------------------------------------------------------------------ */
/*  Global variable parser                                             */
/* ------------------------------------------------------------------ */

static void parse_global_decl(CompilerState *cc, const char *vname, TypeDesc *td) {
    if (cc->nglobals >= CC_MAX_GLOBALS) { cc_error(cc, "too many globals"); return; }
    Symbol *g = &cc->globals[cc->nglobals++];
    strncpy(g->name, vname, 31);
    g->type = *td;
    g->is_global = 1;
    g->is_param  = 0;

    int arr_n = 0;
    if (cur(cc)->type == TK_LBRACKET) {
        advance(cc);
        if (cur(cc)->type == TK_INTLIT) { arr_n = (int)cur(cc)->ival; advance(cc); }
        expect(cc, TK_RBRACKET, "expected ']'");
        g->type.kind = TY_ARRAY;
        g->type.arr_size = arr_n;
    }

    int esz = type_sizeof(cc, td);
    int total = (arr_n > 0) ? arr_n * esz : esz;
    if (total < 8) total = 8;

    int off = cc->data_len;
    if (cc->data_len + total > CC_DATA_MAX) { cc_error(cc, "data section full"); return; }
    cc->data_len += total;
    g->gaddr = data_vaddr(off);

    /* initializer */
    if (cur(cc)->type == TK_ASSIGN) {
        advance(cc);
        if (cur(cc)->type == TK_INTLIT || cur(cc)->type == TK_CHARLIT) {
            int64_t v = cur(cc)->ival; advance(cc);
            int doff = (int)(g->gaddr - (CC_LOAD_BASE + CC_DATA_OFFSET));
            for (int bi=0; bi<8 && bi<esz; bi++)
                cc->data[doff+bi] = (uint8_t)(v >> (bi*8));
        } else if (cur(cc)->type == TK_STRLIT) {
            if (td->kind == TY_CHAR && td->ptr_depth == 0 && arr_n > 0) {
                /* char msg[] = "hello" — copy bytes into data segment */
                const char *str = cur(cc)->sval;
                int slen = (int)strlen(str);
                int doff = (int)(g->gaddr - (CC_LOAD_BASE + CC_DATA_OFFSET));
                for (int ci = 0; ci < arr_n && ci <= slen; ci++)
                    cc->data[doff + ci] = (uint8_t)(ci < slen ? str[ci] : 0);
            } else {
                /* char *p = "string" — store pointer into data */
                int soff = data_add_string(cc, cur(cc)->sval);
                uint64_t sva = data_vaddr(soff);
                int doff = (int)(g->gaddr - (CC_LOAD_BASE + CC_DATA_OFFSET));
                for (int bi=0; bi<8; bi++) cc->data[doff+bi] = (uint8_t)(sva >> (bi*8));
            }
            advance(cc);
        } else if (cur(cc)->type == TK_LBRACE && arr_n > 0) {
            /* int arr[] = {1, 2, 3} — global array initializer */
            advance(cc); /* skip { */
            int idx = 0;
            int doff = (int)(g->gaddr - (CC_LOAD_BASE + CC_DATA_OFFSET));
            while (cur(cc)->type != TK_RBRACE && cur(cc)->type != TK_EOF && !cc->error) {
                int64_t v = 0;
                int neg = 0;
                if (cur(cc)->type == TK_MINUS) { neg = 1; advance(cc); }
                if (cur(cc)->type == TK_INTLIT || cur(cc)->type == TK_CHARLIT) {
                    v = cur(cc)->ival; advance(cc);
                }
                if (neg) v = -v;
                if (idx < arr_n && doff + idx * esz + 8 <= CC_DATA_MAX) {
                    for (int bi = 0; bi < 8 && bi < esz; bi++)
                        cc->data[doff + idx * esz + bi] = (uint8_t)(v >> (bi * 8));
                }
                idx++;
                if (cur(cc)->type == TK_COMMA) advance(cc);
            }
            if (cur(cc)->type == TK_RBRACE) advance(cc);
        }
    }
    expect(cc, TK_SEMI, "expected ';'");
}

/* ------------------------------------------------------------------ */
/*  Top-level parser                                                   */
/* ------------------------------------------------------------------ */

void cc_parse(CompilerState *cc) {
    if (cc->error) return;
    cc->tok_pos = 0;
    cc->code_len = 0;
    cc->data_len = 0;
    cc->nlocals = 0;
    cc->nglobals = 0;
    cc->nfuncs = 0;
    cc->npatches = 0;
    cc->nstructs = 0;
    cc->ntypedefs = 0;
    cc->main_offset = -1;
    cc->local_frame = 0;

    cc_seed_builtin_typedefs(cc);

    /* _start: call main, then exit(rax) — SYS_EXIT = 4 */
    int call_off = emit_call(cc);
    emit1(cc,0x48);emit1(cc,0x89);emit1(cc,0xC7); /* mov rdi,rax (exit code) */
    emit1(cc,0x48);emit1(cc,0xC7);emit1(cc,0xC0); emit4(cc,4); /* mov rax,4 (SYS_EXIT) */
    emit1(cc,0x0F);emit1(cc,0x05); /* syscall */

    while (cur(cc)->type != TK_EOF && !cc->error) {
        Token *t = cur(cc);

        /* typedef */
        if (t->type == TK_TYPEDEF) {
            advance(cc);
            TypeDesc td; parse_type(cc, &td);
            /* extra pointer stars after type before name */
            while (cur(cc)->type == TK_STAR) { td.ptr_depth++; advance(cc); }
            if (cur(cc)->type == TK_IDENT) {
                if (cc->ntypedefs < CC_MAX_TYPEDEFS) {
                    TypedefEntry *te = &cc->typedefs[cc->ntypedefs++];
                    strncpy(te->name, cur(cc)->sval, 31);
                    te->type = td;
                }
                advance(cc);
            }
            /* handle: typedef struct { ... } Name; */
            if (cur(cc)->type != TK_SEMI) {
                /* might have function pointer typedef - skip to semi */
                while (cur(cc)->type != TK_SEMI && cur(cc)->type != TK_EOF) advance(cc);
            }
            expect(cc, TK_SEMI, "expected ';' after typedef");
            continue;
        }

        /* struct definition: struct Name { ... }; */
        if (t->type == TK_STRUCT) {
            advance(cc);
            parse_struct_def(cc, 0);
            /* optional variable declaration after struct def */
            if (cur(cc)->type == TK_IDENT) {
                /* struct Foo x; */
                advance(cc); /* skip for now */
            }
            if (cur(cc)->type == TK_SEMI) { advance(cc); continue; }
            continue;
        }

        /* union definition: union Name { ... }; */
        if (t->type == TK_UNION) {
            advance(cc);
            parse_struct_def(cc, 1);
            if (cur(cc)->type == TK_IDENT) {
                advance(cc);
            }
            if (cur(cc)->type == TK_SEMI) { advance(cc); continue; }
            continue;
        }

        /* function or global variable */
        if (starts_type(cc)) {
            TypeDesc td; parse_type(cc, &td);
            /* handle multiple declarators / functions */
            while (!cc->error) {
                /* extra pointer */
                while (cur(cc)->type == TK_STAR) { td.ptr_depth++; advance(cc); }
                if (cur(cc)->type != TK_IDENT) { advance(cc); break; }
                char name[64]; strncpy(name, cur(cc)->sval, 63); advance(cc);
                if (cur(cc)->type == TK_LPAREN) {
                    parse_function(cc, name, &td);
                    break;
                } else {
                    parse_global_decl(cc, name, &td);
                    if (cur(cc)->type != TK_COMMA) break;
                    advance(cc);
                }
            }
            continue;
        }

        /* enum (simple: just skip it) */
        if (t->type == TK_ENUM) {
            advance(cc);
            if (cur(cc)->type == TK_IDENT) advance(cc);
            if (cur(cc)->type == TK_LBRACE) {
                advance(cc);
                int64_t val = 0;
                while (cur(cc)->type != TK_RBRACE && cur(cc)->type != TK_EOF) {
                    if (cur(cc)->type == TK_IDENT) {
                        /* Declare enum constant as global int with value */
                        char ename[64]; strncpy(ename, cur(cc)->sval, 63); advance(cc);
                        if (cur(cc)->type == TK_ASSIGN) {
                            advance(cc);
                            if (cur(cc)->type == TK_INTLIT) { val = cur(cc)->ival; advance(cc); }
                        }
                        if (cc->nglobals < CC_MAX_GLOBALS) {
                            Symbol *g = &cc->globals[cc->nglobals++];
                            strncpy(g->name, ename, 31);
                            TypeDesc etd; etd.kind=TY_INT; etd.ptr_depth=0;
                            etd.struct_idx=-1; etd.arr_size=0;
                            g->type = etd;
                            g->is_global = 1; g->is_param = 0;
                            int off = cc->data_len;
                            if (cc->data_len + 8 <= CC_DATA_MAX) { cc->data_len += 8; }
                            g->gaddr = data_vaddr(off);
                            int doff = off;
                            for (int bi=0;bi<8;bi++) cc->data[doff+bi]=(uint8_t)(val>>(bi*8));
                        }
                        val++;
                        if (cur(cc)->type == TK_COMMA) advance(cc);
                    } else advance(cc);
                }
                if (cur(cc)->type == TK_RBRACE) advance(cc);
            }
            if (cur(cc)->type == TK_SEMI) advance(cc);
            continue;
        }

        advance(cc); /* unknown top-level token */
    }

    /* patch _start call to main */
    if (cc->main_offset >= 0) {
        patch_rel32(cc, call_off, cc->main_offset);
    } else {
        cc_error(cc, "no main() defined");
    }

    /* patch remaining forward calls */
    for (int i = 0; i < cc->npatches; i++) {
        FuncInfo *f = find_func(cc, cc->patches[i].name);
        if (f && f->defined)
            patch_rel32(cc, cc->patches[i].code_off, f->code_offset);
    }
}
