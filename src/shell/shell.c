/* shell.c — Shell core: input loop, dispatch, history */

#include "shell.h"
#include "shell_cmds.h"
#include "shell_cmd_table.h"
#include "vga.h"
#include "keyboard.h"
#include "printf.h"
#include "string.h"
#include "serial.h"
#include "editor.h"
#include "vfs.h"
#include "fs.h"
#include "process.h"
#include "scheduler.h"
#include "vermagic.h"

/* Forward declaration for fnmatch (defined in lib/stdlib.c) */
int fnmatch(const char *pattern, const char *string, int flags);

#define MAX_VAR_NAME 32
#define MAX_VAR_VALUE 128

/* ─── Alias table ──────────────────────────────────────────────────────────── */
#define ALIAS_MAX 32
#define ALIAS_NAME_LEN 32
#define ALIAS_VAL_LEN  128

struct alias_entry {
    char name[ALIAS_NAME_LEN];
    char value[ALIAS_VAL_LEN];
    int  used;
};
static struct alias_entry aliases[ALIAS_MAX];

void shell_alias_set(const char *name, const char *value) {
    /* update existing */
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].used && strcmp(aliases[i].name, name) == 0) {
            strncpy(aliases[i].value, value, ALIAS_VAL_LEN - 1);
            aliases[i].value[ALIAS_VAL_LEN - 1] = '\0';
            return;
        }
    }
    /* add new */
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (!aliases[i].used) {
            strncpy(aliases[i].name,  name,  ALIAS_NAME_LEN - 1);
            aliases[i].name[ALIAS_NAME_LEN - 1] = '\0';
            strncpy(aliases[i].value, value, ALIAS_VAL_LEN - 1);
            aliases[i].value[ALIAS_VAL_LEN - 1] = '\0';
            aliases[i].used = 1;
            return;
        }
    }
}
void shell_alias_del(const char *name) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].used && strcmp(aliases[i].name, name) == 0) {
            aliases[i].used = 0; return;
        }
    }
}
const char *shell_alias_get(const char *name) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].used && strcmp(aliases[i].name, name) == 0)
            return aliases[i].value;
    }
    return NULL;
}
void shell_alias_list(void) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].used)
            kprintf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
    }
}

/* ─── Glob expansion ─────────────────────────────────────────────────────────── */

/* Returns 1 if pattern matches string (supports * and ?) */
static int glob_match_depth(const char *pat, const char *str, int depth);

#define GLOB_MAX_DEPTH 32

static int glob_match(const char *pat, const char *str) {
    return glob_match_depth(pat, str, 0);
}

static int glob_match_depth(const char *pat, const char *str, int depth) {
    if (depth > GLOB_MAX_DEPTH) return 0;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1; /* trailing * matches everything */
            while (*str) {
                if (glob_match_depth(pat, str, depth + 1)) return 1;
                str++;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
        } else {
            if (*pat != *str) return 0;
            pat++; str++;
        }
    }
    return *str == '\0';
}

/*
 * Expand glob tokens in `line` (space-separated words) into `out`.
 * If a word contains * or ?, list all matching filesystem names.
 * Words without wildcards are copied verbatim.
 */
#define GLOB_MAX_MATCHES 32
static void glob_expand_line(const char *line, char *out, int out_max) {
    int oi = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' && oi < out_max - 1) out[oi++] = *p++;
        if (!*p) break;
        /* Collect the next word */
        char word[128]; int wi = 0;
        while (*p && *p != ' ' && wi < 127) word[wi++] = *p++;
        word[wi] = '\0';
        if (!wi) break;
        /* Does the word contain a wildcard? */
        int has_glob = 0;
        for (int i = 0; i < wi; i++) {
            if (word[i] == '*' || word[i] == '?') { has_glob = 1; break; }
        }
        /* Compute last_slash early so the bare-wildcard check can use it */
        int last_slash = -1;
        if (has_glob) {
            for (int i = wi - 1; i >= 0; i--) {
                if (word[i] == '/') { last_slash = i; break; }
            }
        }
        if (!has_glob) {
            /* No wildcard — copy verbatim */
            for (int i = 0; i < wi && oi < out_max - 1; i++) out[oi++] = word[i];
        } else if (last_slash < 0 && wi == 1) {
            /* Bare single-character wildcard (* or ?) with no path prefix:
             * leave as-is to avoid expanding math operators like 'expr 6 * 7' */
            for (int i = 0; i < wi && oi < out_max - 1; i++) out[oi++] = word[i];
        } else {
            /* Determine dir prefix and pattern name */
            char dir[64]; char pat[64];
            /* last_slash already computed above */
            if (last_slash < 0) {
                /* No slash: search in root */
                strncpy(dir, "/", sizeof(dir)); dir[sizeof(dir)-1] = '\0';
                strncpy(pat, word, sizeof(pat)); pat[sizeof(pat)-1] = '\0';
            } else {
                int dl = last_slash;
                if (dl == 0) dl = 1; /* keep leading / */
                if (dl >= 63) dl = 63;
                memcpy(dir, word, dl); dir[dl] = '\0';
                strncpy(pat, word + last_slash + 1, sizeof(pat)); pat[sizeof(pat)-1] = '\0';
            }
            /* Determine prefix (everything up to first wildcard in pat) */
            char prefix[64]; int pi = 0;
            for (; pat[pi] && pat[pi] != '*' && pat[pi] != '?' && pi < 63; pi++)
                prefix[pi] = pat[pi];
            prefix[pi] = '\0';
            /* List files with that prefix */
            char gnames[GLOB_MAX_MATCHES][FS_MAX_NAME];
            int n = fs_list_names(dir, prefix[0] ? prefix : NULL, gnames, GLOB_MAX_MATCHES);
            int matched = 0;
            for (int i = 0; i < n; i++) {
                if (glob_match(pat, gnames[i])) {
                    if (matched > 0 && oi < out_max - 1) out[oi++] = ' ';
                    /* Reconstruct full path */
                    char full[128]; int fi = 0;
                    if (last_slash >= 0) {
                        /* prepend dir */
                        int dl = strlen(dir);
                        if (dl < 126) { memcpy(full, dir, dl); fi = dl; }
                        /* cppcheck-suppress negativeIndex */
                        if (fi < 126 && dir[fi-1] != '/') full[fi++] = '/';
                    }
                    int nl = strlen(gnames[i]);
                    for (int j = 0; j < nl && fi < 127; j++) full[fi++] = gnames[i][j];
                    full[fi] = '\0';
                    for (int j = 0; j < fi && oi < out_max - 1; j++) out[oi++] = full[j];
                    matched++;
                }
            }
            if (!matched) {
                /* No match: leave pattern as-is (bash behaviour) */
                for (int i = 0; i < wi && oi < out_max - 1; i++) out[oi++] = word[i];
            }
        }
    }
    out[oi] = '\0';
}

/* Exit status of the most recently executed command */
static int last_exit_status = 0;

void shell_set_exit_status(int s) { last_exit_status = s; }
int  shell_get_exit_status(void)  { return last_exit_status; }

/* Helper for pipe/redirect output capture via kprintf hook */
struct shell_capture_ctx {
    char *buf;
    int  *len;
    int   max;    /* buffer capacity (buf[max] is the NUL terminator slot) */
};

static void shell_capture_cb(char c, void *ctx) {
    struct shell_capture_ctx *sc = (struct shell_capture_ctx *)ctx;
    if (*sc->len < sc->max) sc->buf[(*sc->len)++] = c;
}

/* ── Shell stdin (pipe input) ───────────────────────────────────── */
static const char *g_stdin_buf = NULL;
static int         g_stdin_len = 0;
static int         g_stdin_pos = 0;

void shell_set_stdin(const char *buf, int len) {
    g_stdin_buf = buf;
    g_stdin_len = len;
    g_stdin_pos = 0;
}

void shell_clear_stdin(void) {
    g_stdin_buf = NULL;
    g_stdin_len = 0;
    g_stdin_pos = 0;
}

int shell_has_stdin(void) {
    return (g_stdin_buf != NULL && g_stdin_pos < g_stdin_len);
}

int shell_stdin_read(char *buf, int max) {
    if (!g_stdin_buf || g_stdin_pos >= g_stdin_len) return 0;
    int avail = g_stdin_len - g_stdin_pos;
    int n = (avail < max) ? avail : max;
    for (int i = 0; i < n; i++) buf[i] = g_stdin_buf[g_stdin_pos + i];
    g_stdin_pos += n;
    return n;
}

/* ── Arithmetic evaluator for $(( expr )) ───────────────────────── */
static const char *arith_p;

static int64_t arith_expr(void);   /* forward declaration */

static int64_t arith_primary(void) {
    while (*arith_p == ' ') arith_p++;
    int neg = 0;
    if (*arith_p == '-') { neg = 1; arith_p++; }
    else if (*arith_p == '+') { arith_p++; }
    if (*arith_p == '(') {
        arith_p++;
        int64_t v = arith_expr();
        if (*arith_p == ')') arith_p++;
        return neg ? -v : v;
    }
    int64_t v = 0;
    while (*arith_p >= '0' && *arith_p <= '9')
        v = v * 10 + (*arith_p++ - '0');
    return neg ? -v : v;
}

static int64_t arith_term(void) {
    int64_t v = arith_primary();
    while (*arith_p == ' ') arith_p++;
    while (*arith_p == '*' || *arith_p == '/' || *arith_p == '%') {
        char op = *arith_p++;
        while (*arith_p == ' ') arith_p++;
        int64_t r = arith_primary();
        while (*arith_p == ' ') arith_p++;
        if (op == '*') v *= r;
        else if (op == '/' && r != 0) v /= r;
        else if (op == '%' && r != 0) v %= r;
    }
    return v;
}

static int64_t arith_expr(void) {
    int64_t v = arith_term();
    while (*arith_p == ' ') arith_p++;
    while (*arith_p == '+' || *arith_p == '-') {
        char op = *arith_p++;
        while (*arith_p == ' ') arith_p++;
        int64_t r = arith_term();
        while (*arith_p == ' ') arith_p++;
        if (op == '+') v += r;
        else            v -= r;
    }
    return v;
}

static int64_t arith_evaluate(const char *expr) {
    arith_p = expr;
    return arith_expr();
}

/* ── Shell function table ────────────────────────────────────────── */
#define SHELL_FUNC_MAX       8
#define SHELL_FUNC_NAME_MAX  32
#define SHELL_FUNC_BODY_MAX  1024

static struct {
    char name[SHELL_FUNC_NAME_MAX];
    char body[SHELL_FUNC_BODY_MAX];
    int  defined;
} shell_funcs[SHELL_FUNC_MAX];

static void shell_func_define(const char *name, const char *body) {
    for (int i = 0; i < SHELL_FUNC_MAX; i++) {
        if (!shell_funcs[i].defined || strcmp(shell_funcs[i].name, name) == 0) {
            strncpy(shell_funcs[i].name, name, SHELL_FUNC_NAME_MAX - 1);
            shell_funcs[i].name[SHELL_FUNC_NAME_MAX - 1] = '\0';
            strncpy(shell_funcs[i].body, body, SHELL_FUNC_BODY_MAX - 1);
            shell_funcs[i].body[SHELL_FUNC_BODY_MAX - 1] = '\0';
            shell_funcs[i].defined = 1;
            return;
        }
    }
}

static const char *shell_func_get(const char *name) {
    for (int i = 0; i < SHELL_FUNC_MAX; i++)
        if (shell_funcs[i].defined && strcmp(shell_funcs[i].name, name) == 0)
            return shell_funcs[i].body;
    return NULL;
}

/* ── Positional parameters for function calls ($1, $2, ..., $@, $#) ── */
#define FUNC_ARGS_MAX   32
#define FUNC_ARG_BUF_SZ 256

/* Storage for positional args during function execution */
static char *func_argv[FUNC_ARGS_MAX];
static int   func_argc;
static char  func_argbuf[FUNC_ARG_BUF_SZ];
static int   func_return_flag;   /* set to non-zero to trigger early return */
static int   func_in_call;       /* 1 if currently inside a function body */

/* Shift positional parameters left by N positions (default 1) */
static void func_shift(int n) {
    if (n <= 0) n = 1;
    if (n > func_argc) n = func_argc;
    int new_count = func_argc - n;
    /* Move remaining args and their storage down */
    int remaining_bytes = 0;
    for (int i = n; i < func_argc; i++)
        remaining_bytes += strlen(func_argv[i]) + 1;
    if (remaining_bytes > 0) {
        char tmp[FUNC_ARG_BUF_SZ];
        int pos = 0;
        for (int i = n; i < func_argc; i++) {
            int len = strlen(func_argv[i]);
            if (pos + len < FUNC_ARG_BUF_SZ) {
                memcpy(tmp + pos, func_argv[i], len + 1);
                func_argv[i - n] = tmp + pos;
                pos += len + 1;
            }
        }
        memcpy(func_argbuf, tmp, pos);
        func_argc = new_count;
    } else {
        func_argc = 0;
    }
}

/* ── Shell array table ───────────────────────────────────────────── */
#define SHELL_ARRAY_MAX      8
#define SHELL_ARRAY_NAME_MAX 32
#define SHELL_ARRAY_ELEM_MAX 16
#define SHELL_ARRAY_ELEM_SZ  64

struct shell_array {
    char name[SHELL_ARRAY_NAME_MAX];
    char elems[SHELL_ARRAY_ELEM_MAX][SHELL_ARRAY_ELEM_SZ];
    int  count;
    int  defined;
};

static struct shell_array shell_arrays[SHELL_ARRAY_MAX];

static struct shell_array *shell_array_get(const char *name) {
    for (int i = 0; i < SHELL_ARRAY_MAX; i++)
        if (shell_arrays[i].defined && strcmp(shell_arrays[i].name, name) == 0)
            return &shell_arrays[i];
    return NULL;
}

