/* syntax.c — Syntax highlighting engine for editor
 *
 * Provides language detection from file extensions and line-by-line
 * tokenization with multi-line comment tracking for C-like languages.
 *
 * Supported languages:
 *   - C / C++ (.c, .h, .cpp, .hpp)
 *   - Assembly (.asm, .s, .S)
 *   - Shell (.sh)
 *   - Python (.py)
 *   - Makefile (Makefile, makefile, *.mk)
 */

#include "syntax.h"
#include "string.h"

/* ── Keyword tables ───────────────────────────────────────────────── */

/* C / C++ keywords */
static const char *c_keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do",
    "else", "enum", "extern", "for", "goto", "if", "inline",
    "register", "return", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "volatile", "while",
    /* C11 keywords */
    "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn",
    "_Static_assert", "_Thread_local",
    NULL
};

/* C / C++ type names */
static const char *c_types[] = {
    "char", "double", "float", "int", "long", "short",
    "unsigned", "void", "size_t", "ssize_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "bool", "intptr_t", "uintptr_t", "off_t", "pid_t", "uid_t", "gid_t",
    "FILE", "va_list",
    NULL
};

/* Assembly directives / keywords */
static const char *asm_keywords[] = {
    "section", "global", "extern", "bits", "org", "align", "times",
    "db", "dw", "dd", "dq", "resb", "resw", "resd", "resq",
    "incbin", "equ", "macro", "endm", "struc", "endstruc", "istruc",
    "at", "iend", "%define", "%macro", "%endm", "%if", "%else",
    "%endif", "%include", "cpu", "default", "absolute",
    NULL
};

/* Shell keywords */
static const char *shell_keywords[] = {
    "if", "then", "elif", "else", "fi", "for", "while", "until",
    "do", "done", "case", "esac", "in", "function", "return",
    "break", "continue", "exit", "export", "local", "readonly",
    "select", "time", "shift", "source", "trap", "unset", "eval",
    "exec", "let", "declare", "typeset",
    NULL
};

/* Python keywords */
static const char *py_keywords[] = {
    "False", "None", "True", "and", "as", "assert", "async",
    "await", "break", "class", "continue", "def", "del", "elif",
    "else", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "nonlocal", "not", "or",
    "pass", "raise", "return", "try", "while", "with", "yield",
    NULL
};

/* Python built-in function names */
static const char *py_builtins[] = {
    "print", "len", "range", "int", "str", "float", "list",
    "dict", "set", "tuple", "type", "open", "input", "super",
    "map", "filter", "zip", "enumerate", "sorted", "reversed",
    "abs", "all", "any", "bin", "bool", "bytearray", "bytes",
    "chr", "complex", "dir", "divmod", "eval", "exec",
    "format", "frozenset", "getattr", "globals", "hasattr",
    "hash", "hex", "id", "isinstance", "issubclass", "iter",
    "locals", "max", "min", "next", "object", "oct", "ord",
    "pow", "property", "repr", "round", "setattr", "slice",
    "staticmethod", "sum", "vars", "zip",
    NULL
};

/* Makefile special variables / keywords */
static const char *make_keywords[] = {
    "ifdef", "ifndef", "ifeq", "ifneq", "else", "endif",
    "define", "endef", "override", "export", "unexport",
    "include", "-include", "sinclude", "vpath",
    NULL
};

/* ── Helper: check if character is alphanumeric or underscore ─────── */
static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

/* ── Helper: lookup word in NULL-terminated string table ──────────── */
static int in_table(const char *word, const char **table) {
    for (int i = 0; table[i] != NULL; i++) {
        const char *k = table[i];
        const char *w = word;
        while (*k && *k == *w) { k++; w++; }
        if (*k == '\0' && *w == '\0') return 1;
    }
    return 0;
}

