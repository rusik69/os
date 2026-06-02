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

static void process_cmd(void) {
    char *cmd = cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

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
            /* Execute each line of the function body */
            char body_copy[SHELL_FUNC_BODY_MAX];
            strncpy(body_copy, fbody, SHELL_FUNC_BODY_MAX - 1);
            body_copy[SHELL_FUNC_BODY_MAX - 1] = '\0';
            char *line = body_copy;
            while (*line) {
                char *nl = line;
                while (*nl && *nl != '\n' && *nl != ';') nl++;
                char saved = *nl; *nl = '\0';
                char *l = line;
                while (*l == ' ' || *l == '\t') l++;
                if (*l) {
                    strncpy(cmd_buf, l, CMD_BUF_SIZE - 1);
                    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
                    cmd_len = (int)strlen(cmd_buf);
                    process_cmd();
                }
                *nl = saved;
                line = (*nl) ? nl + 1 : nl;
            }
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
/* --- Function definition state shared between telnet and keyboard paths --- */
static int    s_in_func_def  = 0;
static char   s_func_def_name[SHELL_FUNC_NAME_MAX];
static char   s_func_def_body[SHELL_FUNC_BODY_MAX];
static int    s_func_def_len = 0;
static int    s_func_brace   = 0;

void shell_process_line(const char *line) {
    if (!line || !*line) return;

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

    for (;;) {
        if (in_func_def)
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
                if (!in_func_def) history_add(cmd_buf);

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

                /* --- Detect heredoc: cmd << WORD --- */
                {
                    char *hpos = 0;
                    for (char *p = cmd_buf; *p; p++) {
                        if (p[0] == '<' && p[1] == '<') { hpos = p; break; }
                    }
                    if (hpos) {
                        /* Split command from heredoc delimiter */
                        *hpos = '\0';
                        char *delim = hpos + 2;
                        while (*delim == ' ') delim++;
                        /* Trim trailing whitespace from delimiter */
                        int dl = strlen(delim);
                        while (dl > 0 && (delim[dl-1] == ' ' || delim[dl-1] == '\r' || delim[dl-1] == '\n'))
                            delim[--dl] = '\0';

                        /* Collect heredoc lines into pipe buffer */
                        char here_buf[2048];
                        int here_len = 0;
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
                                memcpy(here_buf + here_len, hline, hl);
                                here_len += hl;
                                here_buf[here_len++] = '\n';
                            }
                        }
                        here_buf[here_len] = '\0';

                        /* Write heredoc content as pipe file for the command */
                        char hpipe[32];
                        struct process *self = process_get_current();
                        snprintf(hpipe, sizeof(hpipe), "/.heredoc_%u",
                                 self ? self->pid : 0u);
                        vfs_write(hpipe, here_buf, (uint32_t)here_len);

                        /* Trim trailing space from cmd part */
                        char *ct = cmd_buf + strlen(cmd_buf) - 1;
                        while (ct >= cmd_buf && *ct == ' ') *ct-- = '\0';

                        /* Append heredoc file as argument */
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