static void shell_array_define(const char *name, const char *list) {
    int slot = -1;
    for (int i = 0; i < SHELL_ARRAY_MAX; i++) {
        if (!shell_arrays[i].defined) { if (slot < 0) slot = i; }
        else if (strcmp(shell_arrays[i].name, name) == 0) { slot = i; break; }
    }
    if (slot < 0) return;
    strncpy(shell_arrays[slot].name, name, SHELL_ARRAY_NAME_MAX - 1);
    shell_arrays[slot].name[SHELL_ARRAY_NAME_MAX - 1] = '\0';
    shell_arrays[slot].count = 0;
    shell_arrays[slot].defined = 1;
    /* Parse space-separated words */
    const char *p = list;
    while (*p == ' ' || *p == '(') p++;
    while (*p && *p != ')' && shell_arrays[slot].count < SHELL_ARRAY_ELEM_MAX) {
        while (*p == ' ') p++;
        if (!*p || *p == ')') break;
        int ei = 0;
        while (*p && *p != ' ' && *p != ')' && ei < SHELL_ARRAY_ELEM_SZ - 1)
            shell_arrays[slot].elems[shell_arrays[slot].count][ei++] = *p++;
        shell_arrays[slot].elems[shell_arrays[slot].count][ei] = '\0';
        shell_arrays[slot].count++;
    }
}

