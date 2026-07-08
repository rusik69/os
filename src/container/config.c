/*
 * config.c — OCI config.json parser (Item C2)
 *
 * Parses the OCI runtime-spec config.json file format and populates an
 * oci_config structure with the container's configuration.
 *
 * The parser is a minimal recursive-descent JSON parser that handles the
 * subset of JSON required by the OCI runtime-spec. It supports:
 *   - Objects (nested key-value pairs)
 *   - Arrays (of strings, numbers, objects)
 *   - Strings (with standard escape sequences)
 *   - Numbers (integers and decimals)
 *   - Booleans (true/false) and null
 *
 * File: src/container/config.c
 */

#define KERNEL_INTERNAL
#include "oci_spec.h"
#include "fs.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  JSON Tokenizer
 * ═══════════════════════════════════════════════════════════════════════ */

/* Token types returned by the tokenizer */
enum json_token_type {
    TOKEN_EOF,           /* end of input */
    TOKEN_ERROR,         /* lexical error */
    TOKEN_STRING,        /* "..." */
    TOKEN_NUMBER,        /* 123, -45, 3.14 */
    TOKEN_TRUE,          /* true */
    TOKEN_FALSE,         /* false */
    TOKEN_NULL,          /* null */
    TOKEN_LBRACE,        /* { */
    TOKEN_RBRACE,        /* } */
    TOKEN_LBRACKET,      /* [ */
    TOKEN_RBRACKET,      /* ] */
    TOKEN_COMMA,         /* , */
    TOKEN_COLON,         /* : */
};

/* Tokenizer state */
struct json_token {
    enum json_token_type type;
    char  str[512];       /* string value (for TOKEN_STRING) */
    double num;           /* numeric value (for TOKEN_NUMBER) */
    int    intval;        /* integer value (for TOKEN_NUMBER, truncated) */
    int    line;          /* line number for error reporting */
    int    col;           /* column number for error reporting */
};

/* Tokenizer context */
struct json_lexer {
    const char *pos;      /* current position in input */
    const char *end;      /* end of input */
    int         line;
    int         col;
    char        err_msg[256];
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Tokenizer implementation
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize the lexer */
static void lexer_init(struct json_lexer *lex, const char *json, uint64_t size)
{
    lex->pos  = json;
    lex->end  = json + size;
    lex->line = 1;
    lex->col  = 1;
    lex->err_msg[0] = '\0';
}

/* Advance one character */
static inline void lexer_advance(struct json_lexer *lex)
{
    if (*lex->pos == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    lex->pos++;
}

/* Skip whitespace */
static void lexer_skip_ws(struct json_lexer *lex)
{
    while (lex->pos < lex->end) {
        char c = *lex->pos;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            lexer_advance(lex);
        } else {
            break;
        }
    }
}

/* Set an error message with position info */
static void lexer_error(struct json_lexer *lex, const char *msg)
{
    snprintf(lex->err_msg, sizeof(lex->err_msg),
             "JSON error at line %d col %d: %s", lex->line, lex->col, msg);
}

/* Store an error in a token */
static void token_error(struct json_token *tok, const char *msg)
{
    tok->type = TOKEN_ERROR;
    snprintf(tok->str, sizeof(tok->str), "%s", msg);
}

/* Parse a JSON string (after the opening quote has been consumed) */
static int lexer_string(struct json_lexer *lex, struct json_token *tok)
{
    int out = 0;
    int max_len = (int)sizeof(tok->str) - 1;

    while (lex->pos < lex->end) {
        char c = *lex->pos;

        if (c == '"') {
            /* End of string */
            lexer_advance(lex);
            tok->str[out < max_len ? out : max_len] = '\0';
            tok->type = TOKEN_STRING;
            return 1;
        }

        if (c == '\\') {
            /* Escape sequence */
            lexer_advance(lex);
            if (lex->pos >= lex->end) {
                lexer_error(lex, "unexpected end in string escape");
                token_error(tok, lex->err_msg);
                return 0;
            }
            char escaped = *lex->pos;
            switch (escaped) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                /* \uXXXX — parse hex code point and emit UTF-8 */
                /* First advance past the 'u' */
                lexer_advance(lex);
                if (lex->pos + 3 >= lex->end) {
                    lexer_error(lex, "truncated \\uXXXX escape");
                    token_error(tok, lex->err_msg);
                    return 0;
                }
                /* Parse 4 hex digits */
                uint32_t cp = 0;
                int valid_hex = 1;
                for (int i = 0; i < 4; i++) {
                    char h = *lex->pos;
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp |= (uint32_t)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp |= (uint32_t)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        cp |= (uint32_t)(h - 'A' + 10);
                    else
                        valid_hex = 0;
                    lexer_advance(lex);
                }
                if (!valid_hex) {
                    lexer_error(lex, "invalid hex digit in \\uXXXX escape");
                    token_error(tok, lex->err_msg);
                    return 0;
                }
                /* Convert code point to UTF-8 (1-3 bytes) */
                if (cp <= 0x007F) {
                    if (out < max_len) tok->str[out++] = (char)(uint8_t)cp;
                } else if (cp <= 0x07FF) {
                    if (out < max_len)
                        tok->str[out++] = (char)(uint8_t)(0xC0 | (cp >> 6));
                    if (out < max_len)
                        tok->str[out++] = (char)(uint8_t)(0x80 | (cp & 0x3F));
                } else if (cp <= 0xFFFF) {
                    if (out < max_len)
                        tok->str[out++] = (char)(uint8_t)(0xE0 | (cp >> 12));
                    if (out < max_len)
                        tok->str[out++] = (char)(uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                    if (out < max_len)
                        tok->str[out++] = (char)(uint8_t)(0x80 | (cp & 0x3F));
                } else {
                    /* Code point out of range for \uXXXX (max U+FFFF) */
                    lexer_error(lex, "code point out of range in \\uXXXX");
                    token_error(tok, lex->err_msg);
                    return 0;
                }
                /* Don't advance again below — we already moved past 4 hex digits */
                continue;
            }
            default:
                c = escaped; /* pass through */
                break;
            }
            lexer_advance(lex);
            if (out < max_len) tok->str[out++] = c;
        } else if (c < 0x20) {
            /* Control characters not allowed in JSON strings */
            lexer_error(lex, "control character in string");
            token_error(tok, lex->err_msg);
            return 0;
        } else {
            lexer_advance(lex);
            if (out < max_len) tok->str[out++] = c;
        }
    }

