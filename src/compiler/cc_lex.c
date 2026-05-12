/* cc_lex.c — Lexer for the in-kernel C compiler */

#include "cc.h"
#include "string.h"
#include "printf.h"

static void cc_error(CompilerState *cc, const char *msg) {
    cc->nerrors++;
    if (!cc->error) {
        cc->error = 1;
        strncpy(cc->errmsg, msg, sizeof(cc->errmsg) - 1);
        cc->errmsg[sizeof(cc->errmsg) - 1] = '\0';
    }
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

static int escape_char(char c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        default:   return c;
    }
}

static Token *next_tok(CompilerState *cc) {
    if (cc->ntokens >= CC_MAX_TOKENS) { cc_error(cc, "too many tokens"); return 0; }
    return &cc->tokens[cc->ntokens++];
}

/* Keyword table */
typedef struct { const char *word; TokenType type; } KW;
static const KW kwtab[] = {
    {"int",      TK_INT},
    {"char",     TK_CHAR},
    {"void",     TK_VOID},
    {"unsigned", TK_UNSIGNED},
    {"signed",   TK_INT},      /* treat 'signed' as int */
    {"long",     TK_LONG},
    {"short",    TK_SHORT},
    {"struct",   TK_STRUCT},
    {"typedef",  TK_TYPEDEF},
    {"enum",     TK_ENUM},
    {"const",    TK_CONST},
    {"extern",   TK_EXTERN},
    {"inline",   TK_INLINE},
    {"static",   TK_STATIC},
    {"return",   TK_RETURN},
    {"if",       TK_IF},
    {"else",     TK_ELSE},
    {"while",    TK_WHILE},
    {"do",       TK_DO},
    {"for",      TK_FOR},
    {"break",    TK_BREAK},
    {"continue", TK_CONTINUE},
    {"switch",   TK_SWITCH},
    {"case",     TK_CASE},
    {"default",  TK_DEFAULT},
    {"goto",     TK_GOTO},
    {"sizeof",   TK_SIZEOF},
    {"union",    TK_UNION},
    {"volatile", TK_VOLATILE},
    {"restrict", TK_RESTRICT},
    {"auto",     TK_STATIC},  /* treat auto as static (ignored) */
    {0, TK_EOF}
};