/* Expand $VAR and $? references in src into dst (dst_max includes NUL) */
static void var_expand(const char *src, char *dst, int dst_max) {
    int di = 0;
    int in_sq = 0; /* inside single quotes: no expansion */
    while (*src && di < dst_max - 1) {
        if (*src == '\'') {
            in_sq ^= 1;
            src++;
            continue;
        }
        if (*src == '$' && !in_sq) {
            src++;
            /* $((arith)) — arithmetic expansion */
            if (src[0] == '(' && src[1] == '(') {
                src += 2;
                char abuf[128]; int ai = 0;
                while (*src && ai < (int)sizeof(abuf) - 1) {
                    if (src[0] == ')' && src[1] == ')') { src += 2; break; }
                    abuf[ai++] = *src++;
                }
                abuf[ai] = '\0';
                int64_t result = arith_evaluate(abuf);
                /* convert to string */
                char numstr[24]; int ni = 0;
                int64_t r = result;
                if (r < 0) {
                    if (di < dst_max - 1) dst[di++] = '-';
                    if (r == (-9223372036854775807LL - 1)) {
                        char *ns = "-9223372036854775808";
                        while (*ns && di < dst_max - 1) dst[di++] = *ns++;
                        continue;
                    }
                    r = -r;
                }
                if (r == 0) { if (di < dst_max - 1) dst[di++] = '0'; }
                else {
                    while (r > 0 && ni < 22) { numstr[ni++] = '0' + (int)(r % 10); r /= 10; }
                    while (ni > 0 && di < dst_max - 1) dst[di++] = numstr[--ni];
                }
                continue;
            }
            /* $(cmd) — command substitution */
            if (*src == '(') {
                src++;
                char sub_cmd[256]; int si = 0;
                int depth = 1;
                while (*src && depth > 0 && si < (int)sizeof(sub_cmd) - 1) {
                    if (*src == '(') depth++;
                    else if (*src == ')') { if (--depth == 0) { src++; break; } }
                    sub_cmd[si++] = *src++;
                }
                sub_cmd[si] = '\0';
                /* Capture output of sub_cmd */
                char sub_out[512];
                int sub_len = 0;
                void (*sh_hook)(char, void *) = 0; void *sh_ctx = 0;
                kprintf_get_hook(&sh_hook, &sh_ctx);
                struct shell_capture_ctx sub_ctx = { sub_out, &sub_len, (int)sizeof(sub_out) - 1 };
                kprintf_set_hook(shell_capture_cb, &sub_ctx);
                /* Parse and exec sub_cmd */
                char sc_buf[256]; char *sc_cmd = sc_buf;
                strncpy(sc_buf, sub_cmd, sizeof(sc_buf) - 1); sc_buf[sizeof(sc_buf)-1] = '\0';
                while (*sc_cmd == ' ') sc_cmd++;
                char *sc_args = sc_cmd;
                while (*sc_args && *sc_args != ' ') sc_args++;
                if (*sc_args) { *sc_args = '\0'; sc_args++; while (*sc_args == ' ') sc_args++; }
                else sc_args = NULL;
                shell_exec_cmd(sc_cmd, sc_args);
                kprintf_set_hook(sh_hook, sh_ctx);
                /* Trim trailing newlines */
                while (sub_len > 0 && (sub_out[sub_len-1] == '\n' || sub_out[sub_len-1] == '\r')) sub_len--;
                sub_out[sub_len] = '\0';
                for (int xi = 0; xi < sub_len && di < dst_max - 1; xi++) dst[di++] = sub_out[xi];
                continue;
            }
            /* ${name[N]} — array element OR ${#name[@]} — array count */
            if (*src == '{') {
                src++;
                /* ${#name[@]} count */
                if (*src == '#') {
                    src++;
                    char aname[SHELL_ARRAY_NAME_MAX]; int ani = 0;
                    while (*src && *src != '[' && *src != '}' && ani < SHELL_ARRAY_NAME_MAX - 1)
                        aname[ani++] = *src++;
                    aname[ani] = '\0';
                    while (*src && *src != '}') src++;
                    if (*src == '}') src++;
                    struct shell_array *arr = shell_array_get(aname);
                    int cnt = arr ? arr->count : 0;
                    char numstr[12]; int ni = 0;
                    if (cnt == 0) { if (di < dst_max - 1) dst[di++] = '0'; }
                    else { while (cnt > 0 && ni < 10) { numstr[ni++] = '0' + cnt % 10; cnt /= 10; }
                           while (ni > 0 && di < dst_max - 1) dst[di++] = numstr[--ni]; }
                    continue;
                }
                char aname[SHELL_ARRAY_NAME_MAX]; int ani = 0;
                while (*src && *src != '[' && *src != '}' && ani < SHELL_ARRAY_NAME_MAX - 1)
                    aname[ani++] = *src++;
                aname[ani] = '\0';
                if (*src == '[') {
                    src++;
                    int idx = 0;
                    while (*src >= '0' && *src <= '9') {
                        int nd = *src++ - '0';
                        if (idx > (2147483647 - nd) / 10) break;
                        idx = idx * 10 + nd;
                    }
                    if (*src == ']') src++;
                    if (*src == '}') src++;
                    void *arrp = shell_array_get(aname);
                    if (arrp && idx >= 0 && idx < SHELL_ARRAY_ELEM_MAX) {
                        struct shell_array *a = (struct shell_array *)arrp;
                        if (idx < a->count) {
                            const char *ev = a->elems[idx];
                            while (*ev && di < dst_max - 1) dst[di++] = *ev++;
                        }
                    }
                    continue;
                }
                /* ${name} — regular variable in braces */
                if (*src == '}') src++;
                const char *val = shell_var_get(aname);
                if (val && *val) {
                    while (*val && di < dst_max - 1) dst[di++] = *val++;
                } else {
                    if (di < dst_max - 1) dst[di++] = '$';
                    if (di < dst_max - 1) dst[di++] = '{';
                    for (int i = 0; i < ani && di < dst_max - 1; i++) dst[di++] = aname[i];
                    if (di < dst_max - 1) dst[di++] = '}';
                }
                continue;
            }
            /* $? — last exit status */
            if (*src == '?') {
                src++;
                int n = last_exit_status;
                if (n == 0) {
                    if (di < dst_max - 1) dst[di++] = '0';
                } else {
                    char tmp[12]; int ti = 0;
                    int nn = (n < 0) ? -n : n;
                    if (n < 0 && di < dst_max - 1) dst[di++] = '-';
                    while (nn > 0) { tmp[ti++] = '0' + (nn % 10); nn /= 10; }
                    while (ti > 0 && di < dst_max - 1) dst[di++] = tmp[--ti];
                }
                continue;
            }
            /* $1..$9 — positional parameters (only expanded inside functions) */
            if (*src >= '1' && *src <= '9') {
                int idx = *src - '1';  /* 0-based index */
                src++;
                if (func_in_call && idx < func_argc) {
                    const char *v = func_argv[idx];
                    while (*v && di < dst_max - 1) dst[di++] = *v++;
                }
                continue;
            }
            /* $0 — function/script name (expand as empty for now) */
            if (*src == '0') {
                src++;
                /* $0 is not stored in our positional args; leave empty */
                continue;
            }
            /* $@ — all positional args as separate words (space-separated) */
            if (*src == '@') {
                src++;
                if (func_in_call) {
                    for (int i = 0; i < func_argc; i++) {
                        if (i > 0 && di < dst_max - 1) dst[di++] = ' ';
                        const char *v = func_argv[i];
                        while (*v && di < dst_max - 1) dst[di++] = *v++;
                    }
                }
                continue;
            }
            /* $* — all positional args as single string (space-separated) */
            if (*src == '*') {
                src++;
                if (func_in_call) {
                    for (int i = 0; i < func_argc; i++) {
                        if (i > 0 && di < dst_max - 1) dst[di++] = ' ';
                        const char *v = func_argv[i];
                        while (*v && di < dst_max - 1) dst[di++] = *v++;
                    }
                }
                continue;
            }
            /* $# — number of positional parameters */
            if (*src == '#') {
                src++;
                if (func_in_call) {
                    int n = func_argc;
                    if (n == 0) {
                        if (di < dst_max - 1) dst[di++] = '0';
                    } else {
                        char tmp[12]; int ti = 0;
                        while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
                        while (ti > 0 && di < dst_max - 1) dst[di++] = tmp[--ti];
                    }
                } else {
                    if (di < dst_max - 1) dst[di++] = '0';
                }
                continue;
            }
            char name[MAX_VAR_NAME];
            int ni = 0;
            while (*src && ni < MAX_VAR_NAME - 1 &&
                   ((*src >= 'A' && *src <= 'Z') ||
                    (*src >= 'a' && *src <= 'z') ||
                    (*src >= '0' && *src <= '9') ||
                    *src == '_')) {
                name[ni++] = *src++;
            }
            name[ni] = '\0';
            const char *val = shell_var_get(name);
            if (*val) {
                while (*val && di < dst_max - 1)
                    dst[di++] = *val++;
            } else {
                if (di < dst_max - 1) dst[di++] = '$';
                for (int i = 0; i < ni && di < dst_max - 1; i++)
                    dst[di++] = name[i];
            }
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static void putchar_both(char c) {
    vga_putchar(c);
    if (c == '\b') {
        serial_putchar('\b');
        serial_putchar(' ');
        serial_putchar('\b');
    } else {
        serial_putchar(c);
    }
}

#define CMD_BUF_SIZE 256

static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len;

static char history[HISTORY_SIZE][CMD_BUF_SIZE];
static int history_count = 0;
static int history_pos = 0;

#define HISTORY_FILE  "/history"
#define HISTORY_FILE_MAX  (HISTORY_SIZE * CMD_BUF_SIZE)

static char history_filebuf[HISTORY_FILE_MAX];

/* History functions extracted to sub-module */
#include "shell_history.inc"

/* Command table + tab completion extracted to sub-module */
#include "shell_completion.inc"

/* --- Background process support --- */
struct bg_cmd_info {
    char cmd[64];
    char args[CMD_BUF_SIZE];
    int  has_args;
};
static struct bg_cmd_info bg_slots[8];
static int bg_slot_next = 0;

static void bg_cmd_entry(void) {
    /* Find our slot by matching process name to bg_slots[].cmd */
    struct process *me = process_get_current();
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (me->name == bg_slots[i].cmd) { slot = i; break; }
    }
    if (slot < 0) { process_exit(); return; }
    const char *a = bg_slots[slot].has_args ? bg_slots[slot].args : NULL;
    shell_exec_cmd(bg_slots[slot].cmd, a);
    process_exit();
}

/* ── Loop execution state (for break/continue support) ──────────── */
#define LOOP_NEST_MAX 16
static int    s_loop_nest_level     = 0;  /* how many nested loops currently executing */
static int    s_loop_break_level    = 0;  /* >0: break out of this many levels */
static int    s_loop_continue_level = 0;  /* >0: continue this many levels */

static void process_cmd(void) {
    char *cmd = cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    /* --- break / continue handling --- */
    {
        char *b = cmd;
        char bword[16]; int bi = 0;
        while (*b && *b != ' ' && bi < 15) bword[bi++] = *b++;
        bword[bi] = '\0';
        if (strcmp(bword, "break") == 0 || strcmp(bword, "continue") == 0) {
            int is_break = (strcmp(bword, "break") == 0);
            /* Parse optional numeric argument */
            int n = 1;
            if (*b == ' ') {
                const char *np = b;
                while (*np == ' ') np++;
                if (*np >= '1' && *np <= '9') {
                    n = 0;
                    while (*np >= '0' && *np <= '9') n = n * 10 + (*np++ - '0');
                    if (n > LOOP_NEST_MAX) n = LOOP_NEST_MAX;
                }
            }
            if (s_loop_nest_level > 0) {
                if (is_break)
                    s_loop_break_level = n;
                else
                    s_loop_continue_level = n;
            }
            last_exit_status = 0;
            return;
        }
    }

    /* --- Sequence/conditional operators: ;  &&  ||  ---
     * Scanned BEFORE expansion so operators are not inside a variable value.
     * First operator wins; the right side is saved on the stack before any
     * modification, so recursive calls do not clobber it. */
    {
        int op = 0; /* 1=; 2=&& 3=|| */
        char *op_pos = 0;
        int brace_depth = 0; /* don't split on ; inside {...} */
        for (char *p = cmd; *p; p++) {
            if (*p == '{') { brace_depth++; continue; }
            if (*p == '}') { if (brace_depth > 0) brace_depth--; continue; }
            if (brace_depth > 0) continue;
            if (p[0] == ';') { op = 1; op_pos = p; break; }
            if (p[0] == '&' && p[1] == '&') { op = 2; op_pos = p; break; }
            if (p[0] == '|' && p[1] == '|') { op = 3; op_pos = p; break; }
        }
        if (op) {
            int op_len = (op == 1) ? 1 : 2;
            /* Save right side before we modify cmd_buf */
            char right_buf[CMD_BUF_SIZE];
            char *right = op_pos + op_len;
            while (*right == ' ') right++;
            strncpy(right_buf, right, CMD_BUF_SIZE - 1);
            right_buf[CMD_BUF_SIZE - 1] = '\0';
            /* Terminate left side at the operator */
            *op_pos = '\0';
            /* Trim trailing spaces from left */
            char *lt = op_pos - 1;
            while (lt >= cmd && *lt == ' ') { *lt = '\0'; lt--; }
            /* Execute left side if non-empty */
            if (cmd[0] != '\0') {
                cmd_len = (int)strlen(cmd_buf);
                process_cmd();
            } else {
                last_exit_status = 0;
            }
            /* Decide whether to execute right side */
            int run_right = 0;
            if (op == 1) run_right = 1;                           /* ;  always */
            else if (op == 2) run_right = (last_exit_status == 0); /* && success */
            else if (op == 3) run_right = (last_exit_status != 0); /* || failure */
            if (run_right && right_buf[0] != '\0') {
                strncpy(cmd_buf, right_buf, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);
                process_cmd();
            }
            return;
        }
    }

    /* --- Variable expansion: replace $VAR with its value --- */
    char expanded[CMD_BUF_SIZE];
    var_expand(cmd_buf, expanded, CMD_BUF_SIZE);
    strncpy(cmd_buf, expanded, CMD_BUF_SIZE - 1);
    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
    cmd = cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    /* --- Alias expansion: replace leading word with alias value --- */
    {
        char first_word[ALIAS_NAME_LEN]; int fw = 0;
        const char *p = cmd;
        while (*p && *p != ' ' && fw < ALIAS_NAME_LEN - 1) first_word[fw++] = *p++;
        first_word[fw] = '\0';
        const char *aval = shell_alias_get(first_word);
        if (aval) {
            char alias_expanded[CMD_BUF_SIZE];
            strncpy(alias_expanded, aval, CMD_BUF_SIZE - 1);
            alias_expanded[CMD_BUF_SIZE - 1] = '\0';
            if (*p == ' ') {
                int al = strlen(alias_expanded);
                if (al < CMD_BUF_SIZE - 2) {
                    alias_expanded[al] = ' ';
                    strncpy(alias_expanded + al + 1, p + 1, CMD_BUF_SIZE - al - 2);
                    alias_expanded[CMD_BUF_SIZE - 1] = '\0';
                }
            }
            strncpy(cmd_buf, alias_expanded, CMD_BUF_SIZE - 1);
            cmd_buf[CMD_BUF_SIZE - 1] = '\0';
            cmd = cmd_buf;
            while (*cmd == ' ') cmd++;
            if (*cmd == '\0') return;
        }
    }

    /* --- Glob expansion: expand * and ? wildcards in arguments --- */
    {
        /* Only expand in the args portion (after the first word) */
        char *first_end = cmd_buf;
        while (*first_end && *first_end != ' ') first_end++;
        int has_glob = 0;
        for (char *gp = first_end; *gp; gp++) {
            if (*gp == '*' || *gp == '?') { has_glob = 1; break; }
        }
        if (has_glob && *first_end) {
            char glob_out[CMD_BUF_SIZE];
            /* Copy first word verbatim */
            int fw_len = (int)(first_end - cmd_buf);
            char fw[32];
            int fw_cmp = fw_len;
            if (fw_cmp >= (int)sizeof(fw)) fw_cmp = (int)sizeof(fw) - 1;
            memcpy(fw, cmd_buf, fw_cmp);
            fw[fw_cmp] = '\0';
            int skip_glob = (strcmp(fw, "expr") == 0 || strcmp(fw, "calc") == 0);
            if (!skip_glob) {
                memcpy(glob_out, cmd_buf, fw_len);
                char rest[CMD_BUF_SIZE];
                strncpy(rest, first_end, CMD_BUF_SIZE - 1); rest[CMD_BUF_SIZE-1] = '\0';
                char expanded_rest[CMD_BUF_SIZE];
                glob_expand_line(rest, expanded_rest, CMD_BUF_SIZE);
                int er_len = strlen(expanded_rest);
                if (fw_len + er_len < CMD_BUF_SIZE - 1) {
                    memcpy(glob_out + fw_len, expanded_rest, er_len);
                    glob_out[fw_len + er_len] = '\0';
                    strncpy(cmd_buf, glob_out, CMD_BUF_SIZE - 1);
                    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                    cmd = cmd_buf;
                    while (*cmd == ' ') cmd++;
                }
            }
        }
    }

    /* --- Check for variable assignment: NAME=value [command] ---
     * Supports:
     *   NAME=value              — set variable permanently
     *   NAME=value command ...  — set variable for one command only
     *   NAME=(elem1 elem2 ...)  — set array */
    {
        const char *p = cmd;
        int valid_name = 1;
        while (*p && *p != '=' && *p != ' ') {
            char c = *p;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_'))
                { valid_name = 0; break; }
            p++;
        }
        if (valid_name && *p == '=' && p > cmd) {
            char name[MAX_VAR_NAME];
            int nl = (int)(p - cmd);
            if (nl > MAX_VAR_NAME - 1) nl = MAX_VAR_NAME - 1;
            memcpy(name, cmd, nl);
            name[nl] = '\0';

            /* Array assignment: NAME=(elem1 elem2 ...) */
            if (*(p + 1) == '(') {
                shell_array_define(name, p + 1);
                return;
            }

            /* Find end of value (up to next space) */
            const char *val_start = p + 1;
            const char *val_end = val_start;
            while (*val_end && *val_end != ' ') val_end++;

            /* Check if there's a command after the value */
            const char *after_val = val_end;
            while (*after_val == ' ') after_val++;

            if (*after_val) {
                /* Per-command environment: NAME=value command */
                /* Save old value for restoration after command */
                char saved_value[MAX_VAR_VALUE];
                const char *old_val = shell_var_get(name);
                int had_old = (old_val && *old_val);
                if (had_old) {
                    strncpy(saved_value, old_val, MAX_VAR_VALUE - 1);
                    saved_value[MAX_VAR_VALUE - 1] = '\0';
                }

                /* Set temporary value */
                char val_buf[MAX_VAR_VALUE];
                int vl = (int)(val_end - val_start);
                if (vl > MAX_VAR_VALUE - 1) vl = MAX_VAR_VALUE - 1;
                memcpy(val_buf, val_start, vl);
                val_buf[vl] = '\0';
                shell_var_set(name, val_buf);

                /* Rewrite cmd_buf with just the command part */
                char cmd_rest[CMD_BUF_SIZE];
                strncpy(cmd_rest, after_val, CMD_BUF_SIZE - 1);
                cmd_rest[CMD_BUF_SIZE - 1] = '\0';
                strncpy(cmd_buf, cmd_rest, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);

                /* Execute the command with the temporary variable */
                process_cmd();

                /* Restore old value (or remove if it didn't exist) */
                if (had_old)
                    shell_var_set(name, saved_value);
                else
                    shell_var_set(name, "");
                return;
            }

            /* Standalone assignment: NAME=value */
            shell_var_set(name, val_start);
            return;
        }
    }

    /* --- Check for function call via shell function table --- */
    {
        char fname[SHELL_FUNC_NAME_MAX]; int fi = 0;
        const char *p = cmd;
        while (*p && *p != ' ' && fi < SHELL_FUNC_NAME_MAX - 1) fname[fi++] = *p++;
        fname[fi] = '\0';
        const char *fbody = shell_func_get(fname);
        if (fbody) {
            /* Extract positional args from the rest of the command line */
            while (*p == ' ') p++;
            func_argc = 0;
            int argbuf_pos = 0;
            while (*p && func_argc < FUNC_ARGS_MAX && argbuf_pos < FUNC_ARG_BUF_SZ - 1) {
                func_argv[func_argc] = func_argbuf + argbuf_pos;
                /* Handle quoted arguments */
                if (*p == '\'') {
                    p++; /* opening single quote */
                    while (*p && *p != '\'' && argbuf_pos < FUNC_ARG_BUF_SZ - 1)
                        func_argbuf[argbuf_pos++] = *p++;
                    if (*p == '\'') p++; /* closing single quote */
                } else if (*p == '"') {
                    p++; /* opening double quote */
                    while (*p && *p != '"' && argbuf_pos < FUNC_ARG_BUF_SZ - 1)
                        func_argbuf[argbuf_pos++] = *p++;
                    if (*p == '"') p++; /* closing double quote */
                } else {
                    while (*p && *p != ' ' && argbuf_pos < FUNC_ARG_BUF_SZ - 1)
                        func_argbuf[argbuf_pos++] = *p++;
                }
                func_argbuf[argbuf_pos++] = '\0';
                func_argc++;
                while (*p == ' ') p++;
            }
            func_argbuf[argbuf_pos] = '\0';

            /* Execute each line of the function body with positional params */
            int saved_func_in_call = func_in_call;
            func_in_call = 1;
            func_return_flag = 0;

            char body_copy[SHELL_FUNC_BODY_MAX];
            strncpy(body_copy, fbody, SHELL_FUNC_BODY_MAX - 1);
            body_copy[SHELL_FUNC_BODY_MAX - 1] = '\0';
            char *line = body_copy;
            while (*line && !func_return_flag) {
                char *nl = line;
                while (*nl && *nl != '\n' && *nl != ';') nl++;
                char saved = *nl; *nl = '\0';
                char *l = line;
                while (*l == ' ' || *l == '\t') l++;
                if (*l) {
                    /* Handle return keyword — early exit with status */
                    if (strncmp(l, "return", 6) == 0 && (l[6] == '\0' || l[6] == ' ')) {
                        const char *rv = l + 6;
                        while (*rv == ' ') rv++;
                        int retval = 0;
                        while (*rv >= '0' && *rv <= '9')
                            retval = retval * 10 + (*rv++ - '0');
                        func_return_flag = 1;
                        last_exit_status = retval;
                        *nl = saved;
                        break;
                    }
                    /* Handle shift keyword — shift positional parameters */
                    if (strncmp(l, "shift", 5) == 0 && (l[5] == '\0' || l[5] == ' ')) {
                        const char *sv = l + 5;
                        while (*sv == ' ') sv++;
                        int sn = 0;
                        if (*sv >= '0' && *sv <= '9') {
                            while (*sv >= '0' && *sv <= '9')
                                sn = sn * 10 + (*sv++ - '0');
                        } else {
                            sn = 1;
                        }
                        func_shift(sn);
                        *nl = saved;
                        line = (*nl) ? nl + 1 : nl;
                        continue;
                    }
                    /* Handle local keyword — declare a local variable */
                    if (strncmp(l, "local ", 6) == 0) {
                        const char *local_rest = l + 6;
                        while (*local_rest == ' ') local_rest++;
                        /* Parse local NAME=value or local NAME */
                        char local_name[MAX_VAR_NAME];
                        int local_ni = 0;
                        while (*local_rest && *local_rest != '=' && *local_rest != ' '
                              && local_ni < MAX_VAR_NAME - 1)
                            local_name[local_ni++] = *local_rest++;
                        local_name[local_ni] = '\0';
                        if (local_ni > 0) {
                            char local_val[MAX_VAR_VALUE];
                            if (*local_rest == '=') {
                                local_rest++;
                                int local_vi = 0;
                                while (*local_rest && *local_rest != ' '
                                      && local_vi < MAX_VAR_VALUE - 1)
                                    local_val[local_vi++] = *local_rest++;
                                local_val[local_vi] = '\0';
                            } else {
                                local_val[0] = '\0';
                            }
                            /* Set the local variable (shadowing any global) */
                            shell_var_set(local_name, local_val);
                        }
                        *nl = saved;
                        line = (*nl) ? nl + 1 : nl;
                        continue;
                    }
                    strncpy(cmd_buf, l, CMD_BUF_SIZE - 1);
                    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                    cmd_len = (int)strlen(cmd_buf);
                    process_cmd();
                }
                *nl = saved;
                line = (*nl) ? nl + 1 : nl;
            }

            /* Clean up positional args */
            func_in_call = saved_func_in_call;
            func_argc = 0;
            func_return_flag = 0;
            return;
        }
    }

    /* --- Check for background operator: cmd & --- */
    int bg = 0;
    {
        int len = strlen(cmd);
        while (len > 0 && cmd[len-1] == ' ') len--;
        if (len > 0 && cmd[len-1] == '&') {
            bg = 1;
            cmd[len-1] = '\0';
            len--;
            while (len > 0 && cmd[len-1] == ' ') { cmd[len-1] = '\0'; len--; }
        }
    }

    if (bg) {
        /* Parse command and args, then launch as background process */
        char *bcmd = cmd;
        while (*bcmd == ' ') bcmd++;
        char *bargs = bcmd;
        while (*bargs && *bargs != ' ') bargs++;
        if (*bargs) { *bargs = '\0'; bargs++; while (*bargs == ' ') bargs++; }
        else bargs = NULL;

        int slot = bg_slot_next;
        bg_slot_next = (bg_slot_next + 1) % 8;
        strncpy(bg_slots[slot].cmd, bcmd, 63);
        bg_slots[slot].cmd[63] = '\0';
        if (bargs && *bargs) {
            strncpy(bg_slots[slot].args, bargs, CMD_BUF_SIZE - 1);
            bg_slots[slot].args[CMD_BUF_SIZE - 1] = '\0';
            bg_slots[slot].has_args = 1;
        } else {
            bg_slots[slot].args[0] = '\0';
            bg_slots[slot].has_args = 0;
        }

        struct process *p = process_create(bg_cmd_entry, bg_slots[slot].cmd);
        if (p) {
            p->is_background = 1;
            p->pgid = p->pid;
            if (p->sid == 0) p->sid = p->pid;
            kprintf("[%lu] %s\n", (unsigned long)p->pid, bg_slots[slot].cmd);
        } else {
            kprintf("Failed to create background process\n");
        }
        return;
    }

    /* --- Check for pipe: cmd1 | cmd2 | ... (sync capture buffer, no blocking) --- */
    {
        int pipe_count = 0;
        for (const char *p = cmd; *p; p++) {
            if (*p == '|' && p[1] != '|' && (p == cmd || p[-1] != '|'))
                pipe_count++;
        }
        if (pipe_count > 0) {
            char pipe_work[CMD_BUF_SIZE];
            char pipe_xfer[4096];
            int pipe_xfer_len = 0;

            strncpy(pipe_work, cmd, CMD_BUF_SIZE - 1);
            pipe_work[CMD_BUF_SIZE - 1] = '\0';

            char *seg = pipe_work;
            while (seg) {
                char *pipe_sep = NULL;
                for (char *p = seg; *p; p++) {
                    if (*p == '|' && p[1] != '|' && (p == seg || p[-1] != '|')) {
                        pipe_sep = p;
                        break;
                    }
                }
                int is_last = (pipe_sep == NULL);
                if (pipe_sep) *pipe_sep = '\0';

                while (*seg == ' ') seg++;
                int slen = (int)strlen(seg);
                while (slen > 0 && seg[slen - 1] == ' ') seg[--slen] = '\0';

                char *scmd  = seg;
                char *sargs = seg;
                while (*sargs && *sargs != ' ') sargs++;
                if (*sargs) { *sargs = '\0'; sargs++; while (*sargs == ' ') sargs++; }
                else sargs = NULL;

                if (pipe_xfer_len > 0)
                    shell_set_stdin(pipe_xfer, pipe_xfer_len);
                else
                    shell_clear_stdin();

                if (is_last) {
                    shell_exec_cmd(scmd, sargs);
                    shell_clear_stdin();
                    return;
                }

                pipe_xfer_len = 0;
                void (*saved_hook)(char, void *) = NULL;
                void  *saved_ctx                  = NULL;
                kprintf_get_hook(&saved_hook, &saved_ctx);
                struct shell_capture_ctx cap_ctx = { pipe_xfer, &pipe_xfer_len, (int)sizeof(pipe_xfer) - 1 };
                kprintf_set_hook(shell_capture_cb, &cap_ctx);
                shell_exec_cmd(scmd, sargs);
                kprintf_set_hook(saved_hook, saved_ctx);
                pipe_xfer[pipe_xfer_len] = '\0';

                seg = pipe_sep + 1;
            }
            shell_clear_stdin();
            return;
        }
    }

    /* --- Check for input redirection: cmd < file --- */
    {
        char *iredir = NULL;
        for (char *p = cmd; *p; p++) {
            /* Only treat '<' as redirect when preceded by space (standalone operator)
             * and the following word contains no '>' (not an HTML/XML tag like <html>) */
            if (*p == '<' && (p == cmd || *(p-1) == ' ')) {
                const char *q = p + 1;
                while (*q == ' ') q++;
                int has_close = 0;
                for (const char *qq = q; *qq && *qq != ' '; qq++) {
                    if (*qq == '>') { has_close = 1; break; }
                }
                if (!has_close) { iredir = p; break; }
            }
        }
        if (iredir) {
            *iredir = '\0';
            char *ifile = iredir + 1;
            while (*ifile == ' ') ifile++;
            int iflen = strlen(ifile);
            while (iflen > 0 && ifile[iflen-1] == ' ') ifile[--iflen] = '\0';
            /* Trim left command */
            char *lc = cmd; while (*lc == ' ') lc++;
            char *lt = iredir - 1;
            while (lt > lc && *lt == ' ') { *lt = '\0'; lt--; }
            /* Parse cmd and args from left */
            char *ic = lc;
            char *ia = ic;
            while (*ia && *ia != ' ') ia++;
            if (*ia) { *ia = '\0'; ia++; while (*ia == ' ') ia++; }
            else ia = NULL;
            /* Read file contents and pass as argument (inject via env) */
            char iredir_buf[4096];
            uint32_t iredir_sz = 0;
            char ipath[64];
            if (ifile[0] != '/') {
                ipath[0] = '/'; strncpy(ipath + 1, ifile, 62); ipath[63] = '\0';
            } else {
                strncpy(ipath, ifile, 63); ipath[63] = '\0';
            }
            if (vfs_read(ipath, iredir_buf, sizeof(iredir_buf) - 1, &iredir_sz) == 0) {
                iredir_buf[iredir_sz] = '\0';
                /* Remove trailing newline for nicer arg passing */
                while (iredir_sz > 0 && (iredir_buf[iredir_sz-1] == '\n' || iredir_buf[iredir_sz-1] == '\r'))
                    iredir_buf[--iredir_sz] = '\0';
                shell_var_set("__STDIN__", iredir_buf);
            }
            /* Build combined args: original_args + file contents via shell var */
            char iredir_args[CMD_BUF_SIZE];
            if (ia && ia[0]) {
                strncpy(iredir_args, ia, CMD_BUF_SIZE - 1);
            } else {
                strncpy(iredir_args, iredir_buf, CMD_BUF_SIZE - 1);
            }
            iredir_args[CMD_BUF_SIZE - 1] = '\0';
            shell_exec_cmd(ic, iredir_args[0] ? iredir_args : iredir_buf);
            return;
        }
    }

    /* --- Check for output redirection: cmd > file or cmd >> file --- */
    char *redir_pos = 0;
    int redir_append = 0;
    for (char *p = cmd; *p; p++) {
        /* Only treat '>' as redirect when preceded by space (standalone operator) */
        if (*p == '>' && (p == cmd || *(p-1) == ' ')) {
            if (*(p + 1) == '>') { redir_pos = p; redir_append = 1; break; }
            redir_pos = p; break;
        }
    }
    if (redir_pos) {
        *redir_pos = '\0';
        char *file = redir_pos + 1;
        if (redir_append) { file++; }
        while (*file == ' ') file++;
        /* trim trailing spaces from file */
        int flen = strlen(file);
        while (flen > 0 && file[flen-1] == ' ') file[--flen] = '\0';
        /* prefix with / if needed */
        char filepath[64];
        if (file[0] != '/') {
            filepath[0] = '/';
            strncpy(filepath + 1, file, 62);
        } else {
            strncpy(filepath, file, 63);
        }
        filepath[63] = '\0';

        /* Trim left side and parse */
        char *lcmd = cmd; while (*lcmd == ' ') lcmd++;
        char *lt = redir_pos - 1;
        while (lt > lcmd && *lt == ' ') { *lt = '\0'; lt--; }
        char *largs = lcmd;
        while (*largs && *largs != ' ') largs++;
        if (*largs) { *largs = '\0'; largs++; while (*largs == ' ') largs++; }
        else largs = 0;

        /* Capture output */
        char redir_buf[4096];
        int redir_len = 0;
        void (*saved_hook)(char, void*) = 0;
        void *saved_ctx = 0;
        kprintf_get_hook(&saved_hook, &saved_ctx);
        struct shell_capture_ctx redir_ctx = { redir_buf, &redir_len, (int)sizeof(redir_buf) - 1 };
        kprintf_set_hook(shell_capture_cb, &redir_ctx);
        shell_exec_cmd(lcmd, largs);
        kprintf_set_hook(saved_hook, saved_ctx);
        redir_buf[redir_len] = '\0';

        if (redir_append) {
            /* Read existing content, append */
            uint32_t existing = 0;
            char old[4096];
            if (vfs_read(filepath, old, 4093, &existing) == 0 && existing > 0) {
                int add_nl = (old[existing-1] != '\n') ? 1 : 0;
                int total = (int)existing + add_nl + redir_len;
                if (total > 4095) total = 4095;
                if (add_nl && existing < 4095) old[existing++] = '\n';
                memcpy(old + existing, redir_buf, total - (int)existing);
                old[total] = '\0';
                vfs_write(filepath, old, (uint32_t)total);
            } else {
                vfs_create(filepath, 1);
                vfs_write(filepath, redir_buf, (uint32_t)redir_len);
            }
        } else {
            vfs_create(filepath, 1);
            vfs_write(filepath, redir_buf, (uint32_t)redir_len);
        }
        return;
    }

    /* --- Normal command --- */
    char *args = cmd;
    while (*args && *args != ' ') args++;
    if (*args) { *args = '\0'; args++; while (*args == ' ') args++; }
    else args = NULL;

    shell_exec_cmd(cmd, args);
}

/* Process a full command line (with pipe/redirect/background support).
 * Used by telnet daemon to get the same features as the local shell. */

/* ── State shared between keyboard and telnet paths ─────────────── */

/* Loop-block accumulation state */
#define LOOP_BLOCK_BODY_MAX 2048
static int    s_in_loop_block    = 0;  /* 1=accumulating loop body */
static char   s_loop_block_body[LOOP_BLOCK_BODY_MAX];
static int    s_loop_block_len   = 0;
static int    s_loop_block_depth = 0;  /* nested loop depth */
static int    s_loop_block_type  = 0;  /* LOOP_FOR / LOOP_WHILE / LOOP_UNTIL */

/* Loop type constants */
#define LOOP_FOR   1
#define LOOP_WHILE 2
#define LOOP_UNTIL 3

/* Function definition state */
static int    s_in_func_def  = 0;
static char   s_func_def_name[SHELL_FUNC_NAME_MAX];
static char   s_func_def_body[SHELL_FUNC_BODY_MAX];
static int    s_func_def_len = 0;
static int    s_func_brace   = 0;

/* If-block accumulation state */
#define IF_BLOCK_BODY_MAX 2048
static int    s_in_if_block   = 0;
static char   s_if_block_body[IF_BLOCK_BODY_MAX];
static int    s_if_block_len  = 0;
static int    s_if_block_depth = 0;  /* nested if depth */

/* Case-block accumulation state */
#define CASE_BLOCK_BODY_MAX 2048
static int    s_in_case_block   = 0;
static char   s_case_block_body[CASE_BLOCK_BODY_MAX];
static int    s_case_block_len  = 0;
static int    s_case_block_depth = 0;  /* nested case depth */

/*
 * Parse and execute a case word / in / PATTERN) COMMANDS ;; esac block.
 *
 * Syntax:
 *   case WORD in
 *     PATTERN1) COMMANDS1 ;;
 *     PATTERN2|PATTERN3) COMMANDS2 ;;
 *     *) DEFAULT ;;
 *   esac
 *
 * Returns the exit status of the executed branch, or 0 if no pattern matched.
 */
static int process_case_block(const char *block) {
    if (!block || !*block) return 0;

    /* Work on a mutable copy */
    char buf[CASE_BLOCK_BODY_MAX];
    strncpy(buf, block, CASE_BLOCK_BODY_MAX - 1);
    buf[CASE_BLOCK_BODY_MAX - 1] = '\0';

    /* Strip leading whitespace */
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "case ", 5) != 0 && strncmp(p, "case\t", 5) != 0) {
        return 0;
    }
    p += 4; /* skip "case" */
    while (*p == ' ' || *p == '\t') p++;

    if (!*p) return 0;

    /* Extract the word (everything between "case" and "in") */
    char word[128];
    int wi = 0;
    int in_sq = 0, in_dq = 0;
    while (*p && wi < (int)sizeof(word) - 1) {
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (!in_sq && !in_dq &&
            ((*p == ' ' || *p == '\t') && (strncmp(p, " in", 3) == 0 || strncmp(p, "\tin", 3) == 0)))
            break;
        word[wi++] = *p++;
    }
    word[wi] = '\0';

    /* Skip whitespace and the "in" keyword */
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "in", 2) == 0) {
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* If no word was extracted, fall back to executing normally */
    if (wi == 0) return 0;

    /* Now parse clauses separated by ;;, each with PATTERN) COMMANDS */
    int executed = 0;
    int last_status = 0;

    while (*p && !executed) {
        /* Skip whitespace and newlines */
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;

        /* Check for esac terminator */
        if (strncmp(p, "esac", 4) == 0 &&
            (*(p+4) == '\0' || *(p+4) == ' ' || *(p+4) == '\t' || *(p+4) == ';' || *(p+4) == '\n'))
            break;

        /* Extract pattern(s): everything up to ')' */
        char pattern[128];
        int pi = 0;
        in_sq = 0; in_dq = 0;
        while (*p && pi < (int)sizeof(pattern) - 1) {
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
            if (*p == '"'  && !in_sq) { in_dq = !in_dq; p++; continue; }
            if (!in_sq && !in_dq && *p == ')') break;
            pattern[pi++] = *p++;
        }
        pattern[pi] = '\0';
        if (*p == ')') p++; /* skip ')' */
        while (*p == ' ' || *p == '\t') p++;

        /* Extract commands: everything up to ;; or esac */
        char commands[2048];
        int ci = 0;
        in_sq = 0; in_dq = 0;
        int found_term = 0;
        while (*p && ci < (int)sizeof(commands) - 1) {
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; commands[ci++] = *p++; continue; }
            if (*p == '"'  && !in_sq) { in_dq = !in_dq; commands[ci++] = *p++; continue; }
            if (!in_sq && !in_dq) {
                /* Check for ;; terminator */
                if (strncmp(p, ";;", 2) == 0) {
                    found_term = 1;
                    p += 2;
                    break;
                }
                /* Check for esac terminator */
                if (strncmp(p, "esac", 4) == 0 &&
                    (*(p+4) == '\0' || *(p+4) == ' ' || *(p+4) == '\t' || *(p+4) == ';' || *(p+4) == '\n'))
                    break;
            }
            commands[ci++] = *p++;
        }
        commands[ci] = '\0';

        /* Try to match the word against the pattern(s).
         * Patterns can be separated by | (alternatives). */
        int matched = 0;
        char pat_copy[128];
        strncpy(pat_copy, pattern, sizeof(pat_copy) - 1);
        pat_copy[sizeof(pat_copy) - 1] = '\0';

        char *pat_token = pat_copy;
        while (pat_token && *pat_token) {
            /* Trim leading whitespace */
            while (*pat_token == ' ' || *pat_token == '\t') pat_token++;
            if (!*pat_token) break;

            /* Find end of this alternative (| or end) */
            char *pat_end = pat_token;
            while (*pat_end && *pat_end != '|') pat_end++;
            char saved = *pat_end;
            if (*pat_end == '|') *pat_end = '\0';

            /* Trim trailing whitespace */
            char *pe = pat_end - 1;
            while (pe >= pat_token && (*pe == ' ' || *pe == '\t')) *pe-- = '\0';

            /* Check match — use fnmatch for wildcard patterns */
            if (strcmp(pat_token, "*") == 0) {
                matched = 1;
            } else if (fnmatch(pat_token, word, 0) == 0) {
                matched = 1;
            }

            *pat_end = saved;
            if (*pat_end == '|')
                pat_token = pat_end + 1;
            else
                break;

            if (matched) break;
        }

        if (matched && !executed) {
            /* Execute the commands */
            executed = 1;

            /* Strip leading/trailing whitespace/newlines from commands */
            char *cmd_start = commands;
            while (*cmd_start == ' ' || *cmd_start == '\t' || *cmd_start == '\n') cmd_start++;

            if (*cmd_start) {
                /* Execute each line of the commands */
                char line_copy[2048];
                strncpy(line_copy, cmd_start, sizeof(line_copy) - 1);
                line_copy[sizeof(line_copy) - 1] = '\0';

                char *line_save = NULL;
                char *line = strtok_r(line_copy, "\n", &line_save);
                while (line) {
                    /* Trim whitespace */
                    char *l = line;
                    while (*l == ' ' || *l == '\t') l++;
                    if (*l) {
                        /* Execute the line as a shell command */
                        char saved_cmd[CMD_BUF_SIZE];
                        strncpy(saved_cmd, cmd_buf, CMD_BUF_SIZE - 1);
                        saved_cmd[CMD_BUF_SIZE - 1] = '\0';
                        int saved_len = cmd_len;

                        strncpy(cmd_buf, l, CMD_BUF_SIZE - 1);
                        cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                        cmd_len = strlen(cmd_buf);
                        process_cmd();
                        last_status = last_exit_status;

                        strncpy(cmd_buf, saved_cmd, CMD_BUF_SIZE - 1);
                        cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                        cmd_len = saved_len;
                    }
                    line = strtok_r(NULL, "\n", &line_save);
                }
            }
            break;
        }

        /* If we found ;; but didn't match, continue to next clause */
        if (!found_term) break;
    }

    return last_status;
}