    lexer_error(lex, "unterminated string");
    token_error(tok, lex->err_msg);
    return 0;
}

/* Parse a JSON number (after the first digit/minus has been consumed) */
static int lexer_number(struct json_lexer *lex, struct json_token *tok)
{
    char buf[64];
    int out = 0;
    int max_len = (int)sizeof(buf) - 1;

    /* Already consumed the first character (minus or digit), which is in
     * lex->pos[-1].  We use start = pos-1 to capture it. */
    const char *start = lex->pos - 1;
    const char *p = start;

    while (p < lex->end) {
        char c = *p;
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
            c == '.' || c == 'e' || c == 'E') {
            if (out < max_len) buf[out++] = c;
            p++;
        } else {
            break;
        }
    }
    buf[out < max_len ? out : max_len] = '\0';

    /* Update lexer position to reflect actual consumed chars */
    lex->pos = p;
    /* Update col */
    lex->col += (int)(p - start);

    /* Parse the number */
    tok->type = TOKEN_NUMBER;
    tok->intval = 0;
    tok->num = 0.0;

    /* Simple integer parsing (for flexibility, parse as int first) */
    int sign = 1;
    const char *np = buf;
    if (*np == '-') { sign = -1; np++; }
    else if (*np == '+') { np++; }

    while (*np) {
        if (*np >= '0' && *np <= '9') {
            tok->intval = tok->intval * 10 + (int)(*np - '0');
            np++;
        } else if (*np == '.') {
            np++;
            /* Skip fractional part for intval */
            while (*np >= '0' && *np <= '9') np++;
        } else {
            /* 'e' or 'E' — skip exponent for intval */
            np++;
            if (*np == '+' || *np == '-') np++;
            while (*np >= '0' && *np <= '9') np++;
        }
    }
    tok->intval *= sign;
    tok->num = (double)tok->intval;

    snprintf(tok->str, sizeof(tok->str), "%s", buf);
    return 1;
}