/* ── Language detection from filename ─────────────────────────────── */
syntax_lang_t syntax_detect(const char *filename) {
    if (!filename || filename[0] == '\0')
        return SYNTAX_NONE;

    /* Find last dot */
    const char *dot = NULL;
    const char *p = filename;
    while (*p) {
        if (*p == '.') dot = p;
        p++;
    }

    /* Also check basename for Makefile */
    const char *base = filename;
    p = filename;
    while (*p) {
        if (*p == '/') base = p + 1;
        p++;
    }

    if (base[0] == 'M' && strcmp(base, "Makefile") == 0)
        return SYNTAX_MAKEFILE;
    if (base[0] == 'm' && strcmp(base, "makefile") == 0)
        return SYNTAX_MAKEFILE;
    if (base[0] == 'G' && strcmp(base, "GNUmakefile") == 0)
        return SYNTAX_MAKEFILE;

    if (!dot) return SYNTAX_NONE;

    if (strcmp(dot, ".c")    == 0 ||
        strcmp(dot, ".h")    == 0 ||
        strcmp(dot, ".cpp")  == 0 ||
        strcmp(dot, ".hpp")  == 0 ||
        strcmp(dot, ".cxx")  == 0 ||
        strcmp(dot, ".hxx")  == 0 ||
        strcmp(dot, ".cc")   == 0 ||
        strcmp(dot, ".hh")   == 0)
        return SYNTAX_C;

    if (strcmp(dot, ".asm")  == 0 ||
        strcmp(dot, ".s")    == 0 ||
        strcmp(dot, ".S")    == 0 ||
        strcmp(dot, ".inc")  == 0)
        return SYNTAX_ASM;

    if (strcmp(dot, ".sh")   == 0)
        return SYNTAX_SHELL;

    if (strcmp(dot, ".py")   == 0)
        return SYNTAX_PYTHON;

    if (strcmp(dot, ".mk")   == 0)
        return SYNTAX_MAKEFILE;

    return SYNTAX_NONE;
}

/* ── Per-language tokenizers ──────────────────────────────────────── */