/*
 * Parse and execute an if/then/elif/else/fi block.
 *
 * The block looks like:
 *   if CONDITION_COMMAND ; then
 *       THEN_BODY
 *   elif CONDITION ; then
 *       ELIF_BODY
 *   else
 *       ELSE_BODY
 *   fi
 *
 * CONDITION_COMMAND may be `test ...` or `[ ... ]` or any command.
 * Returns the exit status of the executed branch, or 1 if no branch matched.
 */
static int process_if_block(const char *block) {
    if (!block || !*block) return 1;

    /* Work on a mutable copy */
    char buf[IF_BLOCK_BODY_MAX];
    strncpy(buf, block, IF_BLOCK_BODY_MAX - 1);
    buf[IF_BLOCK_BODY_MAX - 1] = '\0';

    /* Strip leading "if " keyword */
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "if ", 3) != 0 && strncmp(p, "if\t", 3) != 0) {
        /* Not a valid if block — execute as a normal command */
        char tmp[CMD_BUF_SIZE];
        strncpy(tmp, block, CMD_BUF_SIZE - 1);
        tmp[CMD_BUF_SIZE - 1] = '\0';
        strncpy(cmd_buf, tmp, CMD_BUF_SIZE - 1);
        cmd_buf[CMD_BUF_SIZE - 1] = '\0';
        cmd_len = (int)strlen(cmd_buf);
        process_cmd();
        return last_exit_status;
    }
    p += 2; /* skip "if" */
    while (*p == ' ' || *p == '\t') p++;

    /*
     * Strategy: find the matching "fi" at the end, then split into clauses.
     * Each clause is separated by "then", "elif", "else", "fi".
     *
     * We parse from left to right:
     *   1. Extract condition (everything up to "then")
     *   2. Extract then-body (everything up to "elif"/"else"/"fi")
     *   3. If condition succeeds: execute then-body, return
     *   4. Else if "elif": extract condition, repeat from step 1
     *   5. Else if "else": execute else-body, return
     *   6. If no branch matched: return 1
     *
     * To handle quotes and nested if, we track brace/quote depth.
     */
    char *clause = p;          /* current position in the block */
    int branch_taken = 0;

    while (*clause && !branch_taken) {
        /* Find the next "then" / "elif" / "else" / "fi" token */
        char *then_pos = NULL;
        char *elif_pos = NULL;
        char *else_pos = NULL;
        char *fi_pos   = NULL;

        /* Scan for tokens, respecting quoting */
        int in_squote = 0, in_dquote = 0;
        int if_nest = 0;
        char *scan = clause;

        while (*scan) {
            if (*scan == '\'' && !in_dquote) { in_squote = !in_squote; scan++; continue; }
            if (*scan == '"'  && !in_squote) { in_dquote = !in_dquote; scan++; continue; }
            if (in_squote || in_dquote) { scan++; continue; }

            /* Track nested if blocks inside condition bodies (e.g. if ... fi inside then) */
            if (strncmp(scan, "if ", 3) == 0 || strncmp(scan, "if\t", 3) == 0) {
                /* Check this is at a word boundary */
                if (scan == clause || *(scan - 1) == ' ' || *(scan - 1) == '\t' ||
                    *(scan - 1) == ';' || *(scan - 1) == '\n')
                    if_nest++;
                scan++;
                continue;
            }
            if (strncmp(scan, "fi", 2) == 0 && if_nest > 0 &&
                (*(scan + 2) == '\0' || *(scan + 2) == ' ' || *(scan + 2) == '\t' ||
                 *(scan + 2) == ';' || *(scan + 2) == '\n')) {
                /* Check word boundary */
                if (scan == clause || *(scan - 1) == ' ' || *(scan - 1) == '\t' ||
                    *(scan - 1) == ';' || *(scan - 1) == '\n') {
                    if_nest--;
                    scan += 2;
                    continue;
                }
            }

            /* Check for keywords at word boundaries */
            int at_boundary = (scan == clause || *(scan - 1) == ' ' || *(scan - 1) == '\t' ||
                               *(scan - 1) == ';' || *(scan - 1) == '\n');

            if (at_boundary) {
                if (!then_pos && strncmp(scan, "then", 4) == 0 &&
                    (*(scan + 4) == '\0' || *(scan + 4) == ' ' || *(scan + 4) == '\t' ||
                     *(scan + 4) == ';' || *(scan + 4) == '\n')) {
                    if (if_nest == 0) then_pos = scan;
                    scan += 4;
                    continue;
                }
                if (!elif_pos && strncmp(scan, "elif", 4) == 0 &&
                    (*(scan + 4) == '\0' || *(scan + 4) == ' ' || *(scan + 4) == '\t' ||
                     *(scan + 4) == ';' || *(scan + 4) == '\n')) {
                    if (if_nest == 0) elif_pos = scan;
                    scan += 4;
                    continue;
                }
                if (!else_pos && strncmp(scan, "else", 4) == 0 &&
                    (*(scan + 4) == '\0' || *(scan + 4) == ' ' || *(scan + 4) == '\t' ||
                     *(scan + 4) == ';' || *(scan + 4) == '\n')) {
                    if (if_nest == 0) else_pos = scan;
                    scan += 4;
                    continue;
                }
                if (!fi_pos && strncmp(scan, "fi", 2) == 0 && if_nest == 0 &&
                    (*(scan + 2) == '\0' || *(scan + 2) == ' ' || *(scan + 2) == '\t' ||
                     *(scan + 2) == ';' || *(scan + 2) == '\n')) {
                    fi_pos = scan;
                    scan += 2;
                    continue;
                }
            }
            scan++;
        }

        /* We must have at least "then" and "fi" */
        if (!then_pos || !fi_pos) {
            kprintf("if: syntax error — missing then or fi\n");
            last_exit_status = 2;
            return 2;
        }

        /* ── Step 1: Execute the condition clause ───────────────── */
        /* Condition is from clause up to then_pos */
        char condition[IF_BLOCK_BODY_MAX];
        int cond_len = (int)(then_pos - clause);
        if (cond_len >= IF_BLOCK_BODY_MAX) cond_len = IF_BLOCK_BODY_MAX - 1;
        memcpy(condition, clause, cond_len);
        condition[cond_len] = '\0';

        /* Trim trailing whitespace from condition */
        while (cond_len > 0 && (condition[cond_len - 1] == ' ' || condition[cond_len - 1] == '\t'))
            condition[--cond_len] = '\0';

        /* Execute condition command */
        if (*condition) {
            strncpy(cmd_buf, condition, CMD_BUF_SIZE - 1);
            cmd_buf[CMD_BUF_SIZE - 1] = '\0';
            cmd_len = (int)strlen(cmd_buf);
            process_cmd();
        } else {
            /* Empty condition — treat as failure */
            last_exit_status = 1;
        }

        /* ── Step 2: If condition succeeded (exit 0), execute then-body ── */
        if (last_exit_status == 0) {
            /* Find the end of this then-body: next elif/else/fi */
            char *then_end;
            if (elif_pos)      then_end = elif_pos;
            else if (else_pos) then_end = else_pos;
            else               then_end = fi_pos;

            char then_body[IF_BLOCK_BODY_MAX];
            int tb_len = (int)(then_end - (then_pos + 4)); /* skip "then" */
            if (tb_len > 0) {
                char *tb_start = then_pos + 4;
                while (*tb_start == ' ' || *tb_start == '\t' || *tb_start == ';' || *tb_start == '\n')
                    { tb_start++; tb_len--; }
                if (tb_len > 0) {
                    if (tb_len >= IF_BLOCK_BODY_MAX) tb_len = IF_BLOCK_BODY_MAX - 1;
                    memcpy(then_body, tb_start, tb_len);
                    then_body[tb_len] = '\0';

                    strncpy(cmd_buf, then_body, CMD_BUF_SIZE - 1);
                    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                    cmd_len = (int)strlen(cmd_buf);
                    process_cmd();
                }
            }
            branch_taken = 1;
            break;
        }

        /* ── Step 3: Condition failed. Try elif / else / fi ────────── */
        if (elif_pos) {
            /* Move clause to after "elif" for next iteration */
            clause = elif_pos + 4;
            while (*clause == ' ' || *clause == '\t' || *clause == ';' || *clause == '\n') clause++;
            continue;
        }
        if (else_pos) {
            /* Execute else-body */
            char else_body[IF_BLOCK_BODY_MAX];
            int eb_len = (int)(fi_pos - (else_pos + 4));
            if (eb_len > 0) {
                char *eb_start = else_pos + 4;
                while (*eb_start == ' ' || *eb_start == '\t' || *eb_start == ';' || *eb_start == '\n')
                    { eb_start++; eb_len--; }
                if (eb_len > 0) {
                    if (eb_len >= IF_BLOCK_BODY_MAX) eb_len = IF_BLOCK_BODY_MAX - 1;
                    memcpy(else_body, eb_start, eb_len);
                    else_body[eb_len] = '\0';

                    strncpy(cmd_buf, else_body, CMD_BUF_SIZE - 1);
                    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                    cmd_len = (int)strlen(cmd_buf);
                    process_cmd();
                }
            }
            branch_taken = 1;
            break;
        }

        /* No elif and no else — condition was false, do nothing */
        break;
    }

    if (!branch_taken && last_exit_status != 0) {
        /* No branch taken — condition was false and no else/elif */
        /* last_exit_status reflects the failed condition */
    }

    return last_exit_status;
}