/* Get the next token from the input */
static int lexer_next(struct json_lexer *lex, struct json_token *tok)
{
    lexer_skip_ws(lex);

    if (lex->pos >= lex->end) {
        tok->type = TOKEN_EOF;
        tok->str[0] = '\0';
        return 1;
    }

    char c = *lex->pos;
    tok->line = lex->line;
    tok->col  = lex->col;

    switch (c) {
    case '{': tok->type = TOKEN_LBRACE;  tok->str[0] = c; tok->str[1] = '\0'; lexer_advance(lex); return 1;
    case '}': tok->type = TOKEN_RBRACE;  tok->str[0] = c; tok->str[1] = '\0'; lexer_advance(lex); return 1;
    case '[': tok->type = TOKEN_LBRACKET; tok->str[0] = c; tok->str[1] = '\0'; lexer_advance(lex); return 1;
    case ']': tok->type = TOKEN_RBRACKET; tok->str[0] = c; tok->str[1] = '\0'; lexer_advance(lex); return 1;
    case ',': tok->type = TOKEN_COMMA;   tok->str[0] = c; tok->str[1] = '\0'; lexer_advance(lex); return 1;
    case ':': tok->type = TOKEN_COLON;   tok->str[0] = c; tok->str[1] = '\0'; lexer_advance(lex); return 1;

    case '"':
        lexer_advance(lex); /* consume opening quote */
        return lexer_string(lex, tok);

    case 't':
        if (lex->end - lex->pos >= 4 && memcmp(lex->pos, "true", 4) == 0) {
            tok->type = TOKEN_TRUE;
            snprintf(tok->str, sizeof(tok->str), "true");
            for (int i = 0; i < 4; i++) lexer_advance(lex);
            return 1;
        }
        lexer_error(lex, "expected 'true'");
        token_error(tok, lex->err_msg);
        return 0;

    case 'f':
        if (lex->end - lex->pos >= 5 && memcmp(lex->pos, "false", 5) == 0) {
            tok->type = TOKEN_FALSE;
            snprintf(tok->str, sizeof(tok->str), "false");
            for (int i = 0; i < 5; i++) lexer_advance(lex);
            return 1;
        }
        lexer_error(lex, "expected 'false'");
        token_error(tok, lex->err_msg);
        return 0;

    case 'n':
        if (lex->end - lex->pos >= 4 && memcmp(lex->pos, "null", 4) == 0) {
            tok->type = TOKEN_NULL;
            snprintf(tok->str, sizeof(tok->str), "null");
            for (int i = 0; i < 4; i++) lexer_advance(lex);
            return 1;
        }
        lexer_error(lex, "expected 'null'");
        token_error(tok, lex->err_msg);
        return 0;

    case '-': case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        lexer_advance(lex);
        return lexer_number(lex, tok);

    default:
        lexer_error(lex, "unexpected character");
        token_error(tok, lex->err_msg);
        return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Recursive-descent JSON Parser
 * ═══════════════════════════════════════════════════════════════════════ */

/* Parser context */
struct json_parser {
    struct json_lexer  lex;
    struct json_token  tok;       /* current token (lookahead) */
    int                have_tok;  /* 1 = tok is valid, 0 = need to fetch */
    struct oci_config *config;    /* output config being populated */
};

/* Error message buffer size */
#define PARSE_ERR_LEN 256

/* Advance to the next token */
static int parser_next(struct json_parser *p)
{
    if (!lexer_next(&p->lex, &p->tok)) {
        /* Lexer error — copy message to config */
        snprintf(p->config->err_msg, sizeof(p->config->err_msg),
                 "%s", p->lex.err_msg);
        return 0;
    }
    p->have_tok = 1;
    return 1;
}

/* Expect a specific token type; consume and advance */
static int parser_expect(struct json_parser *p, enum json_token_type type)
{
    if (!p->have_tok) {
        if (!parser_next(p)) return 0;
    }
    if (p->tok.type != type) {
        snprintf(p->config->err_msg, sizeof(p->config->err_msg),
                 "JSON: expected token type %d at line %d col %d, got '%s'",
                 (int)type, p->tok.line, p->tok.col, p->tok.str);
        return 0;
    }
    p->have_tok = 0; /* consumed */
    return 1;
}

/* Peek at the current token type without consuming */
static enum json_token_type parser_peek(struct json_parser *p)
{
    if (!p->have_tok) {
        if (!parser_next(p)) return TOKEN_ERROR;
    }
    return p->tok.type;
}

/* Skip past a value (any JSON value) for fields we don't care about */
static int parser_skip_value(struct json_parser *p);

/* Skip nested object */
static int parser_skip_object(struct json_parser *p)
{
    if (!parser_expect(p, TOKEN_LBRACE)) return 0;
    int depth = 1;
    while (depth > 0) {
        enum json_token_type t = parser_peek(p);
        if (t == TOKEN_EOF) {
            snprintf(p->config->err_msg, sizeof(p->config->err_msg),
                     "JSON: unexpected end of input in object");
            return 0;
        }
        if (t == TOKEN_LBRACE) depth++;
        if (t == TOKEN_RBRACE) depth--;
        p->have_tok = 0;
        if (depth > 0 && !parser_next(p)) return 0;
    }
    return 1;
}

/* Skip nested array */
static int parser_skip_array(struct json_parser *p)
{
    if (!parser_expect(p, TOKEN_LBRACKET)) return 0;
    int depth = 1;
    while (depth > 0) {
        enum json_token_type t = parser_peek(p);
        if (t == TOKEN_EOF) {
            snprintf(p->config->err_msg, sizeof(p->config->err_msg),
                     "JSON: unexpected end of input in array");
            return 0;
        }
        if (t == TOKEN_LBRACKET) depth++;
        if (t == TOKEN_RBRACKET) depth--;
        p->have_tok = 0;
        if (depth > 0 && !parser_next(p)) return 0;
    }
    return 1;
}

static int parser_skip_value(struct json_parser *p)
{
    enum json_token_type t = parser_peek(p);
    switch (t) {
    case TOKEN_STRING:
    case TOKEN_NUMBER:
    case TOKEN_TRUE:
    case TOKEN_FALSE:
    case TOKEN_NULL:
        p->have_tok = 0; /* consume */
        return 1;
    case TOKEN_LBRACE:
        return parser_skip_object(p);
    case TOKEN_LBRACKET:
        return parser_skip_array(p);
    default:
        snprintf(p->config->err_msg, sizeof(p->config->err_msg),
                 "JSON: unexpected token '%s' at line %d col %d",
                 p->tok.str, p->tok.line, p->tok.col);
        return 0;
    }
}

/* ── String array parsing ─────────────────────────────────────────── */

/* Parse a JSON array of strings into a buffer */
static int parse_string_array(struct json_parser *p,
                               char (*out)[OCI_STR_LEN], int max_count,
                               int *out_count)
{
    *out_count = 0;
    if (!parser_expect(p, TOKEN_LBRACKET)) return 0;

    while (parser_peek(p) != TOKEN_RBRACKET) {
        if (*out_count >= max_count) {
            /* Silently skip excess entries */
            if (!parser_skip_value(p)) return 0;
            goto check_comma;
        }
        if (!parser_expect(p, TOKEN_STRING)) return 0;
        snprintf(out[*out_count], OCI_STR_LEN, "%s", p->tok.str);
        (*out_count)++;

    check_comma:
        if (parser_peek(p) == TOKEN_COMMA) {
            p->have_tok = 0;
        }
    }
    return parser_expect(p, TOKEN_RBRACKET);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  OCI-specific field parsers
 * ═══════════════════════════════════════════════════════════════════════ */

/* Parse the "process" object */
static int parse_process(struct json_parser *p)
{
    if (!parser_expect(p, TOKEN_LBRACE)) return 0;

    while (parser_peek(p) != TOKEN_RBRACE) {
        if (!parser_expect(p, TOKEN_STRING)) return 0;
        const char *key = p->tok.str;

        if (!parser_expect(p, TOKEN_COLON)) return 0;

        if (strcmp(key, "terminal") == 0) {
            enum json_token_type t = parser_peek(p);
            p->config->process.terminal = (t == TOKEN_TRUE) ? 1 : 0;
            p->have_tok = 0;
        } else if (strcmp(key, "noNewPrivileges") == 0) {
            enum json_token_type t = parser_peek(p);
            p->config->process.no_new_privs = (t == TOKEN_TRUE) ? 1 : 0;
            p->have_tok = 0;
        } else if (strcmp(key, "cwd") == 0) {
            if (!parser_expect(p, TOKEN_STRING)) return 0;
            snprintf(p->config->process.cwd, sizeof(p->config->process.cwd),
                     "%s", p->tok.str);
        } else if (strcmp(key, "args") == 0) {
            if (!parse_string_array(p, p->config->process.args,
                                    OCI_MAX_ARGS,
                                    &p->config->process.num_args))
                return 0;
        } else if (strcmp(key, "env") == 0) {
            if (!parse_string_array(p, p->config->process.env,
                                    OCI_MAX_ENV,
                                    &p->config->process.num_env))
                return 0;
        } else if (strcmp(key, "user") == 0) {
            /* Parse user object */
            if (!parser_expect(p, TOKEN_LBRACE)) return 0;
            while (parser_peek(p) != TOKEN_RBRACE) {
                if (!parser_expect(p, TOKEN_STRING)) return 0;
                const char *uk = p->tok.str;
                if (!parser_expect(p, TOKEN_COLON)) return 0;
                if (strcmp(uk, "uid") == 0) {
                    if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                    p->config->process.user.uid = (uint32_t)p->tok.intval;
                } else if (strcmp(uk, "gid") == 0) {
                    if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                    p->config->process.user.gid = (uint32_t)p->tok.intval;
                } else if (strcmp(uk, "additionalGids") == 0) {
                    if (!parser_expect(p, TOKEN_LBRACKET)) return 0;
                    int idx = 0;
                    while (parser_peek(p) != TOKEN_RBRACKET) {
                        if (idx < OCI_MAX_GIDS) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->process.user.additional_gids[idx++] =
                                (uint32_t)p->tok.intval;
                        } else {
                            if (!parser_skip_value(p)) return 0;
                        }
                        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
                    }
                    p->config->process.user.num_additional_gids = idx;
                    p->have_tok = 0; /* consume RBRACKET */
                } else {
                    if (!parser_skip_value(p)) return 0;
                }
                if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
            }
            p->have_tok = 0; /* consume RBRACE */
        } else if (strcmp(key, "capabilities") == 0) {
            /* Parse capabilities object */
            if (!parser_expect(p, TOKEN_LBRACE)) return 0;
            while (parser_peek(p) != TOKEN_RBRACE) {
                if (!parser_expect(p, TOKEN_STRING)) return 0;
                const char *ck = p->tok.str;
                if (!parser_expect(p, TOKEN_COLON)) return 0;

                uint32_t *mask = NULL;
                if (strcmp(ck, "effective") == 0)   mask = &p->config->process.caps.effective;
                else if (strcmp(ck, "bounding") == 0)  mask = &p->config->process.caps.bounding;
                else if (strcmp(ck, "permitted") == 0) mask = &p->config->process.caps.permitted;
                else if (strcmp(ck, "inheritable") == 0) mask = &p->config->process.caps.inheritable;

                if (mask) {
                    *mask = 0;
                    /* Parse array of capability strings */
                    if (!parser_expect(p, TOKEN_LBRACKET)) return 0;
                    while (parser_peek(p) != TOKEN_RBRACKET) {
                        if (!parser_expect(p, TOKEN_STRING)) return 0;
                        *mask |= oci_cap_name_to_bit(p->tok.str);
                        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
                    }
                    p->have_tok = 0; /* consume RBRACKET */
                } else {
                    if (!parser_skip_value(p)) return 0;
                }
                if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
            }
            p->have_tok = 0; /* consume RBRACE */
        } else if (strcmp(key, "rlimits") == 0) {
            /* Parse rlimits array */
            if (!parser_expect(p, TOKEN_LBRACKET)) return 0;
            int rl_idx = 0;
            while (parser_peek(p) != TOKEN_RBRACKET) {
                if (!parser_expect(p, TOKEN_LBRACE)) return 0;
                struct oci_rlimit rl;
                memset(&rl, 0, sizeof(rl));
                while (parser_peek(p) != TOKEN_RBRACE) {
                    if (!parser_expect(p, TOKEN_STRING)) return 0;
                    const char *rk = p->tok.str;
                    if (!parser_expect(p, TOKEN_COLON)) return 0;
                    if (strcmp(rk, "type") == 0) {
                        if (!parser_expect(p, TOKEN_STRING)) return 0;
                        snprintf(rl.type, sizeof(rl.type), "%s", p->tok.str);
                    } else if (strcmp(rk, "soft") == 0) {
                        if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                        rl.soft = (uint64_t)(p->tok.intval >= 0 ? p->tok.intval : 0);
                    } else if (strcmp(rk, "hard") == 0) {
                        if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                        rl.hard = (uint64_t)(p->tok.intval >= 0 ? p->tok.intval : 0);
                    } else {
                        if (!parser_skip_value(p)) return 0;
                    }
                    if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
                }
                p->have_tok = 0; /* consume RBRACE */
                if (rl_idx < OCI_MAX_RLIMITS) {
                    p->config->process.rlimits[rl_idx++] = rl;
                    p->config->process.num_rlimits = rl_idx;
                }
                if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
            }
            p->have_tok = 0; /* consume RBRACKET */
        } else {
            if (!parser_skip_value(p)) return 0;
        }

        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
    }
    p->have_tok = 0; /* consume RBRACE */
    return 1;
}

/* Parse the "root" object */
static int parse_root(struct json_parser *p)
{
    if (!parser_expect(p, TOKEN_LBRACE)) return 0;
    while (parser_peek(p) != TOKEN_RBRACE) {
        if (!parser_expect(p, TOKEN_STRING)) return 0;
        const char *key = p->tok.str;
        if (!parser_expect(p, TOKEN_COLON)) return 0;
        if (strcmp(key, "path") == 0) {
            if (!parser_expect(p, TOKEN_STRING)) return 0;
            snprintf(p->config->root.path, sizeof(p->config->root.path),
                     "%s", p->tok.str);
        } else if (strcmp(key, "readonly") == 0) {
            enum json_token_type t = parser_peek(p);
            p->config->root.readonly = (t == TOKEN_TRUE) ? 1 : 0;
            p->have_tok = 0;
        } else {
            if (!parser_skip_value(p)) return 0;
        }
        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
    }
    p->have_tok = 0; /* consume RBRACE */
    return 1;
}

/* Parse a single mount object */
static int parse_mount_entry(struct json_parser *p, struct oci_mount *mnt)
{
    memset(mnt, 0, sizeof(*mnt));
    if (!parser_expect(p, TOKEN_LBRACE)) return 0;

    while (parser_peek(p) != TOKEN_RBRACE) {
        if (!parser_expect(p, TOKEN_STRING)) return 0;
        const char *key = p->tok.str;
        if (!parser_expect(p, TOKEN_COLON)) return 0;

        if (strcmp(key, "destination") == 0) {
            if (!parser_expect(p, TOKEN_STRING)) return 0;
            snprintf(mnt->destination, sizeof(mnt->destination), "%s", p->tok.str);
        } else if (strcmp(key, "type") == 0) {
            if (!parser_expect(p, TOKEN_STRING)) return 0;
            snprintf(mnt->type, sizeof(mnt->type), "%s", p->tok.str);
        } else if (strcmp(key, "source") == 0) {
            if (!parser_expect(p, TOKEN_STRING)) return 0;
            snprintf(mnt->source, sizeof(mnt->source), "%s", p->tok.str);
        } else if (strcmp(key, "options") == 0) {
            /* options is an array of strings */
            if (!parser_expect(p, TOKEN_LBRACKET)) return 0;
            char opts[256];
            int  opos = 0;
            while (parser_peek(p) != TOKEN_RBRACKET) {
                if (!parser_expect(p, TOKEN_STRING)) return 0;
                if (opos > 0 && opos < (int)sizeof(opts) - 1)
                    opts[opos++] = ',';
                int slen = (int)strlen(p->tok.str);
                if (slen + opos < (int)sizeof(opts) - 1) {
                    memcpy(opts + opos, p->tok.str, (size_t)slen);
                    opos += slen;
                }
                if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
            }
            opts[opos < (int)sizeof(opts) ? opos : (int)sizeof(opts) - 1] = '\0';
            snprintf(mnt->options, sizeof(mnt->options), "%s", opts);
            mnt->num_options = opos;
            p->have_tok = 0; /* consume RBRACKET */
        } else {
            if (!parser_skip_value(p)) return 0;
        }
        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
    }
    p->have_tok = 0; /* consume RBRACE */
    return 1;
}

/* Parse the "mounts" array */
static int parse_mounts(struct json_parser *p)
{
    if (!parser_expect(p, TOKEN_LBRACKET)) return 0;
    while (parser_peek(p) != TOKEN_RBRACKET) {
        if (p->config->num_mounts < OCI_MAX_MOUNTS) {
            if (!parse_mount_entry(p, &p->config->mounts[p->config->num_mounts]))
                return 0;
            p->config->num_mounts++;
        } else {
            if (!parser_skip_value(p)) return 0;
        }
        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
    }
    p->have_tok = 0; /* consume RBRACKET */
    return 1;
}

/* Parse the "linux" object (namespaces, resources) */
static int parse_linux(struct json_parser *p)
{
    if (!parser_expect(p, TOKEN_LBRACE)) return 0;

    while (parser_peek(p) != TOKEN_RBRACE) {
        if (!parser_expect(p, TOKEN_STRING)) return 0;
        const char *key = p->tok.str;
        if (!parser_expect(p, TOKEN_COLON)) return 0;

        if (strcmp(key, "namespaces") == 0) {
            /* Parse namespaces array */
            if (!parser_expect(p, TOKEN_LBRACKET)) return 0;
            while (parser_peek(p) != TOKEN_RBRACKET) {
                if (!parser_expect(p, TOKEN_LBRACE)) return 0;
                struct oci_namespace ns;
                memset(&ns, 0, sizeof(ns));
                while (parser_peek(p) != TOKEN_RBRACE) {
                    if (!parser_expect(p, TOKEN_STRING)) return 0;
                    const char *nk = p->tok.str;
                    if (!parser_expect(p, TOKEN_COLON)) return 0;
                    if (strcmp(nk, "type") == 0) {
                        if (!parser_expect(p, TOKEN_STRING)) return 0;
                        snprintf(ns.type, sizeof(ns.type), "%s", p->tok.str);
                    } else if (strcmp(nk, "path") == 0) {
                        if (!parser_expect(p, TOKEN_STRING)) return 0;
                        snprintf(ns.path, sizeof(ns.path), "%s", p->tok.str);
                    } else {
                        if (!parser_skip_value(p)) return 0;
                    }
                    if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
                }
                p->have_tok = 0; /* consume RBRACE */
                if (p->config->linux.num_namespaces < OCI_MAX_NAMESPACES) {
                    p->config->linux.namespaces[p->config->linux.num_namespaces++] = ns;
                }
                if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
            }
            p->have_tok = 0; /* consume RBRACKET */
        } else if (strcmp(key, "resources") == 0) {
            /* Parse resources object */
            if (!parser_expect(p, TOKEN_LBRACE)) return 0;
            while (parser_peek(p) != TOKEN_RBRACE) {
                if (!parser_expect(p, TOKEN_STRING)) return 0;
                const char *rk = p->tok.str;
                if (!parser_expect(p, TOKEN_COLON)) return 0;

                if (strcmp(rk, "memory") == 0) {
                    if (!parser_expect(p, TOKEN_LBRACE)) return 0;
                    while (parser_peek(p) != TOKEN_RBRACE) {
                        if (!parser_expect(p, TOKEN_STRING)) return 0;
                        const char *mk = p->tok.str;
                        if (!parser_expect(p, TOKEN_COLON)) return 0;
                        if (strcmp(mk, "limit") == 0) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->linux.resources.memory_limit =
                                (uint64_t)p->tok.intval;
                        } else if (strcmp(mk, "reservation") == 0) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->linux.resources.memory_reservation =
                                (uint64_t)p->tok.intval;
                        } else if (strcmp(mk, "swap") == 0) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->linux.resources.memory_swap =
                                (uint64_t)p->tok.intval;
                        } else {
                            if (!parser_skip_value(p)) return 0;
                        }
                        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
                    }
                    p->have_tok = 0; /* consume RBRACE */
                } else if (strcmp(rk, "cpu") == 0) {
                    if (!parser_expect(p, TOKEN_LBRACE)) return 0;
                    while (parser_peek(p) != TOKEN_RBRACE) {
                        if (!parser_expect(p, TOKEN_STRING)) return 0;
                        const char *ck = p->tok.str;
                        if (!parser_expect(p, TOKEN_COLON)) return 0;
                        if (strcmp(ck, "shares") == 0) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->linux.resources.cpu_shares =
                                (uint64_t)p->tok.intval;
                        } else if (strcmp(ck, "quota") == 0) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->linux.resources.cpu_quota_us =
                                (uint64_t)p->tok.intval;
                        } else if (strcmp(ck, "period") == 0) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->linux.resources.cpu_period_us =
                                (uint64_t)p->tok.intval;
                        } else if (strcmp(ck, "cpus") == 0) {
                            if (!parser_expect(p, TOKEN_STRING)) return 0;
                            p->config->linux.resources.cpu_cpus =
                                (uint64_t)strlen(p->tok.str);
                        } else {
                            if (!parser_skip_value(p)) return 0;
                        }
                        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
                    }
                    p->have_tok = 0; /* consume RBRACE */
                } else if (strcmp(rk, "pids") == 0) {
                    if (!parser_expect(p, TOKEN_LBRACE)) return 0;
                    while (parser_peek(p) != TOKEN_RBRACE) {
                        if (!parser_expect(p, TOKEN_STRING)) return 0;
                        const char *pk = p->tok.str;
                        if (!parser_expect(p, TOKEN_COLON)) return 0;
                        if (strcmp(pk, "limit") == 0) {
                            if (!parser_expect(p, TOKEN_NUMBER)) return 0;
                            p->config->linux.resources.pids_limit =
                                (uint64_t)p->tok.intval;
                        } else {
                            if (!parser_skip_value(p)) return 0;
                        }
                        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
                    }
                    p->have_tok = 0; /* consume RBRACE */
                } else {
                    if (!parser_skip_value(p)) return 0;
                }
                if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
            }
            p->have_tok = 0; /* consume RBRACE */
        } else if (strcmp(key, "cgroupsPath") == 0) {
            if (!parser_expect(p, TOKEN_STRING)) return 0;
            snprintf(p->config->linux.cgroups_path, sizeof(p->config->linux.cgroups_path),
                     "%s", p->tok.str);
        } else {
            if (!parser_skip_value(p)) return 0;
        }
        if (parser_peek(p) == TOKEN_COMMA) p->have_tok = 0;
    }
    p->have_tok = 0; /* consume RBRACE */
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Top-level config.json parser
 * ═══════════════════════════════════════════════════════════════════════ */