static int tokenize_c(const char *line, int len,
                      syntax_token_t *tokens, int *in_multi) {
    int i = 0;
    int multi = *in_multi;

    while (i < len) {
        /* Handle multi-line comment state */
        if (multi) {
            int multi_end = 0;
            while (i < len) {
                if (i + 1 < len && line[i] == '*' && line[i + 1] == '/') {
                    tokens[i] = TOKEN_COMMENT;
                    tokens[i + 1] = TOKEN_COMMENT;
                    i += 2;
                    multi = 0;
                    multi_end = 1;
                    break;
                }
                tokens[i] = TOKEN_COMMENT;
                i++;
            }
            if (!multi_end) {
                *in_multi = multi;
                return len;
            }
            continue;
        }

        /* Single-line comment // */
        if (i + 1 < len && line[i] == '/' && line[i + 1] == '/') {
            while (i < len) tokens[i++] = TOKEN_COMMENT;
            *in_multi = 0;
            return len;
        }

        /* Multi-line comment start marker */
        if (i + 1 < len && line[i] == '/' && line[i + 1] == '*') {
            tokens[i] = TOKEN_COMMENT;
            tokens[i + 1] = TOKEN_COMMENT;
            i += 2;
            multi = 1;
            while (i < len) {
                if (i + 1 < len && line[i] == '*' && line[i + 1] == '/') {
                    tokens[i] = TOKEN_COMMENT;
                    tokens[i + 1] = TOKEN_COMMENT;
                    i += 2;
                    multi = 0;
                    break;
                }
                tokens[i] = TOKEN_COMMENT;
                i++;
            }
            continue;
        }

        /* Preprocessor directive # */
        if (line[i] == '#' && (i == 0 || line[i-1] == '\n' || line[i-1] == '\r')) {
            while (i < len && line[i] == ' ') {
                tokens[i] = TOKEN_PREPROC;
                i++;
            }
            while (i < len && is_ident_start(line[i])) {
                tokens[i++] = TOKEN_PREPROC;
            }
            /* Rest of line is also preprocessor (may include string literals) */
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_PREPROC;
                    tokens[i + 1] = TOKEN_PREPROC;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_PREPROC;
                }
            }
            *in_multi = 0;
            return len;
        }

        /* String literal */
        if (line[i] == '"') {
            tokens[i++] = TOKEN_STRING;
            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_STRING;
                    tokens[i + 1] = TOKEN_STRING;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_STRING;
                }
            }
            if (i < len) tokens[i++] = TOKEN_STRING;
            continue;
        }

        /* Character literal */
        if (line[i] == '\'') {
            tokens[i++] = TOKEN_CHAR;
            while (i < len && line[i] != '\'') {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_CHAR;
                    tokens[i + 1] = TOKEN_CHAR;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_CHAR;
                }
            }
            if (i < len) tokens[i++] = TOKEN_CHAR;
            continue;
        }

        /* Identifier or keyword */
        if (is_ident_start(line[i]) || line[i] == '_') {
            int start = i;
            while (i < len && is_word_char(line[i])) i++;
            int wlen = i - start;
            if (wlen <= 32) {
                char word[33];
                int j;
                for (j = 0; j < wlen; j++) word[j] = line[start + j];
                word[j] = '\0';

                syntax_token_t t = TOKEN_DEFAULT;
                if (in_table(word, c_keywords))
                    t = TOKEN_KEYWORD;
                else if (in_table(word, c_types))
                    t = TOKEN_TYPE;

                for (j = start; j < i; j++)
                    tokens[j] = t;
            } else {
                for (int j = start; j < i; j++)
                    tokens[j] = TOKEN_DEFAULT;
            }
            continue;
        }

        /* Number literal */
        if (line[i] >= '0' && line[i] <= '9') {
            while (i < len && ((line[i] >= '0' && line[i] <= '9') ||
                                line[i] == '.' || line[i] == 'x' ||
                                line[i] == 'X' || line[i] == 'a' ||
                                line[i] == 'b' || line[i] == 'c' ||
                                line[i] == 'd' || line[i] == 'e' ||
                                line[i] == 'f' || line[i] == 'A' ||
                                line[i] == 'B' || line[i] == 'C' ||
                                line[i] == 'D' || line[i] == 'E' ||
                                line[i] == 'F' || line[i] == 'u' ||
                                line[i] == 'U' || line[i] == 'l' ||
                                line[i] == 'L')) {
                tokens[i++] = TOKEN_NUMBER;
            }
            continue;
        }

        /* Operators */
        if (line[i] == '+' || line[i] == '-' || line[i] == '*' ||
            line[i] == '/' || line[i] == '%' || line[i] == '=' ||
            line[i] == '<' || line[i] == '>' || line[i] == '!' ||
            line[i] == '&' || line[i] == '|' || line[i] == '^' ||
            line[i] == '~' || line[i] == '?' || line[i] == ':') {
            tokens[i++] = TOKEN_OPERATOR;
            continue;
        }

        /* Default: skip */
        tokens[i] = TOKEN_DEFAULT;
        i++;
    }

    *in_multi = multi;
    return len;
}