/*
 * Parse and execute a loop block (for/while/until ... do ... done).
 *
 * Supported forms:
 *   for VAR in WORD1 WORD2 ...; do BODY; done
 *   while CONDITION; do BODY; done
 *   until CONDITION; do BODY; done
 *
 * Returns 0 on normal completion, non-zero if break/error occurred.
 */
static int process_loop_block(const char *block) {
    if (!block || !*block) return 1;

    /* Work on a mutable copy */
    char buf[LOOP_BLOCK_BODY_MAX];
    strncpy(buf, block, LOOP_BLOCK_BODY_MAX - 1);
    buf[LOOP_BLOCK_BODY_MAX - 1] = '\0';

    /* Strip leading whitespace */
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 1;

    /* Determine loop type */
    int type = 0;
    if (strncmp(p, "for ", 4) == 0 || strncmp(p, "for\t", 4) == 0) {
        type = LOOP_FOR;
        p += 3;
    } else if (strncmp(p, "while ", 6) == 0 || strncmp(p, "while\t", 6) == 0) {
        type = LOOP_WHILE;
        p += 5;
    } else if (strncmp(p, "until ", 6) == 0 || strncmp(p, "until\t", 6) == 0) {
        type = LOOP_UNTIL;
        p += 5;
    } else {
        /* Not a loop — execute as normal command */
        char tmp[CMD_BUF_SIZE];
        strncpy(tmp, block, CMD_BUF_SIZE - 1);
        tmp[CMD_BUF_SIZE - 1] = '\0';
        strncpy(cmd_buf, tmp, CMD_BUF_SIZE - 1);
        cmd_buf[CMD_BUF_SIZE - 1] = '\0';
        cmd_len = (int)strlen(cmd_buf);
        process_cmd();
        return last_exit_status;
    }
    while (*p == ' ' || *p == '\t') p++;

    /* Find the "do" keyword that separates prelude from body, respecting quotes/nesting */
    char *do_pos = NULL;
    char *done_pos = NULL;
    int in_squote = 0, in_dquote = 0;
    int loop_nest = 0;
    char *scan = p;

    while (*scan) {
        if (*scan == '\'' && !in_dquote) { in_squote = !in_squote; scan++; continue; }
        if (*scan == '"'  && !in_squote) { in_dquote = !in_dquote; scan++; continue; }
        if (in_squote || in_dquote) { scan++; continue; }

        /* Track nested loop keywords */
        if ((strncmp(scan, "for ", 4) == 0 || strncmp(scan, "for\t", 4) == 0 ||
             strncmp(scan, "while ", 6) == 0 || strncmp(scan, "while\t", 6) == 0 ||
             strncmp(scan, "until ", 6) == 0 || strncmp(scan, "until\t", 6) == 0) &&
            (scan == p || *(scan - 1) == ' ' || *(scan - 1) == '\t' || *(scan - 1) == ';' || *(scan - 1) == '\n')) {
            loop_nest++;
            scan += 4; /* skip past keyword */
            continue;
        }
        if (!do_pos && strncmp(scan, "do", 2) == 0 && loop_nest == 0 &&
            (*(scan + 2) == '\0' || *(scan + 2) == ' ' || *(scan + 2) == '\t' ||
             *(scan + 2) == ';' || *(scan + 2) == '\n') &&
            (scan == p || *(scan - 1) == ' ' || *(scan - 1) == '\t' || *(scan - 1) == ';' || *(scan - 1) == '\n')) {
            do_pos = scan;
            scan += 2;
            continue;
        }
        if (!done_pos && strncmp(scan, "done", 4) == 0 && loop_nest == 0 &&
            (*(scan + 4) == '\0' || *(scan + 4) == ' ' || *(scan + 4) == '\t' ||
             *(scan + 4) == ';' || *(scan + 4) == '\n') &&
            (scan == p || *(scan - 1) == ' ' || *(scan - 1) == '\t' || *(scan - 1) == ';' || *(scan - 1) == '\n')) {
            done_pos = scan;
            scan += 4;
            continue;
        }
        /* Track matching "done" for nested loops */
        if (strncmp(scan, "done", 4) == 0 && loop_nest > 0 &&
            (*(scan + 4) == '\0' || *(scan + 4) == ' ' || *(scan + 4) == '\t' ||
             *(scan + 4) == ';' || *(scan + 4) == '\n') &&
            (scan == p || *(scan - 1) == ' ' || *(scan - 1) == '\t' || *(scan - 1) == ';' || *(scan - 1) == '\n')) {
            loop_nest--;
            scan += 4;
            continue;
        }
        scan++;
    }

    if (!do_pos || !done_pos) {
        kprintf("loop: syntax error — missing do or done\n");
        last_exit_status = 2;
        return 2;
    }

    /* Extract the prelude (everything from keyword to "do") */
    char prelude[LOOP_BLOCK_BODY_MAX];
    int prelude_len = (int)(do_pos - p);
    if (prelude_len >= LOOP_BLOCK_BODY_MAX) prelude_len = LOOP_BLOCK_BODY_MAX - 1;
    memcpy(prelude, p, prelude_len);
    prelude[prelude_len] = '\0';

    /* Trim trailing whitespace from prelude */
    while (prelude_len > 0 && (prelude[prelude_len - 1] == ' ' || prelude[prelude_len - 1] == '\t'))
        prelude[--prelude_len] = '\0';

    /* Extract the body (everything from after "do" to "done") */
    char *body_start = do_pos + 2;
    while (*body_start == ' ' || *body_start == '\t' || *body_start == ';' || *body_start == '\n')
        body_start++;
    int body_len = (int)(done_pos - body_start);
    if (body_len < 0) body_len = 0;
    char body[LOOP_BLOCK_BODY_MAX];
    if (body_len > 0) {
        if (body_len >= LOOP_BLOCK_BODY_MAX) body_len = LOOP_BLOCK_BODY_MAX - 1;
        memcpy(body, body_start, body_len);
        body[body_len] = '\0';
        /* Trim trailing whitespace/semicolons from body */
        while (body_len > 0 && (body[body_len - 1] == ' ' || body[body_len - 1] == '\t' ||
                                body[body_len - 1] == ';' || body[body_len - 1] == '\n'))
            body[--body_len] = '\0';
    } else {
        body[0] = '\0';
    }

    /* ── Execute the loop ── */
    int result = 0;
    s_loop_nest_level++;

    if (type == LOOP_FOR) {
        /* Parse: VAR in WORD1 WORD2 ... */
        /* Skip variable name */
        char *v = prelude;
        while (*v == ' ' || *v == '\t') v++;
        char var_name[MAX_VAR_NAME]; int vn = 0;
        while (*v && *v != ' ' && *v != '\t' && vn < MAX_VAR_NAME - 1) var_name[vn++] = *v++;
        var_name[vn] = '\0';

        /* Skip "in" keyword */
        while (*v == ' ' || *v == '\t') v++;
        if (strncmp(v, "in ", 3) != 0 && strncmp(v, "in\t", 3) != 0) {
            kprintf("for: syntax error — expected 'in' after variable\n");
            if (s_loop_nest_level > 0) s_loop_nest_level--;
            last_exit_status = 2;
            return 2;
        }
        v += 2; /* skip "in" */
        while (*v == ' ' || *v == '\t') v++;

        /* Collect word list */
        char words[64][MAX_VAR_VALUE]; int nwords = 0;
        char wbuf[MAX_VAR_VALUE]; int wi = 0;
        int wq = 0; /* quote flag */
        while (*v && nwords < 64) {
            if (*v == '"' && !wq) { wq = 1; v++; continue; }
            if (*v == '"' && wq) { wq = 0; v++; continue; }
            if (*v == '\'' && !wq) { wq = 1; v++; continue; }
            if (*v == '\'' && wq) { wq = 0; v++; continue; }
            if (!wq && (*v == ' ' || *v == '\t')) {
                if (wi > 0) {
                    wbuf[wi] = '\0';
                    strncpy(words[nwords], wbuf, MAX_VAR_VALUE - 1);
                    words[nwords][MAX_VAR_VALUE - 1] = '\0';
                    nwords++;
                    wi = 0;
                }
                v++;
                continue;
            }
            if (wi < MAX_VAR_VALUE - 1) wbuf[wi++] = *v;
            v++;
        }
        if (wi > 0) {
            wbuf[wi] = '\0';
            strncpy(words[nwords], wbuf, MAX_VAR_VALUE - 1);
            words[nwords][MAX_VAR_VALUE - 1] = '\0';
            nwords++;
        }

        /* Iterate over words */
        for (int wi = 0; wi < nwords; wi++) {
            /* Check for break request */
            if (s_loop_break_level > 0) {
                s_loop_break_level--;
                break;
            }
            if (s_loop_continue_level > 0) {
                s_loop_continue_level--;
                if (s_loop_continue_level > 0) break; /* this continue targets outer loop */
                /* this continue targets our loop — just go to next iteration */
                continue;
            }

            /* Set the loop variable */
            shell_var_set(var_name, words[wi]);

            /* Execute the body */
            if (*body) {
                /* Save and restore cmd_buf around body execution */
                char saved_cmd[CMD_BUF_SIZE];
                strncpy(saved_cmd, cmd_buf, CMD_BUF_SIZE - 1);
                saved_cmd[CMD_BUF_SIZE - 1] = '\0';

                strncpy(cmd_buf, body, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);
                process_cmd();
                result = last_exit_status;

                strncpy(cmd_buf, saved_cmd, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);
            }
        }

    } else if (type == LOOP_WHILE || type == LOOP_UNTIL) {
        /* while CONDITION; do BODY; done */
        /* until CONDITION; do BODY; done */
        int max_iter = 1000000; /* safety limit */
        int iter = 0;

        while (iter < max_iter) {
            iter++;

            /* Check for break request */
            if (s_loop_break_level > 0) {
                s_loop_break_level--;
                break;
            }
            if (s_loop_continue_level > 0) {
                s_loop_continue_level--;
                if (s_loop_continue_level > 0) break;
                /* continue this loop — go test condition again */
            }

            /* Execute condition */
            {
                char saved_cmd[CMD_BUF_SIZE];
                strncpy(saved_cmd, cmd_buf, CMD_BUF_SIZE - 1);
                saved_cmd[CMD_BUF_SIZE - 1] = '\0';

                strncpy(cmd_buf, prelude, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);
                process_cmd();

                strncpy(cmd_buf, saved_cmd, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);
            }

            int cond_ok = (last_exit_status == 0);
            int should_run = (type == LOOP_WHILE) ? cond_ok : !cond_ok;

            if (!should_run) break;

            /* Execute body */
            if (*body) {
                char saved_cmd[CMD_BUF_SIZE];
                strncpy(saved_cmd, cmd_buf, CMD_BUF_SIZE - 1);
                saved_cmd[CMD_BUF_SIZE - 1] = '\0';

                strncpy(cmd_buf, body, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);
                process_cmd();
                result = last_exit_status;

                strncpy(cmd_buf, saved_cmd, CMD_BUF_SIZE - 1);
                cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                cmd_len = (int)strlen(cmd_buf);
            }
        }

        if (iter >= max_iter) {
            kprintf("loop: infinite loop protection — stopped after %d iterations\n", max_iter);
        }
    }

    if (s_loop_nest_level > 0) s_loop_nest_level--;

    /* Reset continue/break levels that targeted this loop (already decremented above) */
    last_exit_status = result;
    return result;
}