int oci_config_parse(struct oci_config *config, const char *json, uint64_t size)
{
    if (!config || !json) {
        if (config)
            snprintf(config->err_msg, sizeof(config->err_msg), "NULL arguments");
        return -1;
    }

    struct json_parser p;
    memset(&p, 0, sizeof(p));
    p.config = config;
    lexer_init(&p.lex, json, size);
    p.have_tok = 0;

    /* Parse top-level object */
    if (!parser_expect(&p, TOKEN_LBRACE)) return -1;

    while (parser_peek(&p) != TOKEN_RBRACE) {
        if (!parser_expect(&p, TOKEN_STRING)) return -1;
        const char *key = p.tok.str;

        if (!parser_expect(&p, TOKEN_COLON)) return -1;

        if (strcmp(key, "ociVersion") == 0) {
            if (!parser_expect(&p, TOKEN_STRING)) return -1;
            snprintf(config->oci_version, sizeof(config->oci_version), "%s", p.tok.str);
        } else if (strcmp(key, "hostname") == 0) {
            if (!parser_expect(&p, TOKEN_STRING)) return -1;
            snprintf(config->hostname, sizeof(config->hostname), "%s", p.tok.str);
        } else if (strcmp(key, "process") == 0) {
            if (!parse_process(&p)) return -1;
        } else if (strcmp(key, "root") == 0) {
            if (!parse_root(&p)) return -1;
        } else if (strcmp(key, "mounts") == 0) {
            if (!parse_mounts(&p)) return -1;
        } else if (strcmp(key, "linux") == 0) {
            if (!parse_linux(&p)) return -1;
        } else {
            /* Skip unknown top-level fields */
            if (!parser_skip_value(&p)) return -1;
        }

        if (parser_peek(&p) == TOKEN_COMMA) p.have_tok = 0;
    }

    /* Consume closing brace */
    p.have_tok = 0;

    /* Check for trailing content */
    if (parser_peek(&p) != TOKEN_EOF) {
        snprintf(config->err_msg, sizeof(config->err_msg),
                 "JSON: unexpected trailing content after root object");
        return -1;
    }

    return 0;
}