static int tokenize_asm(const char *line, int len,
                        syntax_token_t *tokens, int *in_multi) {
    (void)in_multi;
    int i = 0;

    while (i < len) {
        /* Comment: ; or // */
        if (line[i] == ';' || (i + 1 < len && line[i] == '/' && line[i + 1] == '/')) {
            while (i < len) tokens[i++] = TOKEN_COMMENT;
            return len;
        }

        /* String */
        if (line[i] == '"') {
            tokens[i++] = TOKEN_STRING;
            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_STRING;
                    tokens[i + 1] = TOKEN_STRING;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_STRING;
                }
            }
            if (i < len) tokens[i++] = TOKEN_STRING;
            continue;
        }

        /* Identifier or keyword */
        if (is_ident_start(line[i])) {
            int start = i;
            while (i < len && is_word_char(line[i])) i++;
            int wlen = i - start;
            if (wlen <= 32) {
                char word[33];
                int j;
                for (j = 0; j < wlen; j++) word[j] = line[start + j];
                word[j] = '\0';

                syntax_token_t t = TOKEN_DEFAULT;
                /* Check if starts with '.' (label/directive) */
                if (word[0] == '.')
                    t = TOKEN_PREPROC;
                else if (in_table(word, asm_keywords))
                    t = TOKEN_KEYWORD;
                /* Uppercase identifiers (registers, instructions) */
                else if (word[0] >= 'A' && word[0] <= 'Z')
                    t = TOKEN_BUILTIN;

                for (j = start; j < i; j++)
                    tokens[j] = t;
            } else {
                for (int j = start; j < i; j++)
                    tokens[j] = TOKEN_DEFAULT;
            }
            continue;
        }

        /* Number */
        if (line[i] >= '0' && line[i] <= '9') {
            while (i < len && ((line[i] >= '0' && line[i] <= '9') ||
                                line[i] == 'x' || line[i] == 'X' ||
                                line[i] == 'a' || line[i] == 'b' ||
                                line[i] == 'c' || line[i] == 'd' ||
                                line[i] == 'e' || line[i] == 'f' ||
                                line[i] == 'A' || line[i] == 'B' ||
                                line[i] == 'C' || line[i] == 'D' ||
                                line[i] == 'E' || line[i] == 'F' ||
                                line[i] == 'h' || line[i] == 'H' ||
                                line[i] == 'o' || line[i] == 'O' ||
                                line[i] == 'b' || line[i] == 'B')) {
                tokens[i++] = TOKEN_NUMBER;
            }
            continue;
        }

        tokens[i] = TOKEN_DEFAULT;
        i++;
    }
    return len;
}

static int tokenize_shell(const char *line, int len,
                          syntax_token_t *tokens, int *in_multi) {
    (void)in_multi;
    int i = 0;

    while (i < len) {
        /* Comment # */
        if (line[i] == '#' &&
            (i == 0 || line[i-1] == ' ' || line[i-1] == '\t' ||
             line[i-1] == '\n' || line[i-1] == ';' || line[i-1] == '|' ||
             line[i-1] == '&' || line[i-1] == '(')) {
            while (i < len) tokens[i++] = TOKEN_COMMENT;
            return len;
        }

        /* String (double quoted) */
        if (line[i] == '"') {
            tokens[i++] = TOKEN_STRING;
            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_STRING;
                    tokens[i + 1] = TOKEN_STRING;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_STRING;
                }
            }
            if (i < len) tokens[i++] = TOKEN_STRING;
            continue;
        }

        /* String (single quoted — no escape) */
        if (line[i] == '\'') {
            tokens[i++] = TOKEN_STRING;
            while (i < len && line[i] != '\'') tokens[i++] = TOKEN_STRING;
            if (i < len) tokens[i++] = TOKEN_STRING;
            continue;
        }

        /* Variable expansion $ */
        if (line[i] == '$' && i + 1 < len) {
            tokens[i++] = TOKEN_BUILTIN;
            if (line[i] == '{' || line[i] == '(') {
                tokens[i++] = TOKEN_BUILTIN;
                while (i < len && line[i] != '}' && line[i] != ')') {
                    tokens[i++] = TOKEN_BUILTIN;
                }
                if (i < len) tokens[i++] = TOKEN_BUILTIN;
            } else if (is_ident_start(line[i])) {
                while (i < len && is_word_char(line[i]))
                    tokens[i++] = TOKEN_BUILTIN;
            } else {
                tokens[i++] = TOKEN_BUILTIN;
            }
            continue;
        }

        /* Backtick command substitution */
        if (line[i] == '`') {
            tokens[i++] = TOKEN_BUILTIN;
            while (i < len && line[i] != '`') {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_BUILTIN;
                    tokens[i + 1] = TOKEN_BUILTIN;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_BUILTIN;
                }
            }
            if (i < len) tokens[i++] = TOKEN_BUILTIN;
            continue;
        }

        /* Identifier or keyword */
        if (is_ident_start(line[i])) {
            int start = i;
            while (i < len && is_word_char(line[i])) i++;
            int wlen = i - start;
            if (wlen <= 32) {
                char word[33];
                int j;
                for (j = 0; j < wlen; j++) word[j] = line[start + j];
                word[j] = '\0';

                syntax_token_t t = TOKEN_DEFAULT;
                if (in_table(word, shell_keywords))
                    t = TOKEN_KEYWORD;

                for (j = start; j < i; j++)
                    tokens[j] = t;
            } else {
                for (int j = start; j < i; j++)
                    tokens[j] = TOKEN_DEFAULT;
            }
            continue;
        }

        tokens[i] = TOKEN_DEFAULT;
        i++;
    }
    return len;
}