void shell_process_line(const char *line) {
    if (!line || !*line) return;

    /* --- Loop-block accumulation --- */
    if (s_in_loop_block) {
        /* Check for "done" terminator */
        const char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        /* Count opening loop keywords in this line */
        for (const char *p = line; *p; p++) {
            if (((strncmp(p, "for ", 4) == 0 || strncmp(p, "for\t", 4) == 0 ||
                  strncmp(p, "while ", 6) == 0 || strncmp(p, "while\t", 6) == 0 ||
                  strncmp(p, "until ", 6) == 0 || strncmp(p, "until\t", 6) == 0) &&
                 (p == line || *(p-1) == ' ' || *(p-1) == '\t' || *(p-1) == ';')))
                s_loop_block_depth++;
        }
        if (strcmp(l, "done") == 0 || strcmp(l, "done;") == 0) {
            s_loop_block_depth--;
            if (s_loop_block_depth <= 0) {
                /* Close loop-block */
                s_loop_block_body[s_loop_block_len] = '\0';
                /* Process the complete loop-block */
                process_loop_block(s_loop_block_body);
                s_in_loop_block = 0;
                s_loop_block_len = 0;
                s_loop_block_type = 0;
                return;
            }
        }
        /* Append line to loop-block body */
        int ll = strlen(line);
        if (s_loop_block_len + ll + 1 < LOOP_BLOCK_BODY_MAX) {
            memcpy(s_loop_block_body + s_loop_block_len, line, ll);
            s_loop_block_len += ll;
            s_loop_block_body[s_loop_block_len++] = '\n';
        }
        return;
    }

    /* --- Detect start of loop-block --- */
    {
        const char *p = line;
        while (*p == ' ') p++;
        /* Check if line starts with "for ", "while ", or "until " (word-level keywords) */
        int is_loop = 0;
        int loop_type = 0;
        if ((strncmp(p, "for ", 4) == 0 || strncmp(p, "for\t", 4) == 0) &&
            !(p[3] && p[3] != ' ' && p[3] != '\t')) {
            is_loop = 1;
            loop_type = LOOP_FOR;
        } else if ((strncmp(p, "while ", 6) == 0 || strncmp(p, "while\t", 6) == 0) &&
                   !(p[5] && p[5] != ' ' && p[5] != '\t')) {
            is_loop = 1;
            loop_type = LOOP_WHILE;
        } else if ((strncmp(p, "until ", 6) == 0 || strncmp(p, "until\t", 6) == 0) &&
                   !(p[5] && p[5] != ' ' && p[5] != '\t')) {
            is_loop = 1;
            loop_type = LOOP_UNTIL;
        }
        if (is_loop) {
            /* Check if single-line (has both "do" and "done") */
            int has_do = 0, has_done = 0;
            int in_sq = 0, in_dq = 0;
            for (const char *q = line; *q; q++) {
                if (*q == '\'' && !in_dq) { in_sq = !in_sq; continue; }
                if (*q == '"'  && !in_sq) { in_dq = !in_dq; continue; }
                if (in_sq || in_dq) continue;
                if (strncmp(q, "do", 2) == 0 && (q == line || *(q-1) == ' ' || *(q-1) == '\t' || *(q-1) == ';') &&
                    (*(q+2) == '\0' || *(q+2) == ' ' || *(q+2) == '\t' || *(q+2) == ';')) has_do = 1;
                if (strncmp(q, "done", 4) == 0 && (q == line || *(q-1) == ' ' || *(q-1) == '\t' || *(q-1) == ';')) has_done = 1;
            }
            if (has_do && has_done) {
                /* Single-line loop: process immediately */
                process_loop_block(line);
                return;
            }
            /* Multi-line loop: start accumulation */
            s_in_loop_block = 1;
            s_loop_block_len = 0;
            s_loop_block_depth = 1;
            s_loop_block_type = loop_type;
            int ll = strlen(line);
            if (s_loop_block_len + ll + 1 < LOOP_BLOCK_BODY_MAX) {
                memcpy(s_loop_block_body + s_loop_block_len, line, ll);
                s_loop_block_len += ll;
                s_loop_block_body[s_loop_block_len++] = '\n';
            }
            return;
        }
    }

    /* --- Case-block accumulation (telnet path) --- */
    if (s_in_case_block) {
        const char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        /* Count opening case keywords in this line for nesting */
        for (const char *p = line; *p; p++) {
            if ((strncmp(p, "case ", 5) == 0 || strncmp(p, "case\t", 5) == 0) &&
                (p == line || *(p-1) == ' ' || *(p-1) == '\t' || *(p-1) == ';'))
                s_case_block_depth++;
        }
        if (strcmp(l, "esac") == 0 || strcmp(l, "esac;") == 0) {
            s_case_block_depth--;
            if (s_case_block_depth <= 0) {
                /* Close case-block */
                s_case_block_body[s_case_block_len] = '\0';
                /* Process the complete case-block */
                process_case_block(s_case_block_body);
                s_in_case_block = 0;
                s_case_block_len = 0;
                return;
            }
        }
        /* Append line to case-block body */
        int ll = strlen(line);
        if (s_case_block_len + ll + 1 < CASE_BLOCK_BODY_MAX) {
            memcpy(s_case_block_body + s_case_block_len, line, ll);
            s_case_block_len += ll;
            s_case_block_body[s_case_block_len++] = '\n';
        }
        return;
    }

    /* --- Detect start of case-block (telnet path) --- */
    {
        const char *p = line;
        while (*p == ' ') p++;
        /* Check if line starts with "case " (word-level keyword) */
        if ((strncmp(p, "case ", 5) == 0 || strncmp(p, "case\t", 5) == 0) &&
            !(p[4] && p[4] != ' ' && p[4] != '\t')) {
            /* Check if single-line (has both "in" and "esac") */
            int has_in = 0, has_esac = 0;
            int in_sq = 0, in_dq = 0;
            for (const char *q = line; *q; q++) {
                if (*q == '\'' && !in_dq) { in_sq = !in_sq; continue; }
                if (*q == '"'  && !in_sq) { in_dq = !in_dq; continue; }
                if (in_sq || in_dq) continue;
                if (strncmp(q, " in", 3) == 0 && (q == line || *(q-1) == ' ' || *(q-1) == '\t')) has_in = 1;
                if (strncmp(q, "esac", 4) == 0 && (q == line || *(q-1) == ' ' || *(q-1) == '\t')) has_esac = 1;
            }
            if (has_in && has_esac) {
                /* Single-line case: process immediately */
                process_case_block(line);
                return;
            }
            /* Multi-line case: start accumulation */
            s_in_case_block = 1;
            s_case_block_len = 0;
            s_case_block_depth = 1;
            int ll = strlen(line);
            if (s_case_block_len + ll + 1 < CASE_BLOCK_BODY_MAX) {
                memcpy(s_case_block_body + s_case_block_len, line, ll);
                s_case_block_len += ll;
                s_case_block_body[s_case_block_len++] = '\n';
            }
            return;
        }
    }

    /* --- If-block accumulation --- */
    if (s_in_if_block) {
        /* Check for "fi" terminator */
        const char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (strcmp(l, "fi") == 0 || strcmp(l, "fi;") == 0) {
            s_if_block_depth--;
            if (s_if_block_depth <= 0) {
                /* Close if-block */
                s_if_block_body[s_if_block_len] = '\0';
                /* Process the complete if-block */
                process_if_block(s_if_block_body);
                s_in_if_block = 0;
                s_if_block_len = 0;
                return;
            }
        }
        /* Count "if" in this line to track nesting */
        for (const char *p = line; *p; p++) {
            if ((strncmp(p, "if ", 3) == 0 || strncmp(p, "if\t", 3) == 0) &&
                (p == line || *(p-1) == ' ' || *(p-1) == '\t' || *(p-1) == ';'))
                s_if_block_depth++;
        }
        /* Append line to if-block body */
        int ll = strlen(line);
        if (s_if_block_len + ll + 1 < IF_BLOCK_BODY_MAX) {
            memcpy(s_if_block_body + s_if_block_len, line, ll);
            s_if_block_len += ll;
            s_if_block_body[s_if_block_len++] = '\n';
        }
        return;
    }

    /* --- Detect start of if-block --- */
    {
        const char *p = line;
        while (*p == ' ') p++;
        /* Check if line starts with "if " (but not "ifconfig" or similar) */
        if ((strncmp(p, "if ", 3) == 0 || strncmp(p, "if\t", 3) == 0) &&
            !(p[3] && p[3] != ' ' && p[3] != '\t')) {
            /* Enter if-block accumulation mode */
            s_in_if_block = 1;
            s_if_block_len = 0;
            s_if_block_depth = 1;  /* this is the outer if */

            /* Count any nested "if" in the first line */
            const char *scan = p + 2; /* skip "if" */
            while (*scan == ' ' || *scan == '\t') scan++;
            /* Check if the whole block fits on one line (contains "then" and "fi") */
            int has_then = 0, has_fi = 0;
            int in_sq = 0, in_dq = 0;
            for (const char *q = line; *q; q++) {
                if (*q == '\'' && !in_dq) { in_sq = !in_sq; continue; }
                if (*q == '"'  && !in_sq) { in_dq = !in_dq; continue; }
                if (in_sq || in_dq) continue;
                if (strncmp(q, "then", 4) == 0 && (q == line || *(q-1) == ' ')) has_then = 1;
                if (strncmp(q, "fi", 2) == 0 && (q == line || *(q-1) == ' ')) has_fi = 1;
            }

            if (has_then && has_fi) {
                /* Single-line if: process immediately */
                s_in_if_block = 0;
                /* Copy the full line and process */
                char if_line[CMD_BUF_SIZE];
                strncpy(if_line, line, CMD_BUF_SIZE - 1);
                if_line[CMD_BUF_SIZE - 1] = '\0';
                process_if_block(if_line);
                return;
            }

            /* Multi-line if: start accumulation */
            int ll = strlen(line);
            if (s_if_block_len + ll + 1 < IF_BLOCK_BODY_MAX) {
                memcpy(s_if_block_body + s_if_block_len, line, ll);
                s_if_block_len += ll;
                s_if_block_body[s_if_block_len++] = '\n';
            }
            return;
        }
    }

    /* --- Function definition accumulation (telnet/shell_process_line path) --- */
    if (s_in_func_def) {
        const char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (strcmp(l, "}") == 0) {
            s_func_brace--;
            if (s_func_brace <= 0) {
                s_func_def_body[s_func_def_len] = '\0';
                shell_func_define(s_func_def_name, s_func_def_body);
                s_in_func_def = 0;
            } else {
                /* Nested closing brace — keep in body */
                int ll = strlen(line);
                if (s_func_def_len + ll + 1 < SHELL_FUNC_BODY_MAX) {
                    memcpy(s_func_def_body + s_func_def_len, line, ll);
                    s_func_def_len += ll;
                    s_func_def_body[s_func_def_len++] = '\n';
                }
            }
        } else {
            /* Append body line */
            int ll = strlen(line);
            if (s_func_def_len + ll + 1 < SHELL_FUNC_BODY_MAX) {
                memcpy(s_func_def_body + s_func_def_len, line, ll);
                s_func_def_len += ll;
                s_func_def_body[s_func_def_len++] = '\n';
            }
            /* Count opening braces in this line */
            for (const char *p = line; *p; p++)
                if (*p == '{') s_func_brace++;
        }
        return;
    }

    /* --- Detect function definition start --- */
    {
        const char *p = line;
        while (*p == ' ') p++;
        int is_func = 0;
        const char *name_start = p;
        if (strncmp(p, "function ", 9) == 0) {
            p += 9;
            while (*p == ' ') p++;
            name_start = p;
            is_func = 1;
        }
        const char *name_end = name_start;
        while (*name_end && *name_end != '(' && *name_end != ' ') name_end++;
        if (*name_end == '(') {
            int nlen = (int)(name_end - name_start);
            if (nlen > 0 && nlen < SHELL_FUNC_NAME_MAX) {
                const char *brace = name_end;
                while (*brace && *brace != '{') brace++;
                if (*brace == '{' || is_func) {
                    memcpy(s_func_def_name, name_start, nlen);
                    s_func_def_name[nlen] = '\0';
                    s_func_def_body[0] = '\0';
                    s_func_def_len = 0;
                    s_func_brace = 1;
                    s_in_func_def = 1;
                    return;
                }
            }
        }
    }

    strncpy(cmd_buf, line, CMD_BUF_SIZE - 1);
    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
    cmd_len = strlen(cmd_buf);
    process_cmd();
}

