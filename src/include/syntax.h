#ifndef SYNTAX_H
#define SYNTAX_H

#include "vga.h"

/* ── Language identifiers ─────────────────────────────────────────── */
typedef enum {
    SYNTAX_NONE = 0,
    SYNTAX_C,
    SYNTAX_ASM,
    SYNTAX_SHELL,
    SYNTAX_PYTHON,
    SYNTAX_MAKEFILE,
    SYNTAX_COUNT
} syntax_lang_t;

/* ── Token types for coloring ─────────────────────────────────────── */
typedef enum {
    TOKEN_DEFAULT = 0,
    TOKEN_KEYWORD,
    TOKEN_TYPE,
    TOKEN_STRING,
    TOKEN_COMMENT,
    TOKEN_PREPROC,
    TOKEN_NUMBER,
    TOKEN_OPERATOR,
    TOKEN_CHAR,
    TOKEN_BUILTIN
} syntax_token_t;

/* ── VGA color mapping for each token type ────────────────────────── */
#define SYN_COLOR_DEFAULT    (VGA_LIGHT_GREY  | (VGA_BLACK << 4))
#define SYN_COLOR_KEYWORD    (VGA_WHITE       | (VGA_BLACK << 4))
#define SYN_COLOR_TYPE       (VGA_YELLOW      | (VGA_BLACK << 4))
#define SYN_COLOR_STRING     (VGA_LIGHT_GREEN | (VGA_BLACK << 4))
#define SYN_COLOR_COMMENT    (VGA_DARK_GREY   | (VGA_BLACK << 4))
#define SYN_COLOR_PREPROC    (VGA_LIGHT_MAGENTA | (VGA_BLACK << 4))
#define SYN_COLOR_NUMBER     (VGA_CYAN        | (VGA_BLACK << 4))
#define SYN_COLOR_OPERATOR   (VGA_LIGHT_CYAN  | (VGA_BLACK << 4))
#define SYN_COLOR_CHAR       (VGA_LIGHT_GREEN | (VGA_BLACK << 4))
#define SYN_COLOR_BUILTIN    (VGA_LIGHT_RED   | (VGA_BLACK << 4))

/* ── ANSI color sequences for each token type ─────────────────────── */
#define SYN_ANSI_DEFAULT  "\x1b[0m"
#define SYN_ANSI_KEYWORD  "\x1b[1;37m"  /* bold white */
#define SYN_ANSI_TYPE     "\x1b[1;33m"  /* bold yellow */
#define SYN_ANSI_STRING   "\x1b[32m"    /* green */
#define SYN_ANSI_COMMENT  "\x1b[90m"    /* dark grey */
#define SYN_ANSI_PREPROC  "\x1b[35m"    /* magenta */
#define SYN_ANSI_NUMBER   "\x1b[36m"    /* cyan */
#define SYN_ANSI_OPERATOR "\x1b[36m"    /* cyan */
#define SYN_ANSI_CHAR     "\x1b[32m"    /* green */
#define SYN_ANSI_BUILTIN  "\x1b[31m"    /* red */
#define SYN_ANSI_RESET    "\x1b[0m"

/* ── Maximum characters per line we'll color ──────────────────────── */
#define SYN_LINE_MAX    256

/* ── API ──────────────────────────────────────────────────────────── */

/*
 * Detect language from a filename extension.
 * Returns SYNTAX_NONE for unknown extensions.
 */
syntax_lang_t syntax_detect(const char *filename);

/*
 * Tokenize one line of source code.
 * Fills 'tokens[0..line_len-1]' with token types for each character.
 * 'in_multi' is an in/out parameter tracking multi-line comment state
 * for languages that support them (C: slash-asterisk comments).
 * Returns the line length.
 */
int syntax_tokenize(syntax_lang_t lang, const char *line, int line_len,
                    syntax_token_t *tokens, int *in_multi);

/*
 * Convert a token type to a VGA color attribute.
 */
static inline uint8_t syntax_token_to_vga(syntax_token_t t) {
    switch (t) {
        case TOKEN_KEYWORD:  return SYN_COLOR_KEYWORD;
        case TOKEN_TYPE:     return SYN_COLOR_TYPE;
        case TOKEN_STRING:   return SYN_COLOR_STRING;
        case TOKEN_COMMENT:  return SYN_COLOR_COMMENT;
        case TOKEN_PREPROC:  return SYN_COLOR_PREPROC;
        case TOKEN_NUMBER:   return SYN_COLOR_NUMBER;
        case TOKEN_OPERATOR: return SYN_COLOR_OPERATOR;
        case TOKEN_CHAR:     return SYN_COLOR_CHAR;
        case TOKEN_BUILTIN:  return SYN_COLOR_BUILTIN;
        default:             return SYN_COLOR_DEFAULT;
    }
}

/*
 * Convert a token type to an ANSI color escape string.
 * Returns a pointer to a static string.
 */
const char *syntax_token_to_ansi(syntax_token_t t);

#endif /* SYNTAX_H */