static int tokenize_python(const char *line, int len,
                           syntax_token_t *tokens, int *in_multi) {
    int i = 0;
    int multi = *in_multi;

    while (i < len) {
        /* Multi-line string (''' or """) */
        if (multi) {
            int multi_end = 0;
            const char *delim = (multi == 1) ? "'''" : "\"\"\"";
            int dlen = 3;
            while (i < len) {
                if (i + dlen <= len &&
                    line[i] == delim[0] && line[i+1] == delim[1] && line[i+2] == delim[2]) {
                    for (int k = 0; k < dlen; k++)
                        tokens[i + k] = TOKEN_STRING;
                    i += dlen;
                    multi = 0;
                    multi_end = 1;
                    break;
                }
                tokens[i] = TOKEN_STRING;
                i++;
            }
            if (!multi_end) {
                *in_multi = multi;
                return len;
            }
            continue;
        }

        /* Single-line comment # */
        if (line[i] == '#') {
            /* Check it's not inside a string */
            int in_string = 0;
            for (int j = 0; j < i; j++) {
                if (line[j] == '"' || line[j] == '\'') {
                    char quote = line[j];
                    j++;
                    while (j < i && line[j] != quote) {
                        if (line[j] == '\\') j++;
                        j++;
                    }
                    if (j >= i) in_string = !in_string;
                }
            }
            if (!in_string) {
                while (i < len) tokens[i++] = TOKEN_COMMENT;
                return len;
            }
        }

        /* Triple-quoted string start */
        if (i + 2 < len && line[i] == '\'' && line[i+1] == '\'' && line[i+2] == '\'') {
            tokens[i] = tokens[i+1] = tokens[i+2] = TOKEN_STRING;
            i += 3;
            multi = 1;
            while (i < len) {
                if (i + 2 < len && line[i] == '\'' && line[i+1] == '\'' && line[i+2] == '\'') {
                    tokens[i] = tokens[i+1] = tokens[i+2] = TOKEN_STRING;
                    i += 3;
                    multi = 0;
                    break;
                }
                tokens[i++] = TOKEN_STRING;
            }
            if (multi) { *in_multi = multi; return len; }
            continue;
        }

        if (i + 2 < len && line[i] == '"' && line[i+1] == '"' && line[i+2] == '"') {
            tokens[i] = tokens[i+1] = tokens[i+2] = TOKEN_STRING;
            i += 3;
            multi = 2;
            while (i < len) {
                if (i + 2 < len && line[i] == '"' && line[i+1] == '"' && line[i+2] == '"') {
                    tokens[i] = tokens[i+1] = tokens[i+2] = TOKEN_STRING;
                    i += 3;
                    multi = 0;
                    break;
                }
                tokens[i++] = TOKEN_STRING;
            }
            if (multi) { *in_multi = multi; return len; }
            continue;
        }

        /* String (single or double quoted) */
        if (line[i] == '"') {
            tokens[i++] = TOKEN_STRING;
            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_STRING;
                    tokens[i + 1] = TOKEN_STRING;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_STRING;
                }
            }
            if (i < len) tokens[i++] = TOKEN_STRING;
            continue;
        }

        if (line[i] == '\'') {
            tokens[i++] = TOKEN_STRING;
            while (i < len && line[i] != '\'') {
                if (line[i] == '\\' && i + 1 < len) {
                    tokens[i] = TOKEN_STRING;
                    tokens[i + 1] = TOKEN_STRING;
                    i += 2;
                } else {
                    tokens[i++] = TOKEN_STRING;
                }
            }
            if (i < len) tokens[i++] = TOKEN_STRING;
            continue;
        }

        /* f-string or raw string prefix (fr, rf, f, r, b) */
        if ((line[i] == 'f' || line[i] == 'r' || line[i] == 'b') &&
            i + 1 < len && (line[i+1] == '"' || line[i+1] == '\'')) {
            tokens[i] = TOKEN_BUILTIN;
            i++;
            continue;
        }

        /* Identifier or keyword */
        if (is_ident_start(line[i])) {
            int start = i;
            while (i < len && is_word_char(line[i])) i++;
            int wlen = i - start;
            if (wlen <= 32) {
                char word[33];
                int j;
                for (j = 0; j < wlen; j++) word[j] = line[start + j];
                word[j] = '\0';

                syntax_token_t t = TOKEN_DEFAULT;
                if (in_table(word, py_keywords))
                    t = TOKEN_KEYWORD;
                else if (in_table(word, py_builtins))
                    t = TOKEN_BUILTIN;
                /* Types: first letter uppercase */
                else if (word[0] >= 'A' && word[0] <= 'Z')
                    t = TOKEN_TYPE;

                for (j = start; j < i; j++)
                    tokens[j] = t;
            } else {
                for (int j = start; j < i; j++)
                    tokens[j] = TOKEN_DEFAULT;
            }
            continue;
        }

        /* Number */
        if (line[i] >= '0' && line[i] <= '9') {
            while (i < len && ((line[i] >= '0' && line[i] <= '9') ||
                                line[i] == '.' || line[i] == 'x' ||
                                line[i] == 'X' || line[i] == 'o' ||
                                line[i] == 'O' || line[i] == 'b' ||
                                line[i] == 'B' || line[i] == 'e' ||
                                line[i] == 'E' || line[i] == 'j' ||
                                line[i] == 'J')) {
                tokens[i++] = TOKEN_NUMBER;
            }
            continue;
        }

        tokens[i] = TOKEN_DEFAULT;
        i++;
    }

    *in_multi = multi;
    return len;
}