void shell_exec_cmd(const char *cmd, const char *args) {
    last_exit_status = 0;

    if (args && strcmp(args, "--help") == 0) {
        const char *d = shell_cmd_lookup_desc(cmd);
        if (d) {
            kprintf("Usage: %s\n  %s\n", cmd, d);
            return;
        }
        kprintf("Unknown command: %s\n", cmd);
        return;
    }

    /* --version: show kernel version for any command */
    if (args && strcmp(args, "--version") == 0) {
        kprintf("Hermes OS Kernel version " KVERSION " (%s %s)\n", __DATE__, __TIME__);
        return;
    }

    shell_cmd_fn fn = shell_cmd_lookup_fn(cmd);
    if (fn) {
        fn(args);
        return;
    }
    if (strcmp(cmd, "[") == 0) {
        cmd_test(args);
        return;
    }
    kprintf("Unknown command: %s\n", cmd);
    last_exit_status = 127;
}

void shell_run(void) {
    history_load();
    kprintf("\nWelcome to the OS shell. Type 'help' for commands.\n\n");

    /* Function definition state */
    static int    in_func_def  = 0;
    static char   func_def_name[SHELL_FUNC_NAME_MAX];
    static char   func_def_body[SHELL_FUNC_BODY_MAX];
    static int    func_def_len = 0;
    /* Brace depth for multi-line function bodies */
    static int    func_brace   = 0;

    /* If-block accumulation state (local to keyboard path) */
    static int    in_if_block   = 0;
    static char   if_block_body[IF_BLOCK_BODY_MAX];
    static int    if_block_len  = 0;
    static int    if_block_depth = 0;

    /* Loop-block accumulation state (local to keyboard path) */
    static int    in_loop_block    = 0;
    static char   loop_block_body[LOOP_BLOCK_BODY_MAX];
    static int    loop_block_len   = 0;
    static int    loop_block_depth = 0;

    /* Case-block accumulation state (local to keyboard path) */
    static int    in_case_block   = 0;
    static char   case_block_body[CASE_BLOCK_BODY_MAX];
    static int    case_block_len  = 0;
    static int    case_block_depth = 0;

    for (;;) {
        if (in_func_def || in_if_block || in_loop_block || in_case_block)
            kprintf("> ");
        else
            shell_prompt();
        cmd_len = 0;
        memset(cmd_buf, 0, CMD_BUF_SIZE);
        history_pos = history_count;

        while (1) {
            char c = keyboard_getchar();

            if (c == '\n') {
                putchar_both('\n');
                cmd_buf[cmd_len] = '\0';
                if (!in_func_def && !in_if_block && !in_loop_block && !in_case_block) history_add(cmd_buf);

                /* --- If-block accumulation mode (keyboard path) --- */
                if (in_if_block) {
                    const char *l = cmd_buf;
                    while (*l == ' ' || *l == '\t') l++;
                    if (strcmp(l, "fi") == 0 || strcmp(l, "fi;") == 0) {
                        if_block_depth--;
                        if (if_block_depth <= 0) {
                            /* Close if-block */
                            if_block_body[if_block_len] = '\0';
                            process_if_block(if_block_body);
                            in_if_block = 0;
                            if_block_len = 0;
                            break;
                        }
                    }
                    /* Count "if" in this line for nesting */
                    for (const char *p = cmd_buf; *p; p++) {
                        if ((strncmp(p, "if ", 3) == 0 || strncmp(p, "if\t", 3) == 0) &&
                            (p == cmd_buf || *(p-1) == ' ' || *(p-1) == '\t' || *(p-1) == ';'))
                            if_block_depth++;
                    }
                    /* Append line to if-block body */
                    int ll = strlen(cmd_buf);
                    if (if_block_len + ll + 1 < IF_BLOCK_BODY_MAX) {
                        memcpy(if_block_body + if_block_len, cmd_buf, ll);
                        if_block_len += ll;
                        if_block_body[if_block_len++] = '\n';
                    }
                    break;
                }

                /* --- Detect start of if-block (keyboard path) --- */
                {
                    const char *p = cmd_buf;
                    while (*p == ' ') p++;
                    if ((strncmp(p, "if ", 3) == 0 || strncmp(p, "if\t", 3) == 0) &&
                        !(p[3] && p[3] != ' ' && p[3] != '\t')) {
                        /* Check if single-line (has then and fi) */
                        int has_then = 0, has_fi = 0;
                        int in_sq = 0, in_dq = 0;
                        for (const char *q = cmd_buf; *q; q++) {
                            if (*q == '\'' && !in_dq) { in_sq = !in_sq; continue; }
                            if (*q == '"'  && !in_sq) { in_dq = !in_dq; continue; }
                            if (in_sq || in_dq) continue;
                            if (strncmp(q, "then", 4) == 0 && (q == cmd_buf || *(q-1) == ' ')) has_then = 1;
                            if (strncmp(q, "fi", 2) == 0 && (q == cmd_buf || *(q-1) == ' ')) has_fi = 1;
                        }
                        if (has_then && has_fi) {
                            /* Single-line if: process immediately */
                            process_if_block(cmd_buf);
                            break;
                        }
                        /* Multi-line if: start accumulation */
                        in_if_block = 1;
                        if_block_len = 0;
                        if_block_depth = 1;
                        int ll = strlen(cmd_buf);
                        if (if_block_len + ll + 1 < IF_BLOCK_BODY_MAX) {
                            memcpy(if_block_body + if_block_len, cmd_buf, ll);
                            if_block_len += ll;
                            if_block_body[if_block_len++] = '\n';
                        }
                        break;
                    }
                }

                /* --- Loop-block accumulation mode (keyboard path) --- */
                if (in_loop_block) {
                    const char *l = cmd_buf;
                    while (*l == ' ' || *l == '\t') l++;
                    /* Count loop keywords in this line for nesting */
                    for (const char *p = cmd_buf; *p; p++) {
                        if (((strncmp(p, "for ", 4) == 0 || strncmp(p, "for\t", 4) == 0 ||
                              strncmp(p, "while ", 6) == 0 || strncmp(p, "while\t", 6) == 0 ||
                              strncmp(p, "until ", 6) == 0 || strncmp(p, "until\t", 6) == 0) &&
                             (p == cmd_buf || *(p-1) == ' ' || *(p-1) == '\t' || *(p-1) == ';')))
                            loop_block_depth++;
                    }
                    if (strcmp(l, "done") == 0 || strcmp(l, "done;") == 0) {
                        loop_block_depth--;
                        if (loop_block_depth <= 0) {
                            /* Close loop-block */
                            loop_block_body[loop_block_len] = '\0';
                            process_loop_block(loop_block_body);
                            in_loop_block = 0;
                            loop_block_len = 0;
                            break;
                        }
                    }
                    /* Append line to loop-block body */
                    int ll = strlen(cmd_buf);
                    if (loop_block_len + ll + 1 < LOOP_BLOCK_BODY_MAX) {
                        memcpy(loop_block_body + loop_block_len, cmd_buf, ll);
                        loop_block_len += ll;
                        loop_block_body[loop_block_len++] = '\n';
                    }
                    break;
                }

                /* --- Detect start of loop-block (keyboard path) --- */
                {
                    const char *p = cmd_buf;
                    while (*p == ' ') p++;
                    int is_loop = 0;
                    if ((strncmp(p, "for ", 4) == 0 || strncmp(p, "for\t", 4) == 0) &&
                        !(p[3] && p[3] != ' ' && p[3] != '\t')) {
                        is_loop = 1;
                    } else if ((strncmp(p, "while ", 6) == 0 || strncmp(p, "while\t", 6) == 0) &&
                               !(p[5] && p[5] != ' ' && p[5] != '\t')) {
                        is_loop = 1;
                    } else if ((strncmp(p, "until ", 6) == 0 || strncmp(p, "until\t", 6) == 0) &&
                               !(p[5] && p[5] != ' ' && p[5] != '\t')) {
                        is_loop = 1;
                    }
                    if (is_loop) {
                        /* Check if single-line (has both do and done) */
                        int has_do = 0, has_done = 0;
                        int in_sq = 0, in_dq = 0;
                        for (const char *q = cmd_buf; *q; q++) {
                            if (*q == '\'' && !in_dq) { in_sq = !in_sq; continue; }
                            if (*q == '"'  && !in_sq) { in_dq = !in_dq; continue; }
                            if (in_sq || in_dq) continue;
                            if (strncmp(q, "do", 2) == 0 && (q == cmd_buf || *(q-1) == ' ') &&
                                (*(q+2) == '\0' || *(q+2) == ' ' || *(q+2) == '\t' || *(q+2) == ';')) has_do = 1;
                            if (strncmp(q, "done", 4) == 0 && (q == cmd_buf || *(q-1) == ' ')) has_done = 1;
                        }
                        if (has_do && has_done) {
                            /* Single-line loop: process immediately */
                            process_loop_block(cmd_buf);
                            break;
                        }
                        /* Multi-line loop: start accumulation */
                        in_loop_block = 1;
                        loop_block_len = 0;
                        loop_block_depth = 1;
                        int ll = strlen(cmd_buf);
                        if (loop_block_len + ll + 1 < LOOP_BLOCK_BODY_MAX) {
                            memcpy(loop_block_body + loop_block_len, cmd_buf, ll);
                            loop_block_len += ll;
                            loop_block_body[loop_block_len++] = '\n';
                        }
                        break;
                    }
                }

                /* --- Case-block accumulation mode (keyboard path) --- */
                if (in_case_block) {
                    const char *l = cmd_buf;
                    while (*l == ' ' || *l == '\t') l++;
                    /* Count case keywords in this line for nesting */
                    for (const char *p = cmd_buf; *p; p++) {
                        if ((strncmp(p, "case ", 5) == 0 || strncmp(p, "case\t", 5) == 0) &&
                            (p == cmd_buf || *(p-1) == ' ' || *(p-1) == '\t' || *(p-1) == ';'))
                            case_block_depth++;
                    }
                    if (strcmp(l, "esac") == 0 || strcmp(l, "esac;") == 0) {
                        case_block_depth--;
                        if (case_block_depth <= 0) {
                            /* Close case-block */
                            case_block_body[case_block_len] = '\0';
                            process_case_block(case_block_body);
                            in_case_block = 0;
                            case_block_len = 0;
                            break;
                        }
                    }
                    /* Append line to case-block body */
                    int ll = strlen(cmd_buf);
                    if (case_block_len + ll + 1 < CASE_BLOCK_BODY_MAX) {
                        memcpy(case_block_body + case_block_len, cmd_buf, ll);
                        case_block_len += ll;
                        case_block_body[case_block_len++] = '\n';
                    }
                    break;
                }

                /* --- Detect start of case-block (keyboard path) --- */
                {
                    const char *p = cmd_buf;
                    while (*p == ' ') p++;
                    if ((strncmp(p, "case ", 5) == 0 || strncmp(p, "case\t", 5) == 0) &&
                        !(p[4] && p[4] != ' ' && p[4] != '\t')) {
                        /* Check if single-line (has both "in" and "esac") */
                        int has_in = 0, has_esac = 0;
                        int in_sq = 0, in_dq = 0;
                        for (const char *q = cmd_buf; *q; q++) {
                            if (*q == '\'' && !in_dq) { in_sq = !in_sq; continue; }
                            if (*q == '"'  && !in_sq) { in_dq = !in_dq; continue; }
                            if (in_sq || in_dq) continue;
                            if (strncmp(q, " in", 3) == 0 && (q == cmd_buf || *(q-1) == ' ' || *(q-1) == '\t')) has_in = 1;
                            if (strncmp(q, "esac", 4) == 0 && (q == cmd_buf || *(q-1) == ' ' || *(q-1) == '\t')) has_esac = 1;
                        }
                        if (has_in && has_esac) {
                            /* Single-line case: process immediately */
                            process_case_block(cmd_buf);
                            break;
                        }
                        /* Multi-line case: start accumulation */
                        in_case_block = 1;
                        case_block_len = 0;
                        case_block_depth = 1;
                        int ll = strlen(cmd_buf);
                        if (case_block_len + ll + 1 < CASE_BLOCK_BODY_MAX) {
                            memcpy(case_block_body + case_block_len, cmd_buf, ll);
                            case_block_len += ll;
                            case_block_body[case_block_len++] = '\n';
                        }
                        break;
                    }
                }

                /* --- Function definition mode --- */
                if (in_func_def) {
                    const char *l = cmd_buf;
                    while (*l == ' ' || *l == '\t') l++;
                    if (strcmp(l, "}") == 0) {
                        func_brace--;
                        if (func_brace <= 0) {
                            /* Close function */
                            func_def_body[func_def_len] = '\0';
                            shell_func_define(func_def_name, func_def_body);
                            in_func_def = 0;
                        } else {
                            /* nested } inside body */
                            if (func_def_len + 2 < SHELL_FUNC_BODY_MAX) {
                                func_def_body[func_def_len++] = '}';
                                func_def_body[func_def_len++] = '\n';
                            }
                        }
                    } else {
                        /* Append line to body */
                        int ll = strlen(cmd_buf);
                        if (func_def_len + ll + 1 < SHELL_FUNC_BODY_MAX) {
                            memcpy(func_def_body + func_def_len, cmd_buf, ll);
                            func_def_len += ll;
                            func_def_body[func_def_len++] = '\n';
                        }
                        /* Count open braces */
                        for (const char *p = cmd_buf; *p; p++)
                            if (*p == '{') func_brace++;
                    }
                    break;
                }

                /* --- Detect function definition start --- */
                {
                    const char *p = cmd_buf;
                    while (*p == ' ') p++;
                    /* "function name() {" or "name() {" */
                    int is_func = 0;
                    const char *name_start = p;
                    if (strncmp(p, "function ", 9) == 0) {
                        p += 9;
                        while (*p == ' ') p++;
                        name_start = p;
                        is_func = 1;
                    }
                    /* Find name — up to '(' or space */
                    const char *name_end = name_start;
                    while (*name_end && *name_end != '(' && *name_end != ' ') name_end++;
                    if (*name_end == '(') {
                        /* Looks like a function definition */
                        int nlen = (int)(name_end - name_start);
                        if (nlen > 0 && nlen < SHELL_FUNC_NAME_MAX) {
                            /* Check ends with () or () { */
                            const char *brace = name_end;
                            while (*brace && *brace != '{') brace++;
                            if (*brace == '{' || is_func) {
                                memcpy(func_def_name, name_start, nlen);
                                func_def_name[nlen] = '\0';
                                func_def_body[0] = '\0';
                                func_def_len = 0;
                                func_brace = 1;
                                in_func_def = 1;
                                break;
                            }
                        }
                    }
                }

                /* --- Detect heredoc: cmd << WORD, or here-string: cmd <<< word --- */
                {
                    char *hpos = 0;
                    int  is_here_string = 0; /* 1 = <<<word, 0 = <<WORD */
                    for (char *p = cmd_buf; *p; p++) {
                        if (p[0] == '<' && p[1] == '<' && p[2] == '<') {
                            hpos = p;
                            is_here_string = 1;
                            break;
                        }
                        if (p[0] == '<' && p[1] == '<') {
                            hpos = p;
                            is_here_string = 0;
                            break;
                        }
                    }
                    if (hpos) {
                        /* Split command from redirection specifier */
                        *hpos = '\0';
                        char *const content_start = hpos + (is_here_string ? 3 : 2);
                        char *delim = content_start;
                        while (*delim == ' ') delim++;

                        /* Trim trailing whitespace from delimiter/content */
                        int dl = strlen(delim);
                        while (dl > 0 && (delim[dl-1] == ' ' || delim[dl-1] == '\r' || delim[dl-1] == '\n'))
                            delim[--dl] = '\0';

                        char here_buf[2048];
                        int here_len = 0;

                        if (is_here_string) {
                            /* Here-string: cmd <<< word — word is passed as stdin content */
                            int copylen = dl < (int)sizeof(here_buf) - 2 ? dl : (int)sizeof(here_buf) - 2;
                            memcpy(here_buf, delim, (size_t)copylen);
                            here_len = copylen;
                            here_buf[here_len++] = '\n';
                            here_buf[here_len] = '\0';
                        } else {
                            /* Heredoc: cmd << WORD — read lines interactively until delimiter */
                            for (;;) {
                                kprintf("> ");
                                char hline[CMD_BUF_SIZE]; int hl = 0;
                                char hc;
                                while ((hc = keyboard_getchar()) != '\n' && hl < CMD_BUF_SIZE - 1)
                                    hline[hl++] = hc;
                                putchar_both('\n');
                                hline[hl] = '\0';
                                if (strcmp(hline, delim) == 0) break;
                                if (here_len + hl + 1 < (int)sizeof(here_buf)) {
                                    memcpy(here_buf + here_len, hline, (size_t)hl);
                                    here_len += hl;
                                    here_buf[here_len++] = '\n';
                                }
                            }
                            here_buf[here_len] = '\0';
                        }

                        /* Write content as pipe file for the command */
                        char hpipe[32];
                        struct process *self = process_get_current();
                        snprintf(hpipe, sizeof(hpipe), "/.heredoc_%u",
                                 self ? self->pid : 0u);
                        vfs_write(hpipe, here_buf, (uint32_t)here_len);

                        /* Trim trailing space from cmd part */
                        char *ct = cmd_buf + strlen(cmd_buf) - 1;
                        while (ct >= cmd_buf && *ct == ' ') *ct-- = '\0';

                        /* Append pipe file as argument */
                        int clen = strlen(cmd_buf);
                        if (clen + 1 + strlen(hpipe) < CMD_BUF_SIZE - 1) {
                            cmd_buf[clen] = ' ';
                            strncpy(cmd_buf + clen + 1, hpipe, CMD_BUF_SIZE - clen - 2);
                            cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                        }
                        cmd_len = strlen(cmd_buf);
                    }
                }

                process_cmd();
                break;
            } else if (c == KEY_UP) {
                if (history_pos > 0 && history_pos > history_count - HISTORY_SIZE) {
                    history_pos--;
                    erase_line(cmd_len);
                    strcpy(cmd_buf, history[history_pos % HISTORY_SIZE]);
                    cmd_len = strlen(cmd_buf);
                    kprintf("%s", cmd_buf);
                }
            } else if (c == KEY_DOWN) {
                erase_line(cmd_len);
                if (history_pos < history_count - 1) {
                    history_pos++;
                    strcpy(cmd_buf, history[history_pos % HISTORY_SIZE]);
                    cmd_len = strlen(cmd_buf);
                    kprintf("%s", cmd_buf);
                } else {
                    history_pos = history_count;
                    cmd_buf[0] = '\0';
                    cmd_len = 0;
                }
            } else if (c == '\b') {
                if (cmd_len > 0) {
                    cmd_len--;
                    putchar_both('\b');
                }
            } else if (c == '\t') {
                cmd_buf[cmd_len] = '\0';
                shell_tab_complete(cmd_buf, &cmd_len);
            } else if (cmd_len < CMD_BUF_SIZE - 1) {
                cmd_buf[cmd_len++] = c;
                putchar_both(c);
            }
        }
    }
}

void shell_init(void) {
}

/* Read a line from keyboard into buf (up to max-1 chars) */
void shell_read_line(char *buf, int max) {
    int len = 0;
    while (1) {
        char c = keyboard_getchar();
        if (c == '\n') {
            putchar_both('\n');
            break;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                putchar_both('\b');
            }
        } else if (len < max - 1) {
            buf[len++] = c;
            putchar_both(c);
        }
    }
    buf[len] = '\0';
}
