/* shell.c — Shell core: input loop, dispatch, history */

#include "shell.h"
#include "shell_cmds.h"
#include "vga.h"
#include "keyboard.h"
#include "printf.h"
#include "string.h"
#include "serial.h"
#include "editor.h"
#include "vfs.h"
#include "fs.h"
#include "pipe.h"
#include "process.h"
#include "scheduler.h"

#define MAX_VAR_NAME 32

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
static int glob_match(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1; /* trailing * matches everything */
            while (*str) {
                if (glob_match(pat, str)) return 1;
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
            static char gnames[GLOB_MAX_MATCHES][FS_MAX_NAME];
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
};

static void shell_capture_cb(char c, void *ctx) {
    struct shell_capture_ctx *sc = (struct shell_capture_ctx *)ctx;
    if (*sc->len < 4095) sc->buf[(*sc->len)++] = c;
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
            dst[di++] = *src++;
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
                if (r < 0) { if (di < dst_max - 1) dst[di++] = '-'; r = -r; }
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
                static char sub_out[512];
                int sub_len = 0;
                void (*sh_hook)(char, void *) = 0; void *sh_ctx = 0;
                kprintf_get_hook(&sh_hook, &sh_ctx);
                struct shell_capture_ctx sub_ctx = { sub_out, &sub_len };
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
                    while (*src >= '0' && *src <= '9') idx = idx * 10 + (*src++ - '0');
                    if (*src == ']') src++;
                    if (*src == '}') src++;
                    void *arrp = shell_array_get(aname);
                    if (arrp) {
                        struct shell_array *a = (struct shell_array *)arrp;
                        if (idx >= 0 && idx < a->count) {
                            const char *val = a->elems[idx];
                            while (*val && di < dst_max - 1) dst[di++] = *val++;
                        }
                    }
                    continue;
                }
                /* ${name} — regular variable in braces */
                if (*src == '}') src++;
                const char *val = shell_var_get(aname);
                while (*val && di < dst_max - 1) dst[di++] = *val++;
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
            while (*val && di < dst_max - 1)
                dst[di++] = *val++;
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
    static char expanded[CMD_BUF_SIZE];
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
            static char alias_expanded[CMD_BUF_SIZE];
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
            static char glob_out[CMD_BUF_SIZE];
            /* Copy first word verbatim */
            int fw_len = (int)(first_end - cmd_buf);
            memcpy(glob_out, cmd_buf, fw_len);
            /* Expand the rest */
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

    /* --- Check for variable assignment: NAME=value (no spaces around =) --- */
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
            } else {
                shell_var_set(name, p + 1);
            }
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
            kprintf("[%u] %s\n", (uint64_t)p->pid, bg_slots[slot].cmd);
        } else {
            kprintf("Failed to create background process\n");
        }
        return;
    }

    /* --- Check for pipe: cmd1 | cmd2 --- */
    char *pipe_pos = 0;
    for (char *p = cmd; *p; p++) {
        if (*p == '|') { pipe_pos = p; break; }
    }
    if (pipe_pos) {
        *pipe_pos = '\0';
        char *left = cmd;
        char *right = pipe_pos + 1;
        while (*right == ' ') right++;
        /* Trim trailing spaces from left */
        char *lt = pipe_pos - 1;
        while (lt > left && *lt == ' ') { *lt = '\0'; lt--; }

        /* Create a temporary pipe file */
        const char *pipe_file = "/.pipe_tmp";
        /* Execute left side, capturing output to pipe file */
        /* We use kprintf output redirect via a buffer */
        static char pipe_buf[4096];
        int pipe_len = 0;
        /* Save kprintf hook state and redirect output */
        void (*saved_hook)(char, void*) = 0;
        void *saved_ctx = 0;
        kprintf_get_hook(&saved_hook, &saved_ctx);

        /* Parse left command */
        char *lcmd = left; while (*lcmd == ' ') lcmd++;
        char *largs = lcmd;
        while (*largs && *largs != ' ') largs++;
        if (*largs) { *largs = '\0'; largs++; while (*largs == ' ') largs++; }
        else largs = 0;

        /* Execute left, capture to pipe_buf via kprintf hook */
        pipe_len = 0;
        struct shell_capture_ctx pipe_ctx = { pipe_buf, &pipe_len };
        kprintf_set_hook(shell_capture_cb, &pipe_ctx);
        shell_exec_cmd(lcmd, largs);
        kprintf_set_hook(saved_hook, saved_ctx);
        pipe_buf[pipe_len] = '\0';

        /* Write captured output to temp file */
        vfs_write(pipe_file, pipe_buf, (uint32_t)pipe_len);

        /* Parse right command and inject pipe file as arg */
        char *rcmd = right;
        char *rargs = rcmd;
        while (*rargs && *rargs != ' ') rargs++;
        if (*rargs) { *rargs = '\0'; rargs++; while (*rargs == ' ') rargs++; }
        else rargs = 0;

        /* For pipe: append pipe_file as last arg if no file arg given */
        char combined_args[CMD_BUF_SIZE];
        if (rargs && rargs[0]) {
            int n = strlen(rargs);
            if (n > CMD_BUF_SIZE - 2) n = CMD_BUF_SIZE - 2;
            memcpy(combined_args, rargs, n);
            combined_args[n] = ' ';
            strncpy(combined_args + n + 1, pipe_file, CMD_BUF_SIZE - n - 2);
            combined_args[CMD_BUF_SIZE - 1] = '\0';
        } else {
            strncpy(combined_args, pipe_file, CMD_BUF_SIZE - 1);
            combined_args[CMD_BUF_SIZE - 1] = '\0';
        }
        shell_exec_cmd(rcmd, combined_args);
        vfs_unlink(pipe_file);
        return;
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
            static char iredir_buf[4096];
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
            static char iredir_args[CMD_BUF_SIZE];
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
        static char redir_buf[4096];
        int redir_len = 0;
        void (*saved_hook)(char, void*) = 0;
        void *saved_ctx = 0;
        kprintf_get_hook(&saved_hook, &saved_ctx);
        struct shell_capture_ctx redir_ctx = { redir_buf, &redir_len };
        kprintf_set_hook(shell_capture_cb, &redir_ctx);
        shell_exec_cmd(lcmd, largs);
        kprintf_set_hook(saved_hook, saved_ctx);
        redir_buf[redir_len] = '\0';

        if (redir_append) {
            /* Read existing content, append */
            uint32_t existing = 0;
            char old[4096];
            if (vfs_read(filepath, old, 4095, &existing) == 0 && existing > 0) {
                int total = (int)existing + redir_len;
                if (total > 4095) total = 4095;
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
    /* Reset exit status; individual commands may override via shell_set_exit_status() */
    last_exit_status = 0;

    /* Per-command --help */
    if (args && strcmp(args, "--help") == 0) {
        if (strcmp(cmd, "echo") == 0)
            kprintf("Usage: echo [text]\n  Print text to console\n");
        else if (strcmp(cmd, "clear") == 0)
            kprintf("Usage: clear\n  Clear the screen\n");
        else if (strcmp(cmd, "meminfo") == 0)
            kprintf("Usage: meminfo\n  Show physical memory statistics\n");
        else if (strcmp(cmd, "ps") == 0)
            kprintf("Usage: ps\n  List all processes and their states\n");
        else if (strcmp(cmd, "uptime") == 0)
            kprintf("Usage: uptime\n  Show time since boot\n");
        else if (strcmp(cmd, "reboot") == 0)
            kprintf("Usage: reboot\n  Reboot the system\n");
        else if (strcmp(cmd, "shutdown") == 0)
            kprintf("Usage: shutdown\n  Power off via ACPI\n");
        else if (strcmp(cmd, "kill") == 0)
            kprintf("Usage: kill <pid> [signal]\n  Send signal to process (default signal 9 = SIGKILL)\n");
        else if (strcmp(cmd, "color") == 0)
            kprintf("Usage: color <fg> [bg]\n  Set console text/background color (0-15)\n");
        else if (strcmp(cmd, "hexdump") == 0)
            kprintf("Usage: hexdump <addr> [len]\n  Dump memory at address in hex (default len=64, max 256)\n");
        else if (strcmp(cmd, "date") == 0)
            kprintf("Usage: date\n  Show current date and time from RTC\n");
        else if (strcmp(cmd, "cpuinfo") == 0)
            kprintf("Usage: cpuinfo\n  Show CPU vendor and brand string\n");
        else if (strcmp(cmd, "history") == 0)
            kprintf("Usage: history\n  Show recent command history\n");
        else if (strcmp(cmd, "ls") == 0)
            kprintf("Usage: ls [path]\n  List directory contents (default /)\n");
        else if (strcmp(cmd, "cat") == 0)
            kprintf("Usage: cat <file>\n  Print file contents\n");
        else if (strcmp(cmd, "write") == 0)
            kprintf("Usage: write <file> <text>\n  Write text to file\n");
        else if (strcmp(cmd, "touch") == 0)
            kprintf("Usage: touch <file>\n  Create empty file\n");
        else if (strcmp(cmd, "rm") == 0)
            kprintf("Usage: rm <path>\n  Remove file or directory\n");
        else if (strcmp(cmd, "mkdir") == 0)
            kprintf("Usage: mkdir <dir>\n  Create directory\n");
        else if (strcmp(cmd, "stat") == 0)
            kprintf("Usage: stat <path>\n  Show file or directory metadata (type, size)\n");
        else if (strcmp(cmd, "format") == 0)
            kprintf("Usage: format\n  Format the filesystem (WARNING: destroys all data)\n");
        else if (strcmp(cmd, "edit") == 0)
            kprintf("Usage: edit <file>\n  Open file in text editor (Ctrl-S save, Ctrl-Q quit)\n");
        else if (strcmp(cmd, "exec") == 0)
            kprintf("Usage: exec <path>\n  Load and run a static ELF64 binary\n");
        else if (strcmp(cmd, "run") == 0)
            kprintf("Usage: run <path>\n  Execute a shell script file\n");
        else if (strcmp(cmd, "ifconfig") == 0)
            kprintf("Usage: ifconfig\n  Show network interface configuration (MAC, IP, mask, gateway)\n");
        else if (strcmp(cmd, "ping") == 0)
            kprintf("Usage: ping [ip|hostname]\n  Send ICMP echo request (default: gateway)\n");
        else if (strcmp(cmd, "dns") == 0)
            kprintf("Usage: dns <hostname>\n  Resolve hostname to IP address\n");
        else if (strcmp(cmd, "curl") == 0)
            kprintf("Usage: curl [-F] <url>\n  HTTP GET request. -F follows redirects (301/302/303/307/308, max 5)\n  Example: curl -F http://example.com/\n");
        else if (strcmp(cmd, "udpsend") == 0)
            kprintf("Usage: udpsend <ip> <port> <data>\n  Send a UDP datagram\n");
        else if (strcmp(cmd, "beep") == 0)
            kprintf("Usage: beep [freq] [ms]\n  Play tone on PC speaker (default 1000 Hz, 200 ms)\n");
        else if (strcmp(cmd, "play") == 0)
            kprintf("Usage: play <note> [note ...]\n  Play musical notes (C4 D4 E4 F4 G4 A4 B4 C5)\n");
        else if (strcmp(cmd, "mouse") == 0)
            kprintf("Usage: mouse\n  Show current mouse position and button state\n");
        else if (strcmp(cmd, "wc") == 0)
            kprintf("Usage: wc <file>\n  Count lines, words, and bytes in file\n");
        else if (strcmp(cmd, "head") == 0)
            kprintf("Usage: head <file> [n]\n  Show first n lines of file (default 10)\n");
        else if (strcmp(cmd, "tail") == 0)
            kprintf("Usage: tail <file> [n]\n  Show last n lines of file (default 10)\n");
        else if (strcmp(cmd, "cp") == 0)
            kprintf("Usage: cp <src> <dst>\n  Copy file\n");
        else if (strcmp(cmd, "mv") == 0)
            kprintf("Usage: mv <src> <dst>\n  Move or rename file\n");
        else if (strcmp(cmd, "grep") == 0)
            kprintf("Usage: grep <pattern> <file>\n  Search for pattern in file\n");
        else if (strcmp(cmd, "df") == 0)
            kprintf("Usage: df\n  Show disk usage statistics\n");
        else if (strcmp(cmd, "free") == 0)
            kprintf("Usage: free\n  Show memory usage (total/used/free)\n");
        else if (strcmp(cmd, "whoami") == 0)
            kprintf("Usage: whoami\n  Show current process PID and name\n");
        else if (strcmp(cmd, "hostname") == 0)
            kprintf("Usage: hostname\n  Print system hostname\n");
        else if (strcmp(cmd, "env") == 0)
            kprintf("Usage: env\n  Print environment variables (PID, NAME, IP, UPTIME, ...)\n");
        else if (strcmp(cmd, "xxd") == 0)
            kprintf("Usage: xxd <file>\n  Hex dump file contents (first 256 bytes)\n");
        else if (strcmp(cmd, "sleep") == 0)
            kprintf("Usage: sleep <seconds>\n  Pause for n seconds (max 60)\n");
        else if (strcmp(cmd, "seq") == 0)
            kprintf("Usage: seq <end>  or  seq <start> <end>\n  Print integer sequence\n");
        else if (strcmp(cmd, "arp") == 0)
            kprintf("Usage: arp\n  Show ARP cache entries\n");
        else if (strcmp(cmd, "route") == 0)
            kprintf("Usage: route\n  Show routing table\n");
        else if (strcmp(cmd, "uname") == 0)
            kprintf("Usage: uname\n  Print system information\n");
        else if (strcmp(cmd, "lspci") == 0)
            kprintf("Usage: lspci\n  List PCI devices\n");
        else if (strcmp(cmd, "dmesg") == 0)
            kprintf("Usage: dmesg\n  Show kernel boot log\n");
        else if (strcmp(cmd, "cc") == 0)
            kprintf("Usage: cc <source.c> [output]\n  Compile C source to static ELF64 binary\n");
        else if (strcmp(cmd, "ccbuilder") == 0)
            kprintf("Usage: ccbuilder [-k|--keep-going] <manifest.txt>\n  Run manifest steps: cc/exec/run/echo\n");
        else if (strcmp(cmd, "sort") == 0)
            kprintf("Usage: sort <file>\n  Sort lines of a file alphabetically\n");
        else if (strcmp(cmd, "find") == 0)
            kprintf("Usage: find <pattern>\n  Search for files matching pattern\n");
        else if (strcmp(cmd, "calc") == 0)
            kprintf("Usage: calc <expression>\n  Evaluate arithmetic expression (+, -, *, /, %%, parens)\n");
        else if (strcmp(cmd, "uniq") == 0)
            kprintf("Usage: uniq <file>\n  Remove adjacent duplicate lines\n");
        else if (strcmp(cmd, "tr") == 0)
            kprintf("Usage: tr <from> <to> <file>\n  Translate characters in file\n");
        else if (strcmp(cmd, "tmux") == 0)
            kprintf("Usage: tmux\n  Terminal multiplexer (Ctrl-B prefix, see tmux --help)\n");
        else if (strcmp(cmd, "jobs") == 0)
            kprintf("Usage: jobs\n  List background jobs\n");
        else if (strcmp(cmd, "fg") == 0)
            kprintf("Usage: fg <pid|%%job>\n  Bring background job to foreground\n");
        else if (strcmp(cmd, "bg") == 0)
            kprintf("Usage: bg [pid|%%job]\n  Resume a stopped job in the background\n");
        else if (strcmp(cmd, "renice") == 0)
            kprintf("Usage: renice <priority> <pid>\n  Change process priority, 0=high..3=idle\n");
        else if (strcmp(cmd, "wait") == 0)
            kprintf("Usage: wait <pid>\n  Wait for a process to finish\n");
        else if (strcmp(cmd, "help") == 0)
            kprintf("Usage: help\n  List all available commands\n");
        else if (strcmp(cmd, "exit") == 0)
            kprintf("Usage: exit\n  Disconnect telnet session\n");
        else if (strcmp(cmd, "printf") == 0)
            kprintf("Usage: printf <format> [args...]\n  Format and print. Supports \\n \\t %%s %%d\n");
        else if (strcmp(cmd, "time") == 0)
            kprintf("Usage: time <command> [args...]\n  Measure execution time of a command\n");
        else if (strcmp(cmd, "strings") == 0)
            kprintf("Usage: strings <file>\n  Print printable strings found in a file\n");
        else if (strcmp(cmd, "tac") == 0)
            kprintf("Usage: tac <file>\n  Print file lines in reverse order\n");
        else if (strcmp(cmd, "base64") == 0)
            kprintf("Usage: base64 <file>\n  Encode file contents as base64\n");
        else if (strcmp(cmd, "cmos") == 0)
            kprintf("Usage: cmos\n  Show CMOS/NVRAM hardware configuration\n");
        else if (strcmp(cmd, "hwinfo") == 0)
            kprintf("Usage: hwinfo\n  Show comprehensive hardware information\n");
        else if (strcmp(cmd, "fbinfo") == 0)
            kprintf("Usage: fbinfo\n  Show active display backend and framebuffer geometry\n");
        else if (strcmp(cmd, "gui") == 0)
            kprintf("Usage: gui\n  Launch GUI desktop environment (experimental)\n");
        else if (strcmp(cmd, "serial") == 0)
            kprintf("Usage: serial status | serial write <text>\n  COM1 serial port operations\n");
        else if (strcmp(cmd, "fold") == 0)
            kprintf("Usage: fold [-w width] <file>\n  Wrap lines to given width (default 80)\n");
        else if (strcmp(cmd, "expand") == 0)
            kprintf("Usage: expand [-t tabstop] <file>\n  Convert tabs to spaces (default tabstop 8)\n");
        else if (strcmp(cmd, "comm") == 0)
            kprintf("Usage: comm [-123] <file1> <file2>\n  Compare two sorted files line by line\n");
        else if (strcmp(cmd, "split") == 0)
            kprintf("Usage: split [-l lines] <file> [prefix]\n  Split file into chunks (default 100 lines each)\n");
        else if (strcmp(cmd, "which") == 0)
            kprintf("Usage: which <command>\n  Show whether command is a shell built-in\n");
        else if (strcmp(cmd, "ln") == 0)
            kprintf("Usage: ln <source> <dest>\n  Create a copy-link to a file\n");
        else if (strcmp(cmd, "true") == 0)
            kprintf("Usage: true\n  Do nothing, successfully\n");
        else if (strcmp(cmd, "false") == 0)
            kprintf("Usage: false\n  Do nothing, unsuccessfully\n");
        else if (strcmp(cmd, "more") == 0)
            kprintf("Usage: more <file>\n  Display file one page at a time\n");
        else if (strcmp(cmd, "file") == 0)
            kprintf("Usage: file <path>\n  Determine file type (text/binary/ELF/directory)\n");
        else if (strcmp(cmd, "nslookup") == 0)
            kprintf("Usage: nslookup <hostname>\n  Resolve hostname to IP address\n");
        else
            kprintf("Unknown command: %s\n", cmd);
        return;
    }

    if (strcmp(cmd, "help") == 0) cmd_help();
    else if (strcmp(cmd, "echo") == 0) cmd_echo(args);
    else if (strcmp(cmd, "clear") == 0) vga_clear();
    else if (strcmp(cmd, "meminfo") == 0) cmd_meminfo();
    else if (strcmp(cmd, "ps") == 0) cmd_ps();
    else if (strcmp(cmd, "uptime") == 0) cmd_uptime();
    else if (strcmp(cmd, "reboot") == 0) cmd_reboot();
    else if (strcmp(cmd, "shutdown") == 0) cmd_shutdown();
    else if (strcmp(cmd, "kill") == 0) cmd_kill(args);
    else if (strcmp(cmd, "color") == 0) cmd_color(args);
    else if (strcmp(cmd, "hexdump") == 0) cmd_hexdump(args);
    else if (strcmp(cmd, "date") == 0) cmd_date();
    else if (strcmp(cmd, "cpuinfo") == 0) cmd_cpuinfo();
    else if (strcmp(cmd, "history") == 0) cmd_history_show();
    else if (strcmp(cmd, "ls") == 0) cmd_ls(args);
    else if (strcmp(cmd, "cat") == 0) cmd_cat(args);
    else if (strcmp(cmd, "write") == 0) cmd_write(args);
    else if (strcmp(cmd, "touch") == 0) cmd_touch(args);
    else if (strcmp(cmd, "rm") == 0) cmd_rm(args);
    else if (strcmp(cmd, "mkdir") == 0) cmd_mkdir(args);
    else if (strcmp(cmd, "stat") == 0) cmd_stat_file(args);
    else if (strcmp(cmd, "format") == 0) cmd_format_disk();
    else if (strcmp(cmd, "edit") == 0) editor_open(args);
    else if (strcmp(cmd, "exec") == 0) cmd_exec(args);
    else if (strcmp(cmd, "run") == 0) cmd_run(args);
    else if (strcmp(cmd, "beep") == 0) cmd_beep(args);
    else if (strcmp(cmd, "play") == 0) cmd_play(args);
    else if (strcmp(cmd, "mouse") == 0) cmd_mouse_status();
    else if (strcmp(cmd, "udpsend") == 0) cmd_udpsend(args);
    else if (strcmp(cmd, "ifconfig") == 0) cmd_ifconfig();
    else if (strcmp(cmd, "ping") == 0) cmd_ping(args);
    else if (strcmp(cmd, "dns") == 0) cmd_dns(args);
    else if (strcmp(cmd, "curl") == 0) cmd_curl(args);
    else if (strcmp(cmd, "wc") == 0) cmd_wc(args);
    else if (strcmp(cmd, "head") == 0) cmd_head(args);
    else if (strcmp(cmd, "tail") == 0) cmd_tail(args);
    else if (strcmp(cmd, "cp") == 0) cmd_cp(args);
    else if (strcmp(cmd, "mv") == 0) cmd_mv(args);
    else if (strcmp(cmd, "grep") == 0) cmd_grep(args);
    else if (strcmp(cmd, "df") == 0) cmd_df();
    else if (strcmp(cmd, "free") == 0) cmd_free();
    else if (strcmp(cmd, "whoami") == 0) cmd_whoami();
    else if (strcmp(cmd, "hostname") == 0) cmd_hostname();
    else if (strcmp(cmd, "env") == 0) cmd_env();
    else if (strcmp(cmd, "xxd") == 0) cmd_xxd(args);
    else if (strcmp(cmd, "sleep") == 0) cmd_sleep(args);
    else if (strcmp(cmd, "seq") == 0) cmd_seq(args);
    else if (strcmp(cmd, "arp") == 0) cmd_arp();
    else if (strcmp(cmd, "route") == 0) cmd_route();
    else if (strcmp(cmd, "uname") == 0) cmd_uname();
    else if (strcmp(cmd, "lspci") == 0) cmd_lspci();
    else if (strcmp(cmd, "dmesg") == 0) cmd_dmesg();
    else if (strcmp(cmd, "cc") == 0) cmd_cc(args);
    else if (strcmp(cmd, "ccbuilder") == 0) cmd_ccbuilder(args);
    else if (strcmp(cmd, "sort") == 0) cmd_sort(args);
    else if (strcmp(cmd, "find") == 0) cmd_find(args);
    else if (strcmp(cmd, "calc") == 0) cmd_calc(args);
    else if (strcmp(cmd, "uniq") == 0) cmd_uniq(args);
    else if (strcmp(cmd, "tr") == 0) cmd_tr(args);
    else if (strcmp(cmd, "tmux") == 0) cmd_tmux(args);
    else if (strcmp(cmd, "jobs") == 0) cmd_jobs();
    else if (strcmp(cmd, "fg") == 0) cmd_fg(args);
    else if (strcmp(cmd, "bg") == 0) cmd_bg(args);
    else if (strcmp(cmd, "wait") == 0) cmd_wait(args);
    else if (strcmp(cmd, "tee") == 0) cmd_tee(args);
    else if (strcmp(cmd, "cut") == 0) cmd_cut(args);
    else if (strcmp(cmd, "paste") == 0) cmd_paste(args);
    else if (strcmp(cmd, "basename") == 0) cmd_basename(args);
    else if (strcmp(cmd, "dirname") == 0) cmd_dirname(args);
    else if (strcmp(cmd, "yes") == 0) cmd_yes(args);
    else if (strcmp(cmd, "rev") == 0) cmd_rev(args);
    else if (strcmp(cmd, "nl") == 0) cmd_nl(args);
    else if (strcmp(cmd, "du") == 0) cmd_du(args);
    else if (strcmp(cmd, "id") == 0) cmd_id(args);
    else if (strcmp(cmd, "diff") == 0) cmd_diff(args);
    else if (strcmp(cmd, "md5sum") == 0) cmd_md5sum(args);
    else if (strcmp(cmd, "od") == 0) cmd_od(args);
    else if (strcmp(cmd, "expr") == 0) cmd_expr(args);
    else if (strcmp(cmd, "test") == 0) cmd_test(args);
    else if (strcmp(cmd, "[") == 0) cmd_test(args);
    else if (strcmp(cmd, "xargs") == 0) cmd_xargs(args);
    else if (strcmp(cmd, "printf") == 0) cmd_printf(args);
    else if (strcmp(cmd, "time") == 0) cmd_time(args);
    else if (strcmp(cmd, "strings") == 0) cmd_strings(args);
    else if (strcmp(cmd, "tac") == 0) cmd_tac(args);
    else if (strcmp(cmd, "base64") == 0) cmd_base64(args);
    else if (strcmp(cmd, "cmos") == 0) cmd_cmos();
    else if (strcmp(cmd, "hwinfo") == 0) cmd_hwinfo();
    else if (strcmp(cmd, "fbinfo") == 0) cmd_fbinfo();
    else if (strcmp(cmd, "gui") == 0) cmd_gui();
    else if (strcmp(cmd, "serial") == 0) cmd_serial(args);
    else if (strcmp(cmd, "lsusb") == 0) cmd_lsusb();
    else if (strcmp(cmd, "lsblk") == 0) cmd_lsblk();
    else if (strcmp(cmd, "fat") == 0) cmd_fat(args);
    else if (strcmp(cmd, "chmod") == 0) cmd_chmod(args);
    else if (strcmp(cmd, "chown") == 0) cmd_chown(args);
    else if (strcmp(cmd, "login") == 0) cmd_login(args);
    else if (strcmp(cmd, "logout") == 0) cmd_logout();
    else if (strcmp(cmd, "useradd") == 0) cmd_useradd(args);
    else if (strcmp(cmd, "userdel") == 0) cmd_userdel(args);
    else if (strcmp(cmd, "passwd") == 0) cmd_passwd(args);
    else if (strcmp(cmd, "users") == 0) cmd_users();
    else if (strcmp(cmd, "capprof") == 0) cmd_capprof(args);
    else if (strcmp(cmd, "service") == 0) cmd_service(args);
    else if (strcmp(cmd, "fold") == 0) cmd_fold(args);
    else if (strcmp(cmd, "expand") == 0) cmd_expand(args);
    else if (strcmp(cmd, "comm") == 0) cmd_comm(args);
    else if (strcmp(cmd, "split") == 0) cmd_split(args);
    else if (strcmp(cmd, "top") == 0) cmd_top();
    else if (strcmp(cmd, "sed") == 0) cmd_sed(args);
    else if (strcmp(cmd, "tar") == 0) cmd_tar(args);
    else if (strcmp(cmd, "which") == 0) cmd_which(args);
    else if (strcmp(cmd, "ln") == 0) cmd_ln(args);
    else if (strcmp(cmd, "true") == 0) cmd_true(args);
    else if (strcmp(cmd, "false") == 0) cmd_false(args);
    else if (strcmp(cmd, "more") == 0) cmd_more(args);
    else if (strcmp(cmd, "file") == 0) cmd_file(args);
    else if (strcmp(cmd, "nslookup") == 0) cmd_nslookup(args);
    else if (strcmp(cmd, "nc") == 0) cmd_nc(args);
    else if (strcmp(cmd, "wget") == 0) cmd_wget(args);
    else if (strcmp(cmd, "watch") == 0) cmd_watch(args);
    else if (strcmp(cmd, "sha256sum") == 0) cmd_sha256sum(args);
    else if (strcmp(cmd, "alias") == 0) cmd_alias(args);
    else if (strcmp(cmd, "unalias") == 0) cmd_unalias(args);
    else if (strcmp(cmd, "readlink") == 0) cmd_readlink(args);
    else if (strcmp(cmd, "cd") == 0) cmd_cd(args);
    else if (strcmp(cmd, "pwd") == 0) cmd_pwd();
    else if (strcmp(cmd, "nice") == 0) cmd_nice(args);
    else if (strcmp(cmd, "renice") == 0) cmd_renice(args);
    else if (strcmp(cmd, "awk") == 0) cmd_awk(args);
    else if (strcmp(cmd, "netstat") == 0) cmd_netstat(args);
    else if (strcmp(cmd, "trap") == 0) cmd_trap(args);
    else if (strcmp(cmd, "rawsend") == 0) cmd_rawsend(args);
    else { kprintf("Unknown command: %s\n", cmd); last_exit_status = 127; }
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
                        static char here_buf[2048];
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
                        const char *hpipe = "/.heredoc_tmp";
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