static int tokenize_makefile(const char *line, int len,
                             syntax_token_t *tokens, int *in_multi) {
    (void)in_multi;
    int i = 0;
    int in_variable = 0;
    int paren_depth = 0;
    int in_target = 0;

    /* Check if line starts with a target (no leading whitespace, ends with ':') */
    int is_target = 0;
    if (len > 0 && line[0] != '\t' && line[0] != ' ' && line[0] != '#') {
        int j = 0;
        while (j < len && line[j] != ':' && line[j] != ' ' && line[j] != '\t' && line[j] != '#') j++;
        if (j < len && line[j] == ':') is_target = 1;
    }

    while (i < len) {
        /* Comment */
        if (line[i] == '#') {
            while (i < len) tokens[i++] = TOKEN_COMMENT;
            return len;
        }

        /* Target line */
        if (is_target) {
            tokens[i++] = TOKEN_TYPE;
            continue;
        }

        /* Recipe line (starts with tab) */
        if (i == 0 && line[i] == '\t') {
            tokens[i++] = TOKEN_DEFAULT;
            continue;
        }

        /* Variable reference $(...) or ${...} */
        if (line[i] == '$' && i + 1 < len) {
            if (line[i + 1] == '(' || line[i + 1] == '{') {
                tokens[i] = TOKEN_BUILTIN;
                i++;
                char expected_close = (line[i] == '(') ? ')' : '}';
                tokens[i] = TOKEN_BUILTIN;
                i++;
                in_variable = 1;
                paren_depth = 1;
                continue;
            }
            if (line[i + 1] == '$') {
                tokens[i] = tokens[i + 1] = TOKEN_BUILTIN;
                i += 2;
                continue;
            }
            if (line[i + 1] == '@' || line[i + 1] == '<' || line[i + 1] == '^' ||
                line[i + 1] == '*' || line[i + 1] == '%' || line[i + 1] == '?' ||
                line[i + 1] == '+' || line[i + 1] == '|') {
                tokens[i] = TOKEN_BUILTIN;
                tokens[i + 1] = TOKEN_BUILTIN;
                i += 2;
                continue;
            }
        }

        if (in_variable) {
            tokens[i] = TOKEN_BUILTIN;
            if (line[i] == '(' || line[i] == '{') paren_depth++;
            if (line[i] == ')' || line[i] == '}') {
                paren_depth--;
                if (paren_depth == 0) {
                    in_variable = 0;
                }
            }
            i++;
            continue;
        }

        /* Variable assignment (:=, =, +=, ?=) */
        {
            int j = i;
            while (j < len && line[j] != ':' && line[j] != '+' &&
                   line[j] != '?' && line[j] != '=' && line[j] != ' ') j++;
            if (j < len && (line[j] == ':' || line[j] == '+' || line[j] == '?') &&
                j + 1 < len && line[j + 1] == '=') {
                /* Left side is variable name */
                for (int k = i; k <= j + 1; k++)
                    tokens[k] = (k <= j) ? TOKEN_BUILTIN : TOKEN_OPERATOR;
                i = j + 2;
                continue;
            }
            if (j < len && line[j] == '=') {
                for (int k = i; k <= j; k++)
                    tokens[k] = (k < j) ? TOKEN_BUILTIN : TOKEN_OPERATOR;
                i = j + 1;
                continue;
            }
        }

        /* Keyword (ifeq, ifdef, include, etc.) at line start */
        if (is_ident_start(line[i]) || line[i] == '-') {
            int start = i;
            if (line[i] == '-') i++;
            while (i < len && is_word_char(line[i])) i++;
            int wlen = i - start;
            if (wlen <= 16) {
                char word[17];
                int j;
                for (j = 0; j < wlen; j++) word[j] = line[start + j];
                word[j] = '\0';
                if (in_table(word, make_keywords)) {
                    for (j = start; j < i; j++)
                        tokens[j] = TOKEN_KEYWORD;
                    continue;
                }
            }
            for (int j = start; j < i; j++)
                tokens[j] = TOKEN_DEFAULT;
            continue;
        }

        tokens[i] = TOKEN_DEFAULT;
        i++;
    }
    return len;
}