void cc_lex(CompilerState *cc) {
    const char *s = cc->src;
    uint32_t len = cc->src_len;
    uint32_t i = 0;
    int line = 1;

    cc->ntokens = 0;

    while (i < len && !cc->error) {
        char c = s[i];

        /* whitespace */
        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        /* // comment */
        if (c == '/' && i+1 < len && s[i+1] == '/') {
            while (i < len && s[i] != '\n') i++;
            continue;
        }

        /* block comment */
        if (c == '/' && i+1 < len && s[i+1] == '*') {
            i += 2;
            while (i+1 < len && !(s[i] == '*' && s[i+1] == '/')) {
                if (s[i] == '\n') line++;
                i++;
            }
            i += 2;
            continue;
        }

        /* preprocessor directive */
        if (c == '#') {
            i++; /* skip # */
            while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
            char dir[16] = {0};
            int dk = 0;
            while (i < len && is_alpha(s[i]) && dk < 15) dir[dk++] = s[i++];
            dir[dk] = '\0';

            if (strcmp(dir, "define") == 0) {
                while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
                char mname[32] = {0};
                dk = 0;
                while (i < len && is_alnum(s[i]) && dk < 31) mname[dk++] = s[i++];
                mname[dk] = '\0';
                /* skip optional function-like macro parens (just skip to value) */
                if (i < len && s[i] == '(') {
                    while (i < len && s[i] != ')') i++;
                    if (i < len) i++;
                }
                while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
                if (cc->nmacros < CC_MAX_MACROS && mname[0]) {
                    MacroDef *md = &cc->macros[cc->nmacros++];
                    strncpy(md->name, mname, 31);
                    md->sval[0] = '\0'; md->ival = 0;
                    if (i < len && s[i] != '\n') {
                        if (s[i] == '"') {
                            i++; dk = 0;
                            while (i < len && s[i] != '"' && dk < 63) {
                                if (s[i] == '\\' && i+1 < len) {
                                    md->sval[dk++] = (char)escape_char(s[i+1]); i += 2;
                                } else { md->sval[dk++] = s[i++]; }
                            }
                            md->sval[dk] = '\0';
                            if (i < len && s[i] == '"') i++;
                            md->tok_type = TK_STRLIT;
                        } else if (s[i] == '\'') {
                            i++;
                            if (i < len && s[i] == '\\' && i+1 < len) {
                                md->ival = escape_char(s[i+1]); i += 2;
                            } else if (i < len) { md->ival = (unsigned char)s[i++]; }
                            if (i < len && s[i] == '\'') i++;
                            md->tok_type = TK_CHARLIT;
                        } else if (is_digit(s[i]) || (s[i] == '-' && i+1 < len && is_digit(s[i+1]))) {
                            int neg = 0;
                            if (s[i] == '-') { neg = 1; i++; }
                            int64_t v = 0;
                            if (s[i] == '0' && i+1 < len && (s[i+1]=='x'||s[i+1]=='X')) {
                                i += 2;
                                while (i < len && is_hex(s[i])) v = v*16 + hex_val(s[i++]);
                            } else {
                                while (i < len && is_digit(s[i])) v = v*10 + (s[i++]-'0');
                            }
                            while (i < len && (s[i]=='u'||s[i]=='U'||s[i]=='l'||s[i]=='L')) i++;
                            md->ival = neg ? -v : v;
                            md->tok_type = TK_INTLIT;
                        } else if (is_alpha(s[i])) {
                            dk = 0;
                            while (i < len && is_alnum(s[i]) && dk < 63) md->sval[dk++] = s[i++];
                            md->sval[dk] = '\0';
                            md->tok_type = TK_IDENT;
                        } else {
                            md->tok_type = TK_INTLIT; md->ival = 1;
                        }
                    } else {
                        md->tok_type = TK_INTLIT; md->ival = 1;
                    }
                }
                while (i < len && s[i] != '\n') i++;
            } else if (strcmp(dir, "undef") == 0) {
                while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
                char mname[32] = {0};
                dk = 0;
                while (i < len && is_alnum(s[i]) && dk < 31) mname[dk++] = s[i++];
                mname[dk] = '\0';
                for (int mi = 0; mi < cc->nmacros; mi++) {
                    if (strcmp(cc->macros[mi].name, mname) == 0) {
                        for (int mj = mi; mj < cc->nmacros-1; mj++)
                            cc->macros[mj] = cc->macros[mj+1];
                        cc->nmacros--;
                        break;
                    }
                }
                while (i < len && s[i] != '\n') i++;
            } else if (strcmp(dir, "ifdef") == 0 || strcmp(dir, "ifndef") == 0 ||
                       strcmp(dir, "if") == 0) {
                int active = 0;
                if (strcmp(dir, "if") == 0) {
                    while (i < len && (s[i]==' '||s[i]=='\t')) i++;
                    if (is_digit(s[i])) {
                        int64_t v = 0;
                        while (i < len && is_digit(s[i])) v = v*10 + (s[i++]-'0');
                        active = (v != 0);
                    } else if (i+7 <= len && s[i]=='d'&&s[i+1]=='e'&&s[i+2]=='f'&&
                               s[i+3]=='i'&&s[i+4]=='n'&&s[i+5]=='e'&&s[i+6]=='d') {
                        i += 7;
                        while (i < len && (s[i]==' '||s[i]=='('||s[i]=='\t')) i++;
                        char mn[32]={0}; dk=0;
                        while (i < len && is_alnum(s[i]) && dk<31) mn[dk++] = s[i++];
                        mn[dk]='\0';
                        for (int mi=0;mi<cc->nmacros;mi++)
                            if (strcmp(cc->macros[mi].name,mn)==0){active=1;break;}
                    } else if (s[i]=='!') {
                        i++;
                        while (i < len && (s[i]==' '||s[i]=='\t')) i++;
                        if (i+7 <= len && s[i]=='d'&&s[i+1]=='e'&&s[i+2]=='f'&&
                               s[i+3]=='i'&&s[i+4]=='n'&&s[i+5]=='e'&&s[i+6]=='d') {
                            i += 7;
                            while (i < len && (s[i]==' '||s[i]=='('||s[i]=='\t')) i++;
                            char mn[32]={0}; dk=0;
                            while (i < len && is_alnum(s[i]) && dk<31) mn[dk++] = s[i++];
                            mn[dk]='\0';
                            active=1;
                            for (int mi=0;mi<cc->nmacros;mi++)
                                if (strcmp(cc->macros[mi].name,mn)==0){active=0;break;}
                        }
                    }
                } else {
                    int want_def = (dir[2]=='d'); /* ifdef vs ifndef */
                    while (i < len && (s[i]==' '||s[i]=='\t')) i++;
                    char mn[32]={0}; dk=0;
                    while (i < len && is_alnum(s[i]) && dk<31) mn[dk++] = s[i++];
                    mn[dk]='\0';
                    int found=0;
                    for (int mi=0;mi<cc->nmacros;mi++)
                        if (strcmp(cc->macros[mi].name,mn)==0){found=1;break;}
                    active = (want_def == found);
                }
                while (i < len && s[i] != '\n') i++;
                if (!active) {
                    int depth = 1;
                    while (i < len && depth > 0) {
                        if (s[i] == '\n') line++;
                        if (s[i] == '#') {
                            uint32_t si2 = i+1;
                            while (si2<len && (s[si2]==' '||s[si2]=='\t')) si2++;
                            char d2[16]={0}; int d2k=0;
                            while (si2<len && is_alpha(s[si2]) && d2k<15) d2[d2k++]=s[si2++];
                            d2[d2k]='\0';
                            if (strcmp(d2,"ifdef")==0||strcmp(d2,"ifndef")==0||strcmp(d2,"if")==0) depth++;
                            else if (strcmp(d2,"endif")==0){depth--;if(depth==0){i=si2;break;}}
                            else if ((strcmp(d2,"else")==0||strcmp(d2,"elif")==0)&&depth==1){i=si2;break;}
                        }
                        i++;
                    }
                    while (i < len && s[i] != '\n') i++;
                }
            } else if (strcmp(dir, "else") == 0 || strcmp(dir, "elif") == 0) {
                /* Active branch hit #else/#elif — skip to #endif */
                int depth = 1;
                while (i < len && depth > 0) {
                    if (s[i] == '\n') line++;
                    if (s[i] == '#') {
                        uint32_t si2 = i+1;
                        while (si2<len && (s[si2]==' '||s[si2]=='\t')) si2++;
                        char d2[16]={0}; int d2k=0;
                        while (si2<len && is_alpha(s[si2]) && d2k<15) d2[d2k++]=s[si2++];
                        d2[d2k]='\0';
                        if (strcmp(d2,"ifdef")==0||strcmp(d2,"ifndef")==0||strcmp(d2,"if")==0) depth++;
                        else if (strcmp(d2,"endif")==0){depth--;if(depth==0){i=si2;break;}}
                    }
                    i++;
                }
                while (i < len && s[i] != '\n') i++;
            } else if (strcmp(dir, "endif") == 0) {
                while (i < len && s[i] != '\n') i++;
            } else {
                /* unknown directive (#include, #pragma, etc.) — skip line */
                while (i < len && s[i] != '\n') i++;
            }
            continue;
        }

        Token *t = next_tok(cc);
        if (!t) break;
        t->line = line;
        t->ival = 0;
        t->sval[0] = '\0';

        /* string literal */
        if (c == '"') {
            i++;
            int si = 0;
            while (i < len && s[i] != '"') {
                char ch;
                if (s[i] == '\\' && i+1 < len) {
                    if (s[i+1] == 'x' && i+3 < len && is_hex(s[i+2])) {
                        /* \xNN hex escape */
                        int v = hex_val(s[i+2]);
                        if (is_hex(s[i+3])) { v = v*16 + hex_val(s[i+3]); i += 4; }
                        else i += 3;
                        ch = (char)v;
                    } else {
                        ch = (char)escape_char(s[i+1]);
                        i += 2;
                    }
                } else {
                    ch = s[i++];
                }
                if (si < 63) t->sval[si++] = ch;
            }
            if (i < len) i++; /* skip closing " */
            t->sval[si] = '\0';
            t->type = TK_STRLIT;
            continue;
        }

        /* char literal */
        if (c == '\'') {
            i++;
            if (i < len && s[i] == '\\' && i+1 < len) {
                if (s[i+1] == 'x' && i+3 < len && is_hex(s[i+2])) {
                    int v = hex_val(s[i+2]);
                    if (is_hex(s[i+3])) { v = v*16 + hex_val(s[i+3]); i += 4; }
                    else i += 3;
                    t->ival = v;
                } else {
                    t->ival = escape_char(s[i+1]);
                    i += 2;
                }
            } else if (i < len) {
                t->ival = (unsigned char)s[i++];
            }
            if (i < len && s[i] == '\'') i++;
            t->type = TK_CHARLIT;
            continue;
        }

        /* integer literal */
        if (is_digit(c)) {
            if (c == '0' && i+1 < len && (s[i+1] == 'x' || s[i+1] == 'X')) {
                i += 2;
                int64_t v = 0;
                while (i < len && is_hex(s[i])) v = v*16 + hex_val(s[i++]);
                t->ival = v;
            } else if (c == '0' && i+1 < len && s[i+1] >= '0' && s[i+1] <= '7') {
                /* octal */
                i++;
                int64_t v = 0;
                while (i < len && s[i] >= '0' && s[i] <= '7') v = v*8 + (s[i++]-'0');
                t->ival = v;
            } else {
                int64_t v = 0;
                while (i < len && is_digit(s[i])) v = v*10 + (s[i++]-'0');
                t->ival = v;
            }
            /* skip suffix: u U l L ul UL ull ULL */
            while (i < len && (s[i]=='u'||s[i]=='U'||s[i]=='l'||s[i]=='L')) i++;
            t->type = TK_INTLIT;
            continue;
        }

        /* identifier or keyword */
        if (is_alpha(c)) {
            int si = 0;
            while (i < len && is_alnum(s[i]) && si < 63) t->sval[si++] = s[i++];
            while (i < len && is_alnum(s[i])) i++;
            t->sval[si] = '\0';

            /* keyword lookup */
            t->type = TK_IDENT;
            for (int k = 0; kwtab[k].word; k++) {
                if (strcmp(t->sval, kwtab[k].word) == 0) {
                    t->type = kwtab[k].type;
                    break;
                }
            }
            /* macro expansion */
            if (t->type == TK_IDENT) {
                for (int mi = 0; mi < cc->nmacros; mi++) {
                    if (strcmp(t->sval, cc->macros[mi].name) == 0) {
                        t->type = cc->macros[mi].tok_type;
                        if (t->type == TK_INTLIT || t->type == TK_CHARLIT)
                            t->ival = cc->macros[mi].ival;
                        else
                            strncpy(t->sval, cc->macros[mi].sval, 63);
                        break;
                    }
                }
            }
            continue;
        }

        /* operators and punctuation */
        i++;
        switch (c) {
        case '+':
            if (i < len && s[i] == '+') { t->type = TK_INC; i++; }
            else if (i < len && s[i] == '=') { t->type = TK_PLUSEQ; i++; }
            else t->type = TK_PLUS;
            break;
        case '-':
            if (i < len && s[i] == '-') { t->type = TK_DEC; i++; }
            else if (i < len && s[i] == '=') { t->type = TK_MINUSEQ; i++; }
            else if (i < len && s[i] == '>') { t->type = TK_ARROW; i++; }
            else t->type = TK_MINUS;
            break;
        case '*':
            if (i < len && s[i] == '=') { t->type = TK_STAREQ; i++; }
            else t->type = TK_STAR;
            break;
        case '/':
            if (i < len && s[i] == '=') { t->type = TK_SLASHEQ; i++; }
            else t->type = TK_SLASH;
            break;
        case '%':
            if (i < len && s[i] == '=') { t->type = TK_PERCENTEQ; i++; }
            else t->type = TK_PERCENT;
            break;
        case '&':
            if (i < len && s[i] == '&') { t->type = TK_AND; i++; }
            else if (i < len && s[i] == '=') { t->type = TK_AMPEQ; i++; }
            else t->type = TK_AMP;
            break;
        case '|':
            if (i < len && s[i] == '|') { t->type = TK_OR; i++; }
            else if (i < len && s[i] == '=') { t->type = TK_PIPEEQ; i++; }
            else t->type = TK_PIPE;
            break;
        case '^':
            if (i < len && s[i] == '=') { t->type = TK_CARETEQ; i++; }
            else t->type = TK_CARET;
            break;
        case '~': t->type = TK_TILDE; break;
        case '=':
            if (i < len && s[i] == '=') { t->type = TK_EQ; i++; }
            else t->type = TK_ASSIGN;
            break;
        case '!':
            if (i < len && s[i] == '=') { t->type = TK_NEQ; i++; }
            else t->type = TK_NOT;
            break;
        case '<':
            if (i < len && s[i] == '<') {
                i++;
                if (i < len && s[i] == '=') { t->type = TK_LSHIFTEQ; i++; }
                else t->type = TK_LSHIFT;
            } else if (i < len && s[i] == '=') { t->type = TK_LEQ; i++; }
            else t->type = TK_LT;
            break;
        case '>':
            if (i < len && s[i] == '>') {
                i++;
                if (i < len && s[i] == '=') { t->type = TK_RSHIFTEQ; i++; }
                else t->type = TK_RSHIFT;
            } else if (i < len && s[i] == '=') { t->type = TK_GEQ; i++; }
            else t->type = TK_GT;
            break;
        case '.':
            if (i+1 < len && s[i] == '.' && s[i+1] == '.') { t->type = TK_ELLIPSIS; i += 2; }
            else t->type = TK_DOT;
            break;
        case '(': t->type = TK_LPAREN;   break;
        case ')': t->type = TK_RPAREN;   break;
        case '{': t->type = TK_LBRACE;   break;
        case '}': t->type = TK_RBRACE;   break;
        case '[': t->type = TK_LBRACKET; break;
        case ']': t->type = TK_RBRACKET; break;
        case ';': t->type = TK_SEMI;     break;
        case ',': t->type = TK_COMMA;    break;
        case ':': t->type = TK_COLON;    break;
        case '?': t->type = TK_QUESTION; break;
        default:
            cc->ntokens--;
            break;
        }
    }

    /* EOF token */
    if (cc->ntokens < CC_MAX_TOKENS) {
        Token *t = &cc->tokens[cc->ntokens++];
        t->type = TK_EOF;
        t->line = line;
        t->sval[0] = '\0';
        t->ival = 0;
    }
}


