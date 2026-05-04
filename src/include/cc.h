#ifndef CC_H
#define CC_H

#include "types.h"

#define CC_MAX_TOKENS  8192
#define CC_MAX_LOCALS   256
#define CC_MAX_GLOBALS  128
#define CC_MAX_FUNCS     64
#define CC_MAX_STRUCTS   32
#define CC_MAX_FIELDS    32
#define CC_MAX_TYPEDEFS  32
#define CC_CODE_MAX   65536   /* 64KB code */
#define CC_DATA_MAX   16384   /* 16KB data/strings */
#define CC_SRC_MAX    65536   /* 64KB max source */
#define CC_MAX_PATCHES 1024
#define CC_MAX_LABELS       128
#define CC_MAX_MACROS        64
#define CC_MAX_GOTO_PATCHES 128
#define CC_LOAD_BASE  0x400000ULL
#define CC_DATA_OFFSET 0x1000ULL  /* data section offset from load base */

typedef enum {
    TK_EOF=0, TK_IDENT, TK_INTLIT, TK_STRLIT, TK_CHARLIT,
    /* type keywords */
    TK_INT, TK_CHAR, TK_VOID, TK_UNSIGNED, TK_LONG, TK_SHORT,
    TK_STRUCT, TK_TYPEDEF, TK_ENUM, TK_CONST, TK_EXTERN, TK_INLINE,
    TK_STATIC, TK_UNION,
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

    int      error;
    char     errmsg[128];

    int      main_offset;  /* offset of main() in code[] */
} CompilerState;

void cc_lex(CompilerState *cc);
void cc_parse(CompilerState *cc);
int  cc_write_elf(CompilerState *cc, const char *outpath);

#endif /* CC_H */