/* ── Main tokenization dispatcher ─────────────────────────────────── */

int syntax_tokenize(syntax_lang_t lang, const char *line, int line_len,
                    syntax_token_t *tokens, int *in_multi) {
    if (!line || line_len <= 0 || !tokens || !in_multi)
        return 0;

    if (line_len > SYN_LINE_MAX)
        line_len = SYN_LINE_MAX;

    /* Initialize all tokens to default */
    for (int i = 0; i < line_len; i++)
        tokens[i] = TOKEN_DEFAULT;

    switch (lang) {
        case SYNTAX_C:
            return tokenize_c(line, line_len, tokens, in_multi);
        case SYNTAX_ASM:
            return tokenize_asm(line, line_len, tokens, in_multi);
        case SYNTAX_SHELL:
            return tokenize_shell(line, line_len, tokens, in_multi);
        case SYNTAX_PYTHON:
            return tokenize_python(line, line_len, tokens, in_multi);
        case SYNTAX_MAKEFILE:
            return tokenize_makefile(line, line_len, tokens, in_multi);
        default:
            return line_len;
    }
}

/* ── Token-to-ANSI conversion ─────────────────────────────────────── */

const char *syntax_token_to_ansi(syntax_token_t t) {
    switch (t) {
        case TOKEN_KEYWORD:  return SYN_ANSI_KEYWORD;
        case TOKEN_TYPE:     return SYN_ANSI_TYPE;
        case TOKEN_STRING:   return SYN_ANSI_STRING;
        case TOKEN_COMMENT:  return SYN_ANSI_COMMENT;
        case TOKEN_PREPROC:  return SYN_ANSI_PREPROC;
        case TOKEN_NUMBER:   return SYN_ANSI_NUMBER;
        case TOKEN_OPERATOR: return SYN_ANSI_OPERATOR;
        case TOKEN_CHAR:     return SYN_ANSI_CHAR;
        case TOKEN_BUILTIN:  return SYN_ANSI_BUILTIN;
        default:             return SYN_ANSI_DEFAULT;
    }
}
