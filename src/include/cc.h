#ifndef CC_H
#define CC_H

#include "types.h"

#define CC_MAX_TOKENS  131072
#define CC_MAX_LOCALS   1024
#define CC_MAX_GLOBALS  2048
#define CC_MAX_FUNCS     512
#define CC_MAX_STRUCTS   128
#define CC_MAX_FIELDS    64
#define CC_MAX_TYPEDEFS  128
#define CC_CODE_MAX   1048576  /* 1MB code */
#define CC_DATA_MAX   262144   /* 256KB data/strings */
#define CC_SRC_MAX    1048576  /* 1MB max preprocessed source */
#define CC_MAX_PATCHES 16384
#define CC_MAX_LABELS       256
#define CC_MAX_MACROS       256
#define CC_MAX_GOTO_PATCHES 256
#define CC_LOAD_BASE  0x400000ULL
#define CC_DATA_OFFSET 0x1000ULL  /* data section offset from load base */

/* Object file / linker support */
#define CC_MAX_RELOCS   4096
#define CC_MAX_SYMBOLS  2048

typedef enum {
    TK_EOF=0, TK_IDENT, TK_INTLIT, TK_STRLIT, TK_CHARLIT,
    /* type keywords */
    TK_INT, TK_CHAR, TK_VOID, TK_UNSIGNED, TK_LONG, TK_SHORT,
    TK_STRUCT, TK_TYPEDEF, TK_ENUM, TK_CONST, TK_EXTERN, TK_INLINE,
    TK_STATIC, TK_UNION, TK_VOLATILE, TK_RESTRICT,
    TK__STATIC_ASSERT, TK__ALIGNOF, TK__ALIGNAS, TK__NORETURN, TK__THREAD_LOCAL,
    /* control flow */
    TK_RETURN, TK_IF, TK_ELSE, TK_WHILE, TK_DO, TK_FOR,
    TK_BREAK, TK_CONTINUE, TK_SWITCH, TK_CASE, TK_DEFAULT, TK_GOTO,
    /* operators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE,
    TK_EQ, TK_NEQ, TK_LT, TK_GT, TK_LEQ, TK_GEQ,
    TK_ASSIGN,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
    TK_AMPEQ, TK_PIPEEQ, TK_CARETEQ, TK_LSHIFTEQ, TK_RSHIFTEQ,
    TK_AND, TK_OR, TK_NOT,
    TK_INC, TK_DEC,
    TK_LSHIFT, TK_RSHIFT,
    TK_SIZEOF,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_SEMI, TK_COMMA, TK_DOT, TK_ARROW, TK_COLON, TK_QUESTION,
    TK_HASH, TK_ELLIPSIS,
} TokenType;

typedef struct {
    TokenType type;
    char sval[64];
    int64_t ival;
    int line;
} Token;

/* Type descriptor */
#define TY_INT    0   /* int / long / short / unsigned — treated as 8-byte int */
#define TY_CHAR   1   /* char — 1 byte */
#define TY_VOID   2
#define TY_PTR    3   /* pointer to base_type */
#define TY_ARRAY  4
#define TY_STRUCT 5

typedef struct {
    int  kind;        /* TY_* */
    int  ptr_depth;   /* number of * levels (0 = not pointer) */
    int  struct_idx;  /* index into cc->structs[] for TY_STRUCT */
    int  arr_size;    /* number of elements for TY_ARRAY */
    int  is_unsigned; /* 1 = unsigned type */
} TypeDesc;

typedef struct {
    char    name[32];
    TypeDesc type;
    int     offset;      /* rbp-relative offset for locals (negative) */
    uint64_t gaddr;      /* virtual address for globals */
    int     is_global;
    int     is_param;
} Symbol;

typedef struct {
    char    name[32];
    TypeDesc type;
    int     offset;      /* byte offset within struct */
} StructField;

typedef struct {
    char        name[32];
    StructField fields[CC_MAX_FIELDS];
    int         nfields;
    int         total_size;  /* bytes */
    int         is_union;    /* 1 for union, 0 for struct */
} StructDef;

typedef struct {
    char name[32];
    TypeDesc type;
} TypedefEntry;

typedef struct {
    char name[32];
    int  code_offset; /* offset in code[] of function start */
    int  nparams;
    int  defined;
} FuncInfo;

typedef struct {
    int  code_off;
    char name[32];
} Patch;

typedef struct {
    char      name[32];
    TokenType tok_type;  /* TK_INTLIT, TK_STRLIT, TK_IDENT, TK_CHARLIT */
    char      sval[64];
    int64_t   ival;
} MacroDef;

typedef struct {
    char name[32];
    int  code_offset;
} LabelDef;

typedef struct {
    int  code_off;
    char name[32];
} GotoPatch;

/* Relocation types for object file output */
#define CC_RELOC_CALL    1   /* E8 rel32 call to named function */
#define CC_RELOC_ABS64   2   /* 64-bit absolute address of symbol */
#define CC_RELOC_DATA64  3   /* 64-bit absolute address into data section */

typedef struct {
    int  code_off;       /* offset in code[] where patch is needed */
    int  type;           /* CC_RELOC_* */
    char name[32];       /* symbol name (for CALL/ABS64) */
    int  data_off;       /* for DATA64: offset within data section */
    int  addend;         /* addend for relocation */
} RelocEntry;

/* ELF symbol for object file output */
#define CC_SYM_LOCAL   0
#define CC_SYM_GLOBAL  1
#define CC_SYM_UNDEF   2

typedef struct {
    char name[32];
    int  section;        /* 0=undef, 1=text, 2=data */
    int  offset;         /* offset within section */
    int  binding;        /* CC_SYM_LOCAL/GLOBAL/UNDEF */
} ObjSymbol;

typedef struct {
    char     src[CC_SRC_MAX];
    uint32_t src_len;

    Token    tokens[CC_MAX_TOKENS];
    int      ntokens;
    int      tok_pos;

    uint8_t  code[CC_CODE_MAX];
    int      code_len;

    uint8_t  data[CC_DATA_MAX];
    int      data_len;

    Symbol   locals[CC_MAX_LOCALS];
    int      nlocals;
    int      local_frame;  /* current frame size in bytes */

    Symbol   globals[CC_MAX_GLOBALS];
    int      nglobals;

    FuncInfo funcs[CC_MAX_FUNCS];
    int      nfuncs;

    StructDef structs[CC_MAX_STRUCTS];
    int       nstructs;

    TypedefEntry typedefs[CC_MAX_TYPEDEFS];
    int          ntypedefs;

    Patch    patches[CC_MAX_PATCHES];
    int      npatches;

    MacroDef macros[CC_MAX_MACROS];
    int      nmacros;

    LabelDef labels[CC_MAX_LABELS];
    int      nlabels;

    GotoPatch goto_patches[CC_MAX_GOTO_PATCHES];
    int      ngoto_patches;

    /* Object file support */
    RelocEntry relocs[CC_MAX_RELOCS];
    int        nrelocs;

    ObjSymbol  obj_syms[CC_MAX_SYMBOLS];
    int        nobj_syms;

    int        obj_mode;   /* 1 = emit relocatable .o, 0 = emit executable */

    int      error;
    int      nerrors;    /* number of errors (for multi-error reporting) */
    char     errmsg[256]; /* extended to hold line number prefix */

    int      main_offset;  /* offset of main() in code[] */

    char     filename[256]; /* source filename for __FILE__ */
    char     current_func_name[64]; /* current function name for __func__ */
    int      func_name_data_off;    /* data offset of __func__ string */
    int      last_unsigned;         /* 1 if last expr result is unsigned */
    int      unsigned_stack[64];    /* saved unsigned flags for pushed values */
    int      unsigned_depth;        /* depth of unsigned stack */
} CompilerState;

void cc_lex(CompilerState *cc);
void cc_parse(CompilerState *cc);
int  cc_write_elf(CompilerState *cc, const char *outpath);
int  cc_write_obj(CompilerState *cc, const char *outpath);
int  cc_link(const char **obj_paths, int nobj, const char *outpath,
             uint64_t load_base);

#endif /* CC_H */