void oci_config_free(struct oci_config *config)
{
    (void)config;
    /* Currently no dynamically allocated resources to free.
     * All arrays are statically sized in the oci_config struct. */
}

int oci_config_read_file(struct oci_config *config, const char *path)
{
    if (!config || !path) return -EINVAL;

    /* Stat the file to get size */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0)
        return -ENOENT;

    uint64_t file_size = st.size;
    if (file_size == 0 || file_size > 1024 * 1024) {
        /* Config files over 1MB are suspicious */
        snprintf(config->err_msg, sizeof(config->err_msg),
                 "config.json too large: %llu bytes (max 1MB)",
                 (unsigned long long)file_size);
        return -EFBIG;
    }

    /* Allocate buffer for file contents */
    char *buf = (char *)kmalloc((size_t)file_size + 1);
    if (!buf) return -ENOMEM;

    /* Read the file */
    uint32_t bytes_read = 0;
    int ret = vfs_read(path, buf, (uint32_t)file_size, &bytes_read);
    if (ret < 0 || bytes_read != file_size) {
        kfree(buf);
        return -EIO;
    }
    buf[file_size] = '\0';

    /* Parse the JSON */
    ret = oci_config_parse(config, buf, file_size);

    kfree(buf);
    return ret;
}

/* ── config_create ─────────────────────────────── */
static int config_create(const char *name, void *cfg)
{
    (void)name;
    (void)cfg;
    kprintf("[container] Config created: %s\n", name ? name : "unnamed");
    return 0;
}
/* ── config_get ─────────────────────────────── */
static int config_get(const char *name, const char *key, void *val, size_t len)
{
    (void)name;
    (void)key;
    (void)val;
    (void)len;
    return -ENOENT;
}
/* ── config_set ─────────────────────────────── */
static int config_set(const char *name, const char *key, const void *val)
{
    (void)name;
    (void)key;
    (void)val;
    kprintf("[container] Config set %s/%s\n", name ? name : "?", key ? key : "?");
    return 0;
}
