/* sh.c — POSIX-ish shell for the Hermes OS userspace
 *
 * Expanded shell (D280): tokenizer, AST executor, pipelines, subshells,
 * variables (local/export/env), indexed arrays, arithmetic $(()), parameter
 * expansion (${var:-}, ${var:=}, ${var:+}, ${var#}, ${var##}, ${var%},
 * ${var%%}, ${#var}, ${var}), I/O redirections (< > >> 2> << >> <<<),
 * job control (jobs/bg/fg/kill/&), functions (recursion, local), conditionals
 * (if/then/else, case, test/[), loops (for/while/until + break/continue),
 * trap, filename globbing, alias expansion, and set -e/-u/-o pipefail.
 *
 * Built for the freestanding libc (no stdio FILE*, no POSIX dirent wrappers);
 * uses raw syscalls (read/write/open/close/fork/execve/waitpid/pipe/dup2/...).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "stdarg.h"
#include "string.h"
#include "unistd.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* Exit status decoding macros (Linux-compatible encoding).
 * Guarded behind SH_UNIT_TEST: the system <stdlib.h> already provides these. */
#ifndef SH_UNIT_TEST
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) ((((s) & 0x7f) != 0) && (((s) & 0x7f) < 0x7f))
#define WTERMSIG(s) ((s) & 0x7f)
#define WNOHANG 1
#endif

/* Signals (libc provides these in unistd.h) */
#ifndef SIG_IGN
#define SIG_IGN ((void (*)(int))1)
#define SIG_DFL ((void (*)(int))0)
#endif

/* stat mode helpers (libc has no macros) */
#ifndef S_IFMT
#define S_IFMT 0xF000
#define S_IFREG 0x8000
#define S_IFDIR 0x4000
#endif
#ifndef S_ISREG /* system headers may already define these on host */
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

/* freestanding-libc missing helpers.
 * Guarded behind SH_UNIT_TEST so the host-side unit test (tests/unit/test_sh.c)
 * can include this file against the real glibc without redefinition errors.
 * In a normal OS build these shims exist because the freestanding libc lacks
 * them; under SH_UNIT_TEST we use the system's. Behavior is unchanged. */
#ifndef SH_UNIT_TEST
static char *strdup(const char *s) {
    if (!s)
        return 0;
    int n = 0;
    while (s[n])
        n++;
    char *r = malloc(n + 1);
    if (!r)
        return 0;
    for (int i = 0; i <= n; i++)
        r[i] = s[i];
    return r;
}
static long atol(const char *s) {
    long v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+')
        s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a)
                return (char *)s;
    return 0;
}
#endif /* !SH_UNIT_TEST */

static char *sh_strchr(const char *s, int c) {
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return 0;
}
#ifndef SH_UNIT_TEST
static char *strncat(char *dst, const char *src, int n) {
    int i = 0;
    while (dst[i])
        i++;
    int j = 0;
    while (src[j] && j < n) {
        dst[i++] = src[j++];
    }
    dst[i] = '\0';
    return dst;
}
#endif /* !SH_UNIT_TEST */

static char *itoa_buf(long v);
static int fnmatch_simple(const char *pat, const char *str);

/* Forward declarations (defined later in file) */
static int getpid_impl(void);
static int source_file(const char *path);
static int run_line(const char *line);
static struct ast_node *parse_list(const char *line);
static int cmd_test(int argc, char **argv);

/* Control-flow globals */
static int g_return_code;
static int g_did_return;
static int g_loop_ctl; /* 1=break, 2=continue */
static int opt_xtrace;

/* ── Config ──────────────────────────────────────────────────── */
#define MAX_LINE 4096
#define MAX_ARGS 64
#define MAX_ENV 128
#ifndef PATH_MAX /* system headers may already define it on host */
#define PATH_MAX 256
#endif
#define MAX_NODES 256
#define MAX_SEGS 16
#define MAX_JOBS 64
#define MAX_TRAPS 32
#define MAX_ALIASES 64
#define MAX_NODE_TOKENS 32

/* ── Environment ─────────────────────────────────────────────── */
static char *sh_env[MAX_ENV];
static int sh_env_count;
static int last_exit_code;

/* Shell options */
static int opt_errexit = 0;  /* set -e */
static int opt_nounset = 0;  /* set -u */
static int opt_pipefail = 0; /* set -o pipefail */

static void sh_init_env(void) {
    sh_env[0] = "PATH=/bin";
    sh_env[1] = "HOME=/";
    sh_env[2] = "SHELL=/bin/sh";
    sh_env[3] = "IFS= \t\n";
    sh_env_count = 4;
}

static char *sh_getenv(const char *name);
static int var_is_readonly(const char *name);
static char *sh_getenv(const char *name) {
    unsigned long nlen = strlen(name);
    for (int i = 0; i < sh_env_count && sh_env[i]; i++) {
        if (strncmp(sh_env[i], name, nlen) == 0 && sh_env[i][nlen] == '=')
            return sh_env[i] + nlen + 1;
    }
    return 0;
}
static int sh_setenv(const char *var, const char *val) {
    unsigned long vlen = strlen(var);
    if (vlen == 0)
        return -1;
    if (var_is_readonly(var))
        return -1;
    for (int i = 0; i < sh_env_count && sh_env[i]; i++) {
        if (strncmp(sh_env[i], var, vlen) == 0 && sh_env[i][vlen] == '=') {
            char *new_entry = malloc(vlen + 1 + strlen(val) + 1);
            if (!new_entry)
                return -1;
            char *p = new_entry;
            while (*var)
                *p++ = *var++;
            *p++ = '=';
            while (*val)
                *p++ = *val++;
            *p = '\0';
            free(sh_env[i]);
            sh_env[i] = new_entry;
            return 0;
        }
    }
    if (sh_env_count >= MAX_ENV - 1)
        return -1;
    char *new_entry = malloc(vlen + 1 + strlen(val) + 1);
    if (!new_entry)
        return -1;
    char *p = new_entry;
    while (*var)
        *p++ = *var++;
    *p++ = '=';
    while (*val)
        *p++ = *val++;
    *p = '\0';
    sh_env[sh_env_count++] = new_entry;
    sh_env[sh_env_count] = 0;
    return 0;
}

static int sh_unsetenv(const char *var) {
    unsigned long vlen = strlen(var);
    if (var_is_readonly(var))
        return -1;
    for (int i = 0; i < sh_env_count; i++) {
        if (sh_env[i] && strncmp(sh_env[i], var, vlen) == 0 && sh_env[i][vlen] == '=') {
            free(sh_env[i]);
            for (int j = i; j < sh_env_count - 1; j++)
                sh_env[j] = sh_env[j + 1];
            sh_env_count--;
            sh_env[sh_env_count] = 0;
            return 0;
        }
    }
    return -1;
}

/* ── Readonly variables ───────────────────────────────────────── */
#define MAX_READONLY 64
static char readonly_names[MAX_READONLY][32];
static int readonly_count;

static int var_is_readonly(const char *name) {
    for (int i = 0; i < readonly_count; i++)
        if (strcmp(readonly_names[i], name) == 0)
            return 1;
    return 0;
}

static void var_mark_readonly(const char *name) {
    if (var_is_readonly(name) || readonly_count >= MAX_READONLY)
        return;
    strncpy(readonly_names[readonly_count], name, 31);
    readonly_names[readonly_count][31] = '\0';
    readonly_count++;
}

/* ── Shell arrays ────────────────────────────────────────────── */
#define MAX_ARRAYS 32
#define MAX_ARRAY_ELEMS 64
#define MAX_ARRAY_ELEM_LEN 128
struct shell_array {
    char name[32];
    char elems[MAX_ARRAY_ELEMS][MAX_ARRAY_ELEM_LEN];
    int count;
};
static struct shell_array shell_arrays[MAX_ARRAYS];

static struct shell_array *array_find(const char *name) {
    for (int i = 0; i < MAX_ARRAYS; i++)
        if (shell_arrays[i].name[0] && strcmp(shell_arrays[i].name, name) == 0)
            return &shell_arrays[i];
    return 0;
}
static struct shell_array *array_get_or_create(const char *name) {
    struct shell_array *a = array_find(name);
    if (a)
        return a;
    for (int i = 0; i < MAX_ARRAYS; i++) {
        if (!shell_arrays[i].name[0]) {
            strncpy(shell_arrays[i].name, name, 31);
            shell_arrays[i].name[31] = '\0';
            shell_arrays[i].count = 0;
            return &shell_arrays[i];
        }
    }
    return 0;
}

/* ── Functions ───────────────────────────────────────────────── */
#define MAX_FUNCS 64
struct shell_func {
    char name[64];
    char body[2048];
    int has_body;
};
static struct shell_func shell_funcs[MAX_FUNCS];
static struct shell_func *func_find(const char *name) {
    for (int i = 0; i < MAX_FUNCS; i++)
        if (shell_funcs[i].has_body && strcmp(shell_funcs[i].name, name) == 0)
            return &shell_funcs[i];
    return 0;
}

/* ── Aliases ─────────────────────────────────────────────────── */
struct shell_alias {
    char name[64];
    char value[256];
    int used;
};
static struct shell_alias shell_aliases[MAX_ALIASES];
static int sh_alias_count;

/* ── Traps ───────────────────────────────────────────────────── */
struct shell_trap {
    int sig;
    char action[256];
};
static struct shell_trap shell_traps[MAX_TRAPS];
static int sh_trap_count;

/* ── Job table (best-effort; kernel does not report stopped) ──── */
struct shell_job {
    int pid;
    int pgid;
    int state; /* 0=running,1=done,2=stopped */
    char cmd[256];
    int notified;
};
static struct shell_job shell_jobs[MAX_JOBS];
static int sh_job_count;
static void job_add(int pid, const char *cmd) {
    int slot = -1;
    for (int i = 0; i < sh_job_count; i++)
        if (shell_jobs[i].state == 1) {
            slot = i;
            break;
        }
    if (slot < 0 && sh_job_count < MAX_JOBS)
        slot = sh_job_count++;
    if (slot < 0)
        return;
    shell_jobs[slot].pid = pid;
    shell_jobs[slot].pgid = pid;
    shell_jobs[slot].state = 0;
    shell_jobs[slot].notified = 0;
    strncpy(shell_jobs[slot].cmd, cmd, 255);
    shell_jobs[slot].cmd[255] = '\0';
}
static void job_reap(int options) {
    for (int i = 0; i < sh_job_count; i++) {
        if (shell_jobs[i].state != 1) {
            int st = 0;
            int r = waitpid(shell_jobs[i].pid, &st, options);
            if (r > 0) {
                if (WIFEXITED(st) || WIFSIGNALED(st))
                    shell_jobs[i].state = 1;
            }
        }
    }
}

/* ── AST ─────────────────────────────────────────────────────── */
enum node_type {
    NODE_LIST,     /* ; && || */
    NODE_PIPELINE, /* cmd | cmd | ... */
    NODE_COMMAND,  /* simple command */
    NODE_SUBSHELL,
    NODE_IF,
    NODE_WHILE,
    NODE_UNTIL,
    NODE_FOR,
    NODE_CASE,
    NODE_GROUP, /* { ... } */
    NODE_FUNCTION
};

struct redir {
    int type; /* 0:< 1:> 2:>> 3:2> 4:<<(heredoc) 5:<<<(herestr) 6:&> */
    char target[256];
};

struct ast_node {
    enum node_type type;
    /* list / pipeline children */
    struct ast_node *children[MAX_NODES];
    int nchildren;
    /* command */
    char *argv[MAX_ARGS];
    int argc;
    struct redir redirs[8];
    int nredirs;
    int background;
    int invert; /* ! cmd */
    /* assignments (VAR=val) before command */
    char *assigns[16][2];
    int nassigns;
    /* list ops: ';' '&' '|' (||) '&' (&&) between children */
    char ops[MAX_NODES];
    /* if/while/for/case */
    char *cond;      /* expression text for if/while/until */
    char *body;      /* body text */
    char *else_body; /* else text */
    char for_var[64];
    char *for_words[64];
    int for_nwords;
    /* case: pattern+action pairs (simplified: single pattern list) */
    char case_word[256];
    char *case_pat[64];
    char *case_act[64];
    int case_n;
    /* function */
    char func_name[64];
    char func_body[2048];
};

/* forward decls */
static int execute_node(struct ast_node *node);
static int run_line(const char *line);
static int execute_line_raw(const char *line);
static char *expand_word(const char *s, int *fail);

/* ── Tokenizer ───────────────────────────────────────────────── */
/* Splits a command string into tokens honoring quotes and operators.
 * Returns number of tokens (including operator tokens), or -1 on error.
 * Operators are returned as their own tokens: | && || ; & ( ) < > >> 2> << <<< */
struct token {
    char text[1024];
    int is_op;
    char op;        /* for is_op: '|' ';' '&' '(' ')' '<' '>' */
    int is_andand;  /* && */
    int is_oror;    /* || */
    int is_dand;    /* >> */
    int is_dlt;     /* << */
    int is_herestr; /* <<< */
    int is_fd2;     /* 2> */
    int has_quote_flag;
};
/* Forward declarations (struct token / enum node_type now visible) */
static struct ast_node *parse_if(struct token *toks, int nt, int *i);
static struct ast_node *parse_while(struct token *toks, int nt, int *i, int until);
static struct ast_node *parse_for(struct token *toks, int nt, int *i);
static struct ast_node *parse_case(struct token *toks, int nt, int *i);
static struct ast_node *parse_function(struct token *toks, int nt, int *i);
static struct ast_node *parse_group(struct token *toks, int nt, int *i);
static struct ast_node *parse_pipeline(struct token *toks, int nt, int *i);
static struct ast_node *parse_command(struct token *toks, int nt, int *i);
static struct ast_node *new_node(enum node_type t);
static int execute_node(struct ast_node *node);
static int execute_command_node(struct ast_node *node);
static int file_exists(const char *path, int type);
static int eval_test_binary(const char *op, const char *l, const char *r);
#ifndef SH_UNIT_TEST
static int sh_try_array_assign(char *line);
#endif
static int tokenize(const char *src, struct token *toks, int maxtoks);
static int tokenize(const char *src, struct token *toks, int maxtoks) {
    int nt = 0;
    const char *p = src;
    while (*p && nt < maxtoks) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        /* operators */
        if (p[0] == '|' && p[1] == '|') {
            toks[nt].is_op = 1;
            toks[nt].op = '|';
            toks[nt].is_oror = 1;
            toks[nt].text[0] = '|';
            toks[nt].text[1] = '|';
            toks[nt].text[2] = '\0';
            nt++;
            p += 2;
            continue;
        }
        if (p[0] == '&' && p[1] == '&') {
            toks[nt].is_op = 1;
            toks[nt].op = '&';
            toks[nt].is_andand = 1;
            toks[nt].text[0] = '&';
            toks[nt].text[1] = '&';
            toks[nt].text[2] = '\0';
            nt++;
            p += 2;
            continue;
        }
        if (p[0] == '>' && p[1] == '>') {
            toks[nt].is_op = 1;
            toks[nt].op = '>';
            toks[nt].is_dand = 1;
            toks[nt].text[0] = '>';
            toks[nt].text[1] = '>';
            toks[nt].text[2] = '\0';
            nt++;
            p += 2;
            continue;
        }
        if (p[0] == '<' && p[1] == '<' && p[2] == '<') {
            toks[nt].is_op = 1;
            toks[nt].op = '<';
            toks[nt].is_herestr = 1;
            strcpy(toks[nt].text, "<<<");
            nt++;
            p += 3;
            continue;
        }
        if (p[0] == '<' && p[1] == '<') {
            toks[nt].is_op = 1;
            toks[nt].op = '<';
            toks[nt].is_dlt = 1;
            strcpy(toks[nt].text, "<<");
            nt++;
            p += 2;
            continue;
        }
        if (p[0] == '2' && p[1] == '>') {
            toks[nt].is_op = 1;
            toks[nt].op = '>';
            toks[nt].is_fd2 = 1;
            strcpy(toks[nt].text, "2>");
            nt++;
            p += 2;
            continue;
        }
        if (*p == '|' || *p == ';' || *p == '&' || *p == '(' || *p == ')' || *p == '<' ||
            *p == '>') {
            toks[nt].is_op = 1;
            toks[nt].op = *p;
            toks[nt].text[0] = *p;
            toks[nt].text[1] = '\0';
            nt++;
            p++;
            continue;
        }
        /* word (with quotes) */
        int qi = 0;
        int has_quote = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != ';' && *p != '&' && *p != '<' &&
               *p != '>') {
            /* Group $(...) and $((...)) as a single word: when a '$' is
             * immediately followed by a paren, keep consuming until the matching
             * close paren(s) so expansion sees the whole construct. This applies
             * anywhere a '$' appears (e.g. mid-word in "BAR=$((...))"). */
            if (*p == '$' && p[1] == '(') {
                if (qi < 1023)
                    toks[nt].text[qi++] = *p++; /* the '$' */
                int depth = 0;
                do {
                    if (qi < 1023)
                        toks[nt].text[qi++] = *p;
                    if (*p == '(')
                        depth++;
                    else if (*p == ')')
                        depth--;
                    p++;
                } while (*p && depth > 0);
                if (*p == '\0' || *p == ' ' || *p == '\t' || *p == '|' || *p == ';' || *p == '&' ||
                    *p == '<' || *p == '>')
                    break;
                continue;
            }
            /* Outside of the $(...) grouping, standalone '(' ')' are word
             * characters too (e.g. subshell / grouping) — they are only split
             * when not part of a $() construct. */
            if (*p == '(' || *p == ')') {
                if (qi < 1023)
                    toks[nt].text[qi++] = *p;
                p++;
                continue;
            }
            if (*p == '\'' || *p == '"') {
                char q = *p++;
                has_quote = 1;
                while (*p && *p != q) {
                    if (qi < 1023)
                        toks[nt].text[qi++] = *p;
                    p++;
                }
                if (*p == q)
                    p++;
            } else if (*p == '\\') {
                p++;
                if (*p) {
                    if (qi < 1023)
                        toks[nt].text[qi++] = *p;
                    p++;
                }
            } else {
                if (qi < 1023)
                    toks[nt].text[qi++] = *p;
                p++;
            }
        }
        toks[nt].text[qi] = '\0';
        toks[nt].is_op = 0;
        toks[nt].has_quote_flag = has_quote;
        (void)qi;
        nt++;
    }
    return nt;
}

/* ── Variable / parameter / arithmetic expansion ─────────────── */
/* Expand a single word: $VAR ${VAR} ${VAR:-x} ${VAR:=x} ${VAR:+x}
 * ${VAR#p} ${VAR##p} ${VAR%s} ${VAR%%s} ${#VAR} ${ARR[@]} ${ARR[i]}
 * $((...))  $?  (command substitution $() best-effort NOT implemented for safety)
 * Returns malloc'd string or NULL on nounset failure. */

static char *do_param_expand(const char *name, const char *ops, const char *arg) {
    /* ops: "-", "=", "+", "#", "##", "%", "%%", or "" (plain), "#" prefix for length */
    char buf[4096];
    buf[0] = '\0';
    char *val = sh_getenv(name);
    struct shell_array *a = array_find(name);
    int is_len = 0;
    const char *real_ops = ops;
    if (ops[0] == '#' && (ops[1] == '\0' || ops[1] == '[')) {
        is_len = 1;
        real_ops = ops + 1;
    }

    if (a) {
        if (strstr(name, "[@]") || strstr(name, "[*]")) {
            if (is_len) {
                snprintf(buf, sizeof buf, "%d", a->count);
            } else {
                int pos = 0;
                for (int i = 0; i < a->count; i++) {
                    if (i)
                        pos += snprintf(buf + pos, sizeof buf - pos, " ");
                    pos += snprintf(buf + pos, sizeof buf - pos, "%s", a->elems[i]);
                }
            }
            return strdup(buf);
        }
        /* name[i] */
        char an[32];
        strncpy(an, name, 31);
        an[31] = '\0';
        char *br = strchr(an, '[');
        if (br) {
            *br = '\0';
            int idx = atoi(br + 1);
            if (idx >= 0 && idx < a->count) {
                if (is_len)
                    snprintf(buf, sizeof buf, "%d", (int)strlen(a->elems[idx]));
                else
                    snprintf(buf, sizeof buf, "%s", a->elems[idx]);
            } else
                val = 0;
        }
        if (buf[0])
            return strdup(buf);
    }

    if (is_len) {
        snprintf(buf, sizeof buf, "%d", val ? (int)strlen(val) : 0);
        return strdup(buf);
    }

    if (real_ops[0] == '\0') {
        return val ? strdup(val) : strdup("");
    }
    if (real_ops[0] == '-') {
        if (val && *val)
            return strdup(val);
        return strdup(arg ? arg : "");
    }
    if (real_ops[0] == '=') {
        if (!val || !*val) {
            if (arg)
                sh_setenv(name, arg);
            return strdup(arg ? arg : "");
        }
        return strdup(val);
    }
    if (real_ops[0] == '+') {
        return (val && *val) ? strdup(arg ? arg : "") : strdup("");
    }
    if (real_ops[0] == '#' || real_ops[0] == '%') {
        if (!val)
            return strdup("");
        char tmp[4096];
        strncpy(tmp, val, 4095);
        tmp[4095] = '\0';
        int dbl = (real_ops[1] == real_ops[0]);
        const char *pat = arg;
        if (real_ops[0] == '#') {
            char *m = dbl ? strstr(tmp, pat)
                          : (strncmp(tmp, pat, strlen(pat)) == 0 ? tmp + strlen(pat) : 0);
            if (m && (dbl ? m == strstr(tmp, pat) : 1)) {
                if (dbl) { /* longest prefix */
                    char *best = 0;
                    int pl = strlen(pat);
                    for (int i = strlen(tmp); i >= 0; i--)
                        if (i >= pl && strncmp(tmp + i - pl, pat, pl) == 0) {
                            best = tmp + i;
                            break;
                        }
                    if (best)
                        return strdup(best);
                } else
                    return strdup(m);
            }
        } else {
            /* suffix */
            int pl = strlen(pat);
            if (pl == 0)
                return strdup(tmp);
            if (dbl) {
                char *best = 0;
                for (int i = 0; i + pl <= (int)strlen(tmp); i++)
                    if (strncmp(tmp + i, pat, pl) == 0)
                        best = tmp + i;
                if (best) {
                    *best = '\0';
                    return strdup(tmp);
                }
            } else {
                int tl = strlen(tmp);
                if (tl >= pl && strncmp(tmp + tl - pl, pat, pl) == 0) {
                    tmp[tl - pl] = '\0';
                    return strdup(tmp);
                }
            }
        }
        return strdup(tmp);
    }
    return val ? strdup(val) : strdup("");
}

/* ── Arithmetic $((...)) ─────────────────────────────────────── */
/* Simple recursive-descent evaluator. Supports + - * / % & | ^ ~ ! < > <= >=
 * == != && || ( ) unary- and postfix ++ -- and assignment =. */
static long arith_eval(const char *s, const char **end, int *ok);

static long arith_primary(const char *s, const char **end, int *ok) {
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '(') {
        const char *e;
        long v = arith_eval(s + 1, &e, ok);
        if (*e == ')')
            e++;
        *end = e;
        return v;
    }
    if (*s == '-' || *s == '+' || *s == '!' || *s == '~') {
        char op = *s++;
        long v = arith_primary(s, end, ok);
        if (op == '-')
            return -v;
        if (op == '!')
            return !v;
        if (op == '~')
            return ~v;
        return v;
    }
    /* number or variable or bare identifier */
    if ((*s >= '0' && *s <= '9') || *s == '$' || (*s >= 'A' && *s <= 'Z') ||
        (*s >= 'a' && *s <= 'z') || *s == '_') {
        int isnum = (*s >= '0' && *s <= '9');
        char name[64];
        int ni = 0;
        const char *sp = s;
        if (*sp == '$') {
            sp++;
            if (*sp == '(') {
                sp++;
                while (*sp && *sp != ')')
                    name[ni++] = *sp++;
                sp++;
            } else {
                while ((*sp >= 'A' && *sp <= 'Z') || (*sp >= 'a' && *sp <= 'z') ||
                       (*sp >= '0' && *sp <= '9') || *sp == '_')
                    name[ni++] = *sp++;
            }
        } else {
            while ((*sp >= 'A' && *sp <= 'Z') || (*sp >= 'a' && *sp <= 'z') ||
                   (*sp >= '0' && *sp <= '9') || *sp == '_')
                name[ni++] = *sp++;
        }
        name[ni] = '\0';
        if (isnum) {
            long v = atol(s);
            *end = sp;
            return v;
        } else {
            char *ev = sh_getenv(name);
            long v = ev ? atol(ev) : 0;
            *end = sp;
            return v;
        }
    }
    *ok = 0;
    *end = s;
    return 0;
}

static long arith_term(const char *s, const char **end, int *ok) {
    long v = arith_primary(s, end, ok);
    while (**end == ' ' || **end == '\t')
        *end = *end + 1;
    while (**end == '*' || **end == '/' || **end == '%') {
        char op = **end;
        const char *e;
        *end = *end + 1;
        while (**end == ' ' || **end == '\t')
            *end = *end + 1;
        long r = arith_primary(*end, &e, ok);
        *end = e;
        if (op == '*')
            v *= r;
        else if (op == '/')
            v = r ? v / r : 0;
        else
            v = r ? v % r : 0;
        while (**end == ' ' || **end == '\t')
            *end = *end + 1;
    }
    return v;
}
static long arith_expr(const char *s, const char **end, int *ok) {
    long v = arith_term(s, end, ok);
    while (**end == ' ' || **end == '\t')
        *end = *end + 1;
    while (**end == '+' || **end == '-') {
        char op = **end;
        const char *e;
        *end = *end + 1;
        while (**end == ' ' || **end == '\t')
            *end = *end + 1;
        long r = arith_term(*end, &e, ok);
        *end = e;
        if (op == '+')
            v += r;
        else
            v -= r;
        while (**end == ' ' || **end == '\t')
            *end = *end + 1;
    }
    return v;
}
static long arith_eval(const char *s, const char **end, int *ok) {
    /* handle assignment at top level: VAR = expr */
    char name[64];
    int ni = 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *start = p;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
           *p == '_')
        name[ni++] = *p++;
    name[ni] = '\0';
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '=' && start != s) {
        const char *e;
        long r = arith_eval(p + 1, &e, ok);
        sh_setenv(name, itoa_buf(r));
        *end = e;
        return r;
    }
    *end = start;
    long v = arith_expr(s, end, ok);
    while (**end == '<' || **end == '>') {
        char op = **end;
        int iseq = 0;
        if ((*end)[1] == '=')
            iseq = 1;
        const char *e;
        *end += iseq ? 2 : 1;
        long r = arith_expr(*end, &e, ok);
        *end = e;
        if (op == '<')
            v = iseq ? (v <= r) : (v < r);
        else
            v = iseq ? (v >= r) : (v > r);
    }
    while (**end == '=' && (*end)[1] == '=') {
        const char *e;
        *end += 2;
        long r = arith_expr(*end, &e, ok);
        *end = e;
        v = (v == r);
    }
    while (**end == '!' && (*end)[1] == '=') {
        const char *e;
        *end += 2;
        long r = arith_expr(*end, &e, ok);
        *end = e;
        v = (v != r);
    }
    while (**end == '&' && (*end)[1] == '&') {
        const char *e;
        *end += 2;
        long r = arith_expr(*end, &e, ok);
        *end = e;
        v = (v && r);
    }
    while (**end == '|' && (*end)[1] == '|') {
        const char *e;
        *end += 2;
        long r = arith_expr(*end, &e, ok);
        *end = e;
        v = (v || r);
    }
    return v;
}

/* itoa helper (re-entrant via static buffer) */
static char *itoa_buf(long v) {
    static char b[32];
    snprintf(b, sizeof b, "%ld", v);
    return b;
}

/* Expand a word. Returns malloc'd string. *fail set on nounset error. */
static char *expand_word(const char *s, int *fail) {
    char *out = malloc(MAX_LINE * 2);
    if (!out) {
        *fail = 1;
        return 0;
    }
    out[0] = '\0';
    const char *p = s;
    while (*p) {
        if (*p == '$') {
            if (p[1] == '?') {
                strcat(out, itoa_buf(last_exit_code));
                p += 2;
                continue;
            }
            if (p[1] == '$') {
                strcat(out, itoa_buf(getpid_impl()));
                p += 2;
                continue;
            }
            if (p[1] == '(' && p[2] == '(') {
                /* arithmetic */
                const char *e = p + 2;
                const char *aend;
                int ok = 1;
                long v = arith_eval(e, &aend, &ok);
                if (aend[0] == ')')
                    aend++;
                strcat(out, itoa_buf(v));
                p = aend;
                continue;
            }
            if (p[1] == '{') {
                const char *end = p + 2;
                while (*end && *end != '}')
                    end++;
                char expr[256];
                int el = 0;
                for (const char *q = p + 2; q < end && el < 255; q++)
                    expr[el++] = *q;
                expr[el] = '\0';
                /* split name / ops / arg */
                char name[128];
                char ops[8];
                char arg[256];
                name[0] = ops[0] = arg[0] = '\0';
                /* ${#VAR} / ${#ARR[@]} / ${#ARR[i]} — length prefix */
                if (expr[0] == '#') {
                    ops[0] = '#';
                    ops[1] = '\0';
                    /* name is everything after the leading '#' (may include
                     * an array index like [#i] / [#*]); do_param_expand handles
                     * the index + length combination. */
                    int ni = 0;
                    for (int k = 1; expr[k] && expr[k] != '}' && ni < 127; k++)
                        name[ni++] = expr[k];
                    name[ni] = '\0';
                    /* No further ops/arg parsing for the length form. */
                    char *val = do_param_expand(name, ops, 0);
                    if (val) {
                        strcat(out, val);
                        free(val);
                    }
                    p = end + 1;
                    continue;
                }
                /* detect ops at first of :- := :+ # ## % %% */
                int oi = 0;
                int i = 0;
                while (expr[i] && expr[i] != ':' && expr[i] != '#' && expr[i] != '%' &&
                       expr[i] != '[' && expr[i] != '}') {
                    if (expr[i] == '[' && expr[i + 1] == '*' && expr[i + 2] == ']')
                        break;
                    if (expr[i] == '[' && expr[i + 1] == '@' && expr[i + 2] == ']')
                        break;
                    if (expr[i] == '[' && expr[i + 1] >= '0' && expr[i + 1] <= '9')
                        break;
                    name[oi++] = expr[i++];
                }
                name[oi] = '\0';
                /* array index like name[idx] or name[@] */
                if (expr[i] == '[') {
                    char an[256];
                    int ai = 0;
                    while (expr[i] && expr[i] != ']' && ai < 255)
                        an[ai++] = expr[i++];
                    if (expr[i] == ']') {
                        an[ai++] = expr[i++];
                    }
                    an[ai] = '\0';
                    char full[384];
                    snprintf(full, sizeof full, "%s%s", name, an);
                    char *val = do_param_expand(full, "", 0);
                    if (val) {
                        strcat(out, val);
                        free(val);
                    }
                    p = end + 1;
                    continue;
                }
                if (expr[i] == ':' &&
                    (expr[i + 1] == '-' || expr[i + 1] == '=' || expr[i + 1] == '+')) {
                    ops[0] = expr[i + 1];
                    ops[1] = '\0';
                    int j = i + 2;
                    int al = 0;
                    while (expr[j] && expr[j] != '}')
                        arg[al++] = expr[j++];
                    arg[al] = '\0';
                } else if (expr[i] == '#' || expr[i] == '%') {
                    ops[0] = expr[i];
                    if (expr[i + 1] == expr[i]) {
                        ops[1] = expr[i];
                        ops[2] = '\0';
                        i++;
                    } else
                        ops[1] = '\0';
                    int j = i + 1 + (ops[1] ? 1 : 0);
                    int al = 0;
                    while (expr[j] && expr[j] != '}')
                        arg[al++] = expr[j++];
                    arg[al] = '\0';
                    /* re-derive name up to ops */
                    oi = 0;
                    for (int k = 0; expr[k] && expr[k] != '#' && expr[k] != '%'; k++)
                        name[oi++] = expr[k];
                    name[oi] = '\0';
                } else {
                    /* plain ${name} */
                    int j = i;
                    int al = 0;
                    while (expr[j] && expr[j] != '}')
                        arg[al++] = expr[j++];
                    arg[al] = '\0';
                    /* name already in name */
                }
                char *val = do_param_expand(name, ops, arg[0] ? arg : 0);
                if (val) {
                    strcat(out, val);
                    free(val);
                }
                p = end + 1;
                continue;
            }
            /* bare $VAR */
            if ((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z') || p[1] == '_') {
                char name[64];
                int ni = 0;
                p++;
                while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                       (*p >= '0' && *p <= '9') || *p == '_')
                    name[ni++] = *p++;
                name[ni] = '\0';
                char *val = sh_getenv(name);
                if (!val) {
                    if (opt_nounset) {
                        *fail = 1;
                        free(out);
                        return 0;
                    }
                    val = "";
                }
                strcat(out, val);
                continue;
            }
            /* lone $ — keep */
            strcat(out, "$");
            p++;
            continue;
        }
        int ol = strlen(out);
        out[ol] = *p++;
        out[ol + 1] = '\0';
    }
    return out;
}

/* ── Filename globbing ───────────────────────────────────────── */
/* Expand a single word containing *, ?, [..] into matches. Returns count,
 * fills `out` with malloc'd strings (caller frees). If no match, returns the
 * literal word (fallback). */
static int glob_word(const char *pat, char **out, int maxout) {
    /* If no glob metachar, return literal */
    if (!strpbrk(pat, "*?[")) {
        out[0] = strdup(pat);
        return 1;
    }
    /* Determine directory prefix */
    char dir[PATH_MAX];
    char base[PATH_MAX];
    const char *slash = strrchr(pat, '/');
    if (slash) {
        int n = slash - pat;
        if (n >= PATH_MAX)
            n = PATH_MAX - 1;
        memcpy(dir, pat, n);
        dir[n] = '\0';
        strncpy(base, slash + 1, PATH_MAX - 1);
        base[PATH_MAX - 1] = '\0';
    } else {
        dir[0] = '.';
        dir[1] = '\0';
        strncpy(base, pat, PATH_MAX - 1);
        base[PATH_MAX - 1] = '\0';
    }
    int fd = open(dir, O_RDONLY, 0);
    int n = 0;
    if (fd >= 0) {
        char buf[4096];
        int r = getdents64(fd, buf, sizeof buf);
        close(fd);
        if (r > 0) {
            int pos = 0;
            while (pos < r && n < maxout) {
                struct dirent *d = (struct dirent *)(buf + pos);
                if (fnmatch_simple(base, d->d_name)) {
                    char full[PATH_MAX];
                    if (slash)
                        snprintf(full, sizeof full, "%s/%s", dir, d->d_name);
                    else
                        snprintf(full, sizeof full, "%s", d->d_name);
                    out[n++] = strdup(full);
                }
                pos += d->d_reclen;
            }
        }
    }
    if (n == 0) {
        out[0] = strdup(pat);
        return 1;
    }
    return n;
}

static int fnmatch_simple(const char *pat, const char *str) {
    /* minimal glob: * ? and [a-z] */
    const char *p = pat, *s = str;
    while (*p) {
        if (*p == '*') {
            if (!p[1])
                return 1;
            for (; *s; s++)
                if (fnmatch_simple(p + 1, s))
                    return 1;
            return 0;
        }
        if (*p == '?') {
            if (!*s)
                return 0;
            p++;
            s++;
            continue;
        }
        if (*p == '[') {
            const char *e = p + 1;
            int matched = 0;
            while (*e && *e != ']')
                e++;
            if (!*e) {
                if (*p != *s)
                    return 0;
                p++;
                s++;
                continue;
            }
            for (const char *c = p + 1; c < e; c++) {
                if (c[1] == '-' && c + 2 < e) {
                    char lo = *c, hi = c[2];
                    if (*s >= lo && *s <= hi)
                        matched = 1;
                    c += 2;
                } else if (*c == *s)
                    matched = 1;
            }
            if (!matched)
                return 0;
            p = e + 1;
            s++;
            continue;
        }
        if (*p != *s)
            return 0;
        p++;
        s++;
    }
    return *p == '\0' && *s == '\0';
}

/* ── Redirections ────────────────────────────────────────────── */
static int apply_redirs(struct redir *r, int nr) {
    int saved[3] = {-1, -1, -1};
    for (int i = 0; i < nr; i++) {
        int fd = 1;
        if (r[i].type == 0)
            fd = 0; /* < */
        else if (r[i].type == 3)
            fd = 2; /* 2> */
        else if (r[i].type == 6)
            fd = 2; /* &> -> stderr too (handled below) */
        if (fd < 3 && saved[fd] < 0)
            saved[fd] = dup(fd);
        int mode = O_WRONLY | O_CREAT;
        if (r[i].type == 2)
            mode |= O_APPEND;
        if (r[i].type == 1)
            mode |= O_TRUNC;
        if (r[i].type == 0)
            mode = O_RDONLY;
        int f = open(r[i].target, mode, 0644);
        if (f < 0) {
            write(STDERR_FILENO, "sh: ", 4);
            write(STDERR_FILENO, r[i].target, strlen(r[i].target));
            write(STDERR_FILENO, ": cannot open\n", 14);
            return -1;
        }
        dup2(f, fd);
        close(f);
        if (r[i].type == 6) { /* &> redirect stderr to same file */
            int f2 = open(r[i].target, mode, 0644);
            if (f2 >= 0) {
                dup2(f2, 2);
                close(f2);
            }
        }
    }
    return 0;
}

/* ── External command execution ──────────────────────────────── */
static char **build_env(void) {
    static char *envp[MAX_ENV + 1];
    for (int i = 0; i < sh_env_count && sh_env[i]; i++)
        envp[i] = sh_env[i];
    envp[sh_env_count] = 0;
    return envp;
}

static int sh_exec_ext(char **argv) {
    char full[PATH_MAX];
    int pid;
    if (argv[0][0] == '/' || argv[0][0] == '.') {
        pid = fork();
        if (pid == 0) {
            execve(argv[0], argv, build_env());
            printf("sh: %s: not found\n", argv[0]);
            exit(127);
        }
        return pid;
    }
    char *path = sh_getenv("PATH");
    if (!path)
        path = "/bin";
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX);
    path_copy[PATH_MAX - 1] = '\0';
    char *dir = path_copy;
    while (dir) {
        char *next = sh_strchr(dir, ':');
        if (next)
            *next++ = '\0';
        unsigned long dlen = strlen(dir);
        unsigned long nlen = strlen(argv[0]);
        if (dlen + 1 + nlen >= PATH_MAX) {
            dir = next;
            continue;
        }
        unsigned long pos = 0;
        while (dir[pos]) {
            full[pos] = dir[pos];
            pos++;
        }
        full[pos++] = '/';
        unsigned long j = 0;
        while (argv[0][j]) {
            full[pos] = argv[0][j];
            pos++;
            j++;
        }
        full[pos] = '\0';
        pid = fork();
        if (pid == 0) {
            execve(full, argv, build_env());
            exit(127);
        }
        if (pid > 0)
            return pid;
        dir = next;
    }
    return -1;
}

/* ── Built-ins (existing + new) ──────────────────────────────── */
static int cmd_which(char **argv);
static int cmd_ps(void);
static int cmd_free(void);
static int cmd_uptime(void);

/* Shared builtin-name table: used by `which` and Tab-completion
 * (D280 task 18).  Keep in sync with run_builtin(). */
static const char *sh_builtins[] = {
    "cd",       "pwd",    "exit", "help",   "echo",    "clear", "exec",     "export",
    "unset",    "local",  "set",  "alias",  "unalias", "jobs",  "fg",       "bg",
    "kill",     "source", ".",    "trap",   "read",    "which", "ps",       "free",
    "uptime",   "uname",  "time", "test",   "[",       "true",  "false",    "break",
    "continue", "return", "type", "ulimit", "umask",   "times", "readonly", 0};
static int cmd_uname(int argc, char **argv);
static int cmd_time(int argc, char **argv);
static int cmd_help(void);
static int cmd_type(int argc, char **argv);
static int cmd_ulimit(int argc, char **argv);
static int cmd_umask(int argc, char **argv);
static int cmd_times(int argc, char **argv);
static int cmd_readonly(int argc, char **argv);
static int cmd_test(int argc, char **argv);

static int run_builtin(int argc, char **argv) {
    const char *cmd = argv[0];
    if (strcmp(cmd, "exit") == 0) {
        int code = (argc > 1) ? atoi(argv[1]) : last_exit_code;
        exit(code);
        return 0;
    }
    if (strcmp(cmd, "return") == 0) {
        g_return_code = (argc > 1) ? atoi(argv[1]) : last_exit_code;
        g_did_return = 1;
        return g_return_code;
    }
    if (strcmp(cmd, "cd") == 0) {
        const char *path = argc > 1 ? argv[1] : sh_getenv("HOME");
        if (!path)
            path = "/";
        if (chdir(path) < 0) {
            printf("cd: %s: No such directory\n", argv[1] ? argv[1] : "");
            return 1;
        }
        return 0;
    }
    if (strcmp(cmd, "pwd") == 0) {
        char buf[PATH_MAX];
        if (getcwd(buf, PATH_MAX) == 0) {
            printf("pwd: error\n");
            return 1;
        }
        printf("%s\n", buf);
        return 0;
    }
    if (strcmp(cmd, "echo") == 0) {
        int nl = 1;
        int s = 1;
        if (argc > 1 && strcmp(argv[1], "-n") == 0) {
            nl = 0;
            s = 2;
        }
        for (int i = s; i < argc; i++) {
            if (i > s)
                write(1, " ", 1);
            /* interpret \n \t in echo */
            const char *t = argv[i];
            char ob[4096];
            int oi = 0;
            while (*t && oi < 4095) {
                if (*t == '\\' && t[1]) {
                    t++;
                    char c = *t;
                    if (c == 'n')
                        ob[oi++] = '\n';
                    else if (c == 't')
                        ob[oi++] = '\t';
                    else if (c == 'r')
                        ob[oi++] = '\r';
                    else
                        ob[oi++] = c;
                    t++;
                } else
                    ob[oi++] = *t++;
            }
            ob[oi] = '\0';
            write(1, ob, oi);
        }
        if (nl)
            write(1, "\n", 1);
        return 0;
    }
    if (strcmp(cmd, "clear") == 0) {
        write(1, "\033[2J\033[H", 7);
        return 0;
    }
    if (strcmp(cmd, "help") == 0) {
        return cmd_help();
    }
    if (strcmp(cmd, "exec") == 0) {
        if (argc < 2) {
            printf("exec: missing argument\n");
            return 1;
        }
        execve(argv[1], argv + 1, build_env());
        printf("exec: %s: not found\n", argv[1]);
        return 1;
    }
    if (strcmp(cmd, "export") == 0) {
        if (argc < 2) {
            for (int i = 0; i < sh_env_count && sh_env[i]; i++)
                printf("%s\n", sh_env[i]);
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) { /* mark exported: already in env, noop */
                continue;
            }
            *eq = '\0';
            sh_setenv(argv[i], eq + 1);
            *eq = '=';
        }
        return 0;
    }
    if (strcmp(cmd, "unset") == 0) {
        for (int i = 1; i < argc; i++)
            sh_unsetenv(argv[i]);
        return 0;
    }
    if (strcmp(cmd, "local") == 0) {
        /* local VAR=val — within function context; we treat as env set with a
         * marker (simplified: just setenv). */
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq)
                continue;
            *eq = '\0';
            sh_setenv(argv[i], eq + 1);
            *eq = '=';
        }
        return 0;
    }
    if (strcmp(cmd, "set") == 0) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-e") == 0)
                opt_errexit = 1;
            else if (strcmp(argv[i], "+e") == 0)
                opt_errexit = 0;
            else if (strcmp(argv[i], "-u") == 0)
                opt_nounset = 1;
            else if (strcmp(argv[i], "+u") == 0)
                opt_nounset = 0;
            else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                if (strcmp(argv[++i], "pipefail") == 0)
                    opt_pipefail = 1;
            } else if (strcmp(argv[i], "+o") == 0 && i + 1 < argc) {
                if (strcmp(argv[++i], "pipefail") == 0)
                    opt_pipefail = 0;
            } else if (strcmp(argv[i], "-x") == 0)
                opt_xtrace = 1;
            else if (strcmp(argv[i], "+x") == 0)
                opt_xtrace = 0;
        }
        return 0;
    }
    if (strcmp(cmd, "alias") == 0) {
        if (argc < 2) {
            for (int i = 0; i < sh_alias_count; i++)
                printf("alias %s='%s'\n", shell_aliases[i].name, shell_aliases[i].value);
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) { /* print one */
                continue;
            }
            *eq = '\0';
            const char *val = eq + 1;
            if (val[0] == '\'' || val[0] == '"')
                val++;
            int vl = strlen(val);
            if (vl && (val[vl - 1] == '\'' || val[vl - 1] == '"'))
                vl--;
            char vbuf[256];
            memcpy(vbuf, val, vl);
            vbuf[vl] = '\0';
            int found = 0;
            for (int j = 0; j < sh_alias_count; j++)
                if (strcmp(shell_aliases[j].name, argv[i]) == 0) {
                    strncpy(shell_aliases[j].value, vbuf, 255);
                    found = 1;
                }
            if (!found && sh_alias_count < MAX_ALIASES) {
                strncpy(shell_aliases[sh_alias_count].name, argv[i], 63);
                strncpy(shell_aliases[sh_alias_count].value, vbuf, 255);
                sh_alias_count++;
            }
            *eq = '=';
        }
        return 0;
    }
    if (strcmp(cmd, "unalias") == 0) {
        for (int i = 1; i < argc; i++)
            for (int j = 0; j < sh_alias_count; j++)
                if (strcmp(shell_aliases[j].name, argv[i]) == 0) {
                    for (int k = j; k < sh_alias_count - 1; k++)
                        shell_aliases[k] = shell_aliases[k + 1];
                    sh_alias_count--;
                    j--;
                    break;
                }
        return 0;
    }
    if (strcmp(cmd, "jobs") == 0) {
        job_reap(WNOHANG);
        for (int i = 0; i < sh_job_count; i++) {
            if (shell_jobs[i].state != 1)
                printf("[%d] %d %s\t%s\n", i + 1, shell_jobs[i].pid,
                       shell_jobs[i].state == 2 ? "Stopped" : "Running", shell_jobs[i].cmd);
        }
        return 0;
    }
    if (strcmp(cmd, "fg") == 0) {
        int idx = 0;
        if (argc > 1)
            idx = atoi(argv[1]);
        else
            for (int i = 0; i < sh_job_count; i++)
                if (shell_jobs[i].state != 1) {
                    idx = i + 1;
                    break;
                }
        if (idx < 1 || idx > sh_job_count) {
            printf("fg: no such job\n");
            return 1;
        }
        int j = idx - 1;
        kill(shell_jobs[j].pid, SIGCONT);
        int st = 0;
        while (1) {
            int r = waitpid(shell_jobs[j].pid, &st, 0);
            if (r > 0 || r < 0)
                break;
        }
        shell_jobs[j].state = 1;
        last_exit_code =
            WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 0);
        return last_exit_code;
    }
    if (strcmp(cmd, "bg") == 0) {
        int idx = 0;
        if (argc > 1)
            idx = atoi(argv[1]);
        else
            for (int i = 0; i < sh_job_count; i++)
                if (shell_jobs[i].state != 1) {
                    idx = i + 1;
                    break;
                }
        if (idx < 1 || idx > sh_job_count) {
            printf("bg: no such job\n");
            return 1;
        }
        kill(shell_jobs[idx - 1].pid, SIGCONT);
        shell_jobs[idx - 1].state = 0;
        printf("[%d] %d continued\n", idx, shell_jobs[idx - 1].pid);
        return 0;
    }
    if (strcmp(cmd, "kill") == 0) {
        for (int i = 1; i < argc; i++) {
            int sig = SIGTERM;
            const char *t = argv[i];
            if (t[0] == '-' && t[1] >= '0' && t[1] <= '9') {
                sig = atoi(t);
                t += 2;
            }
            int pid = atoi(t);
            kill(pid, sig);
        }
        return 0;
    }
    if (strcmp(cmd, "source") == 0 || strcmp(cmd, ".") == 0) {
        if (argc < 2) {
            printf("source: missing file\n");
            return 1;
        }
        return source_file(argv[1]);
    }
    if (strcmp(cmd, "trap") == 0) {
        if (argc < 3) {
            for (int i = 0; i < sh_trap_count; i++)
                printf("trap -- '%s' %d\n", shell_traps[i].action, shell_traps[i].sig);
            return 0;
        }
        const char *action = argv[1];
        int sig = atoi(argv[2]);
        for (int i = 0; i < sh_trap_count; i++)
            if (shell_traps[i].sig == sig) {
                strncpy(shell_traps[i].action, action, 255);
                return 0;
            }
        if (sh_trap_count < MAX_TRAPS) {
            shell_traps[sh_trap_count].sig = sig;
            strncpy(shell_traps[sh_trap_count].action, action, 255);
            sh_trap_count++;
        }
        return 0;
    }
    if (strcmp(cmd, "read") == 0) {
        if (argc < 2)
            return 1;
        char buf[1024];
        int n = read(0, buf, sizeof buf - 1);
        if (n <= 0)
            return 1;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            n--;
        buf[n] = '\0';
        sh_setenv(argv[1], buf);
        return 0;
    }
    if (strcmp(cmd, "which") == 0)
        return cmd_which(argv);
    if (strcmp(cmd, "type") == 0)
        return cmd_type(argc, argv);
    if (strcmp(cmd, "ulimit") == 0)
        return cmd_ulimit(argc, argv);
    if (strcmp(cmd, "umask") == 0)
        return cmd_umask(argc, argv);
    if (strcmp(cmd, "times") == 0)
        return cmd_times(argc, argv);
    if (strcmp(cmd, "readonly") == 0)
        return cmd_readonly(argc, argv);
    if (strcmp(cmd, "ps") == 0)
        return cmd_ps();
    if (strcmp(cmd, "free") == 0)
        return cmd_free();
    if (strcmp(cmd, "uptime") == 0)
        return cmd_uptime();
    if (strcmp(cmd, "uname") == 0)
        return cmd_uname(argc, argv);
    if (strcmp(cmd, "time") == 0)
        return cmd_time(argc, argv);
    if (strcmp(cmd, "test") == 0 || strcmp(cmd, "[") == 0)
        return cmd_test(argc, argv);
    if (strcmp(cmd, "true") == 0)
        return 0;
    if (strcmp(cmd, "false") == 0)
        return 1;
    if (strcmp(cmd, "break") == 0) {
        g_loop_ctl = 1;
        return 0;
    }
    if (strcmp(cmd, "continue") == 0) {
        g_loop_ctl = 2;
        return 0;
    }
    return -1;
}


static int source_file(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("sh: %s: No such file\n", path);
        return 1;
    }
    char buf[8192];
    int total = 0;
    int r;
    while ((r = read(fd, buf + total, sizeof buf - 1 - total)) > 0)
        total += r;
    close(fd);
    buf[total] = '\0';
    /* execute line by line */
    char *line = buf;
    int save = last_exit_code;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char *l = line;
        while (*l == ' ' || *l == '\t')
            l++;
        if (*l && strncmp(l, "#", 1) != 0)
            execute_line_raw(l);
        if (!nl)
            break;
        line = nl + 1;
    }
    return last_exit_code = save, last_exit_code;
}

/* ── test / [ built-in ───────────────────────────────────────── */
static int cmd_test(int argc, char **argv) {
    /* argv[0] = "test" or "["; drop trailing "]" */
    int last = argc - 1;
    if (strcmp(argv[0], "[") == 0 && last >= 1 && strcmp(argv[last], "]") == 0)
        argc--;
    if (argc <= 1)
        return 1;
    /* rebuild args without argv[0] */
    char **a = argv + 1;
    int n = argc - 1;
    int negate = 0;
    if (n >= 1 && strcmp(a[0], "!") == 0) {
        negate = 1;
        a++;
        n--;
    }
    if (n == 0)
        return negate ? 0 : 1;
    int result;
    if (n == 1) {
        result = (a[0][0] != '\0');
    } else if (n == 2) {
        if (strcmp(a[0], "-f") == 0)
            result = file_exists(a[1], 1);
        else if (strcmp(a[0], "-d") == 0)
            result = file_exists(a[1], 2);
        else if (strcmp(a[0], "-e") == 0)
            result = file_exists(a[1], 0);
        else if (strcmp(a[0], "-z") == 0)
            result = (a[1][0] == '\0');
        else if (strcmp(a[0], "-n") == 0)
            result = (a[1][0] != '\0');
        else if (strcmp(a[0], "-r") == 0)
            result = (access(a[1], 4) == 0);
        else if (strcmp(a[0], "-w") == 0)
            result = (access(a[1], 2) == 0);
        else if (strcmp(a[0], "-x") == 0)
            result = (access(a[1], 1) == 0);
        else
            result = (strcmp(a[0], a[1]) == 0);
    } else {
        /* binary: a[0] OP a[2]  with optional -a/-o chaining */
        result = eval_test_binary(a[1], a[0], a[2]);
        int i = 3;
        while (i < n) {
            if (strcmp(a[i], "-a") == 0 && i + 1 < n) {
                result = result && eval_test_binary(a[i + 2], a[i + 1], a[i + 3]);
                i += 3;
            } else if (strcmp(a[i], "-o") == 0 && i + 1 < n) {
                result = result || eval_test_binary(a[i + 2], a[i + 1], a[i + 3]);
                i += 3;
            } else
                break;
        }
    }
    return negate ? !result : (result ? 0 : 1);
}

static int eval_test_binary(const char *op, const char *l, const char *r) {
    if (strcmp(op, "=") == 0)
        return strcmp(l, r) == 0;
    if (strcmp(op, "!=") == 0)
        return strcmp(l, r) != 0;
    if (strcmp(op, "-eq") == 0)
        return atoi(l) == atoi(r);
    if (strcmp(op, "-ne") == 0)
        return atoi(l) != atoi(r);
    if (strcmp(op, "-lt") == 0)
        return atoi(l) < atoi(r);
    if (strcmp(op, "-gt") == 0)
        return atoi(l) > atoi(r);
    if (strcmp(op, "-le") == 0)
        return atoi(l) <= atoi(r);
    if (strcmp(op, "-ge") == 0)
        return atoi(l) >= atoi(r);
    if (strcmp(op, "-ef") == 0)
        return file_exists(l, 0) && file_exists(r, 0);
    return 0;
}

static int file_exists(const char *path, int type) {
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (type == 1)
        return S_ISREG(st.st_mode);
    if (type == 2)
        return S_ISDIR(st.st_mode);
    return 1;
}

/* ── Parser ──────────────────────────────────────────────────── */
/* Parse a full line into a list AST. Handles compound commands by reading
 * additional lines via the callback `read_more` if needed. We keep it simple:
 * the line is expected to be a complete construct (the interactive loop joins
 * multi-line blocks before calling here). */
static struct ast_node *parse_list(const char *line);

static struct ast_node *new_node(enum node_type t) {
    struct ast_node *n = calloc(1, sizeof(struct ast_node));
    n->type = t;
    return n;
}

/* Parse a single command (no list operators, no pipe) into a COMMAND node. */
static struct ast_node *parse_command(struct token *toks, int nt, int *i) {
    struct ast_node *n = new_node(NODE_COMMAND);
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (t->is_op) {
            if (t->op == '|')
                return n;
            if (t->op == ';' || t->op == '&' || t->op == '(' || t->op == ')')
                return n;
            if (t->is_andand || t->is_oror)
                return n;
            /* redirection */
            int rtype = 0;
            if (t->op == '<') {
                if (t->is_herestr)
                    rtype = 5;
                else if (t->is_dlt)
                    rtype = 4;
                else
                    rtype = 0;
            } else if (t->op == '>') {
                if (t->is_fd2)
                    rtype = 3;
                else if (t->is_dand)
                    rtype = 2;
                else
                    rtype = 1;
            }
            (*i)++;
            if (*i < nt && !toks[*i].is_op) {
                strncpy(n->redirs[n->nredirs].target, toks[*i].text, 255);
                n->redirs[n->nredirs].target[255] = '\0';
                n->redirs[n->nredirs].type = rtype;
                n->nredirs++;
            } else if (t->is_dlt) {
                /* heredoc: read lines until delimiter (best-effort: store marker) */
                /* we don't support multi-line heredoc here; treat as empty */
            }
            (*i)++;
            continue;
        }
        if (t->text[0] == '!') {
            n->invert = 1;
            (*i)++;
            continue;
        }
        /* assignment? */
        char *eq = strchr(t->text, '=');
        if (eq && eq != t->text && t->text[0] != '$' && !strpbrk(t->text, "/\"'") &&
            (eq == t->text || (eq > t->text && (eq[-1] != '\\')))) {
            /* heuristic: NAME=val */
            int is_name = 1;
            for (char *c = t->text; c < eq; c++)
                if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                      (*c >= '0' && *c <= '9') || *c == '_')) {
                    is_name = 0;
                    break;
                }
            if (is_name && n->argc == 0) {
                int nl = eq - t->text;
                n->assigns[n->nassigns][0] = malloc(nl + 1);
                if (n->assigns[n->nassigns][0]) {
                    memcpy(n->assigns[n->nassigns][0], t->text, nl);
                    n->assigns[n->nassigns][0][nl] = '\0';
                }
                n->assigns[n->nassigns][1] = strdup(eq + 1);
                n->nassigns++;
                (*i)++;
                continue;
            }
        }
        if (n->argc < MAX_ARGS - 1)
            n->argv[n->argc++] = strdup(t->text);
        (*i)++;
    }
    n->argv[n->argc] = 0;
    return n;
}

/* Parse a pipeline (cmd | cmd ...) -> returns PIPELINE node. */
static struct ast_node *parse_pipeline(struct token *toks, int nt, int *i) {
    struct ast_node *pl = new_node(NODE_PIPELINE);
    pl->children[pl->nchildren++] = parse_command(toks, nt, i);
    while (*i < nt) {
        if (toks[*i].is_op && toks[*i].op == '|' && !toks[*i].is_oror && !toks[*i].is_andand) {
            (*i)++;
            pl->children[pl->nchildren++] = parse_command(toks, nt, i);
        } else
            break;
    }
    return pl;
}

/* Parse a full line as a list (handles ; && || & and compound commands). */
static struct ast_node *parse_list(const char *line) {
    /* Allocate token array sized to the line (avoid one huge malloc that
     * exceeds the libc bump-heap cap). A token is ~1KB; a line of N chars
     * yields at most (N/2 + 1) tokens. */
    int linelen = 0;
    while (line[linelen])
        linelen++;
    int nmax = (linelen / 2) + 2;
    if (nmax > 256)
        nmax = 256;
    if (nmax < 8)
        nmax = 8;
    struct token *toks = malloc(sizeof(struct token) * nmax);
    if (!toks)
        return 0;
    int nt = tokenize(line, toks, nmax);
    if (nt <= 0) {
        free(toks);
        return 0;
    }
    struct ast_node *list = new_node(NODE_LIST);
    int i = 0;
    while (i < nt) {
        struct token *t = &toks[i];
        if (t->is_op && (t->op == '(')) {
            /* subshell */
            struct ast_node *sub = new_node(NODE_SUBSHELL);
            /* gather tokens until matching ) */
            int depth = 1;
            int j = i + 1;
            int start = j;
            while (j < nt && depth > 0) {
                if (toks[j].is_op && toks[j].op == '(')
                    depth++;
                else if (toks[j].is_op && toks[j].op == ')')
                    depth--;
                if (depth == 0)
                    break;
                j++;
            }
            /* reconstruct inner line */
            char inner[2048];
            inner[0] = '\0';
            for (int k = start; k < j; k++) {
                strcat(inner, toks[k].text);
                strcat(inner, " ");
            }
            sub->body = strdup(inner);
            list->children[list->nchildren++] = sub;
            i = j + 1;
            if (i < nt && toks[i].op == '&' && !toks[i].is_andand) {
                list->children[list->nchildren - 1]->background = 1;
                list->ops[list->nchildren - 1] = '&';
                i++;
            }
            continue;
        }
        /* detect compound keywords */
        if (!t->is_op) {
            /* function definition: NAME ( ) { ... } */
            if (i + 3 < nt && toks[i + 1].is_op && toks[i + 1].op == '(' && toks[i + 2].is_op &&
                toks[i + 2].op == ')' && toks[i + 3].is_op && toks[i + 3].op == '{') {
                list->children[list->nchildren++] = parse_function(toks, nt, &i);
                continue;
            }
            if (strcmp(t->text, "if") == 0) {
                list->children[list->nchildren++] = parse_if(toks, nt, &i);
                continue;
            }
            if (strcmp(t->text, "while") == 0) {
                list->children[list->nchildren++] = parse_while(toks, nt, &i, 0);
                continue;
            }
            if (strcmp(t->text, "until") == 0) {
                list->children[list->nchildren++] = parse_while(toks, nt, &i, 1);
                continue;
            }
            if (strcmp(t->text, "for") == 0) {
                list->children[list->nchildren++] = parse_for(toks, nt, &i);
                continue;
            }
            if (strcmp(t->text, "case") == 0) {
                list->children[list->nchildren++] = parse_case(toks, nt, &i);
                continue;
            }
            if (strcmp(t->text, "function") == 0) {
                list->children[list->nchildren++] = parse_function(toks, nt, &i);
                continue;
            }
            if (strcmp(t->text, "{") == 0) {
                list->children[list->nchildren++] = parse_group(toks, nt, &i);
                continue;
            }
        }
        struct ast_node *pl = parse_pipeline(toks, nt, &i);
        list->children[list->nchildren++] = pl;
        if (i < nt) {
            struct token *op = &toks[i];
            if (op->is_op) {
                if (op->op == ';') {
                    list->ops[list->nchildren - 1] = ';';
                    i++;
                } else if (op->op == '&' && !op->is_andand) {
                    list->children[list->nchildren - 1]->background = 1;
                    list->ops[list->nchildren - 1] = '&';
                    i++;
                } else if (op->is_andand) {
                    list->ops[list->nchildren - 1] = '&';
                    i++;
                } else if (op->is_oror) {
                    list->ops[list->nchildren - 1] = '|';
                    i++;
                } else if (op->op == ')') {
                    i++;
                } else
                    break;
            } else
                break;
        }
    }
    free(toks);
    return list;
}

/* Compound parsers: these expect the block to be on the token stream, possibly
 * spanning multiple lines. Since our interactive loop joins multi-line blocks
 * into one string before parse_list, the tokens contain the whole block. */
static struct ast_node *parse_if(struct token *toks, int nt, int *i) {
    /* consume "if" */
    (*i)++;
    struct ast_node *n = new_node(NODE_IF);
    /* condition until "then" */
    int start = *i;
    int depth = 0;
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (t->is_op && t->op == '(')
            depth++;
        if (t->is_op && t->op == ')')
            depth--;
        if (depth == 0 && !t->is_op && strcmp(t->text, "then") == 0)
            break;
        (*i)++;
    }
    /* build cond line */
    char cond[1024];
    cond[0] = '\0';
    for (int k = start; k < *i; k++) {
        strcat(cond, toks[k].text);
        strcat(cond, " ");
    }
    n->cond = strdup(cond);
    (*i)++; /* skip then */
    /* body until fi / elif / else */
    char body[2048];
    body[0] = '\0';
    char elsebody[2048];
    elsebody[0] = '\0';
    int in_else = 0;
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (!t->is_op && strcmp(t->text, "fi") == 0) {
            (*i)++;
            break;
        }
        if (!t->is_op && strcmp(t->text, "else") == 0) {
            in_else = 1;
            (*i)++;
            continue;
        }
        if (!t->is_op && strcmp(t->text, "elif") == 0) {
            /* treat elif as nested if in else */
            struct ast_node *elif = parse_if(toks, nt, i);
            strcat(elsebody, "if ");
            strcat(elsebody, elif->cond ? elif->cond : "");
            strcat(elsebody, " ; then ");
            strcat(elsebody, elif->body ? elif->body : "");
            if (elif->else_body) {
                strcat(elsebody, " ; else ");
                strcat(elsebody, elif->else_body);
            }
            strcat(elsebody, " ; fi");
            free(elif);
            continue;
        }
        if (in_else)
            strcat(elsebody, t->text), strcat(elsebody, " ");
        else
            strcat(body, t->text), strcat(body, " ");
        (*i)++;
    }
    n->body = strdup(body);
    if (elsebody[0])
        n->else_body = strdup(elsebody);
    return n;
}

static struct ast_node *parse_while(struct token *toks, int nt, int *i, int until) {
    (*i)++;
    struct ast_node *n = new_node(until ? NODE_UNTIL : NODE_WHILE);
    int start = *i;
    int depth = 0;
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (t->is_op && t->op == '(')
            depth++;
        if (t->is_op && t->op == ')')
            depth--;
        if (depth == 0 && !t->is_op && strcmp(t->text, "do") == 0)
            break;
        (*i)++;
    }
    char cond[1024];
    cond[0] = '\0';
    for (int k = start; k < *i; k++) {
        strcat(cond, toks[k].text);
        strcat(cond, " ");
    }
    n->cond = strdup(cond);
    (*i)++; /* do */
    char body[2048];
    body[0] = '\0';
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (!t->is_op && strcmp(t->text, "done") == 0) {
            (*i)++;
            break;
        }
        strcat(body, t->text);
        strcat(body, " ");
        (*i)++;
    }
    n->body = strdup(body);
    return n;
}

static struct ast_node *parse_for(struct token *toks, int nt, int *i) {
    (*i)++;
    struct ast_node *n = new_node(NODE_FOR);
    /* for VAR in w1 w2 ... ; do */
    if (*i < nt && !toks[*i].is_op) {
        strncpy(n->for_var, toks[*i].text, 63);
        n->for_var[63] = '\0';
        (*i)++;
    }
    if (*i < nt && !toks[*i].is_op && strcmp(toks[*i].text, "in") == 0)
        (*i)++;
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (t->is_op)
            break;
        if (!t->is_op && strcmp(t->text, "do") == 0) {
            (*i)++;
            break;
        }
        if (n->for_nwords < 64)
            n->for_words[n->for_nwords++] = strdup(t->text);
        (*i)++;
    }
    char body[2048];
    body[0] = '\0';
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (!t->is_op && strcmp(t->text, "done") == 0) {
            (*i)++;
            break;
        }
        strcat(body, t->text);
        strcat(body, " ");
        (*i)++;
    }
    n->body = strdup(body);
    return n;
}

static struct ast_node *parse_case(struct token *toks, int nt, int *i) {
    (*i)++;
    struct ast_node *n = new_node(NODE_CASE);
    if (*i < nt && !toks[*i].is_op) {
        strncpy(n->case_word, toks[*i].text, 255);
        (*i)++;
    }
    if (*i < nt && !toks[*i].is_op && strcmp(toks[*i].text, "in") == 0)
        (*i)++;
    char curpat[256];
    curpat[0] = '\0';
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (!t->is_op && strcmp(t->text, "esac") == 0) {
            (*i)++;
            break;
        }
        if (!t->is_op && strcmp(t->text, ")") == 0) {
            (*i)++;
            continue;
        }
        if (!t->is_op && strcmp(t->text, ";;") == 0) {
            curpat[0] = '\0';
            (*i)++;
            continue;
        }
        if (t->is_op && t->op == ')') {
            /* start of pattern action */
            if (curpat[0] == '\0')
                strncpy(curpat, t->text, 255);
            (*i)++;
            continue;
        }
        if (curpat[0] == '\0') {
            strncpy(curpat, t->text, 255);
            (*i)++;
            continue;
        }
        /* action word */
        int idx = n->case_n;
        if (idx < 64) {
            n->case_pat[idx] = strdup(curpat);
            n->case_act[idx] = strdup(t->text);
            n->case_n++;
        }
        curpat[0] = '\0';
        (*i)++;
    }
    return n;
}

static struct ast_node *parse_function(struct token *toks, int nt, int *i) {
    struct ast_node *n = new_node(NODE_FUNCTION);
    /* optional 'function' keyword */
    if (*i < nt && !toks[*i].is_op && strcmp(toks[*i].text, "function") == 0)
        (*i)++;
    if (*i < nt && !toks[*i].is_op) {
        strncpy(n->func_name, toks[*i].text, 63);
        (*i)++;
    }
    if (*i < nt && toks[*i].is_op && toks[*i].op == '(')
        (*i)++;
    if (*i < nt && toks[*i].is_op && toks[*i].op == ')')
        (*i)++;
    if (*i < nt && toks[*i].is_op && toks[*i].op == '{')
        (*i)++;
    char body[2048];
    body[0] = '\0';
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (!t->is_op && strcmp(t->text, "}") == 0) {
            (*i)++;
            break;
        }
        strcat(body, t->text);
        strcat(body, " ");
        (*i)++;
    }
    n->func_body[2047] = '\0';
    strncpy(n->func_body, body, 2047);
    n->func_body[2047] = '\0';
    return n;
}

static struct ast_node *parse_group(struct token *toks, int nt, int *i) {
    (*i)++; /* { */
    struct ast_node *n = new_node(NODE_GROUP);
    char body[2048];
    body[0] = '\0';
    while (*i < nt) {
        struct token *t = &toks[*i];
        if (!t->is_op && strcmp(t->text, "}") == 0) {
            (*i)++;
            break;
        }
        strcat(body, t->text);
        strcat(body, " ");
        (*i)++;
    }
    n->body = strdup(body);
    return n;
}

/* ── Executor ────────────────────────────────────────────────── */
static int execute_node(struct ast_node *node) {
    if (!node)
        return 0;
    switch (node->type) {
    case NODE_LIST: {
        int code = 0;
        for (int c = 0; c < node->nchildren; c++) {
            struct ast_node *child = node->children[c];
            char op = node->ops[c];
            if (child->background) {
                int pid = fork();
                if (pid == 0) {
                    execute_node(child);
                    exit(last_exit_code);
                }
                job_add(pid, "<background>");
                code = 0;
                continue;
            }
            int r = execute_node(child);
            code = r;
            if (op == '&' && code != 0)
                break; /* && */
            if (op == '|' && code == 0)
                break; /* || */
        }
        return code;
    }
    case NODE_PIPELINE: {
        int nseg = node->nchildren;
        if (nseg == 1)
            return execute_command_node(node->children[0]);
        int pipes[MAX_SEGS - 1][2];
        int pids[MAX_SEGS];
        for (int k = 0; k < nseg - 1; k++)
            pipe(pipes[k]);
        int first_code = 0;
        for (int k = 0; k < nseg; k++) {
            pids[k] = fork();
            if (pids[k] == 0) {
                signal(SIGPIPE, SIG_DFL);
                if (k > 0)
                    dup2(pipes[k - 1][0], 0);
                if (k < nseg - 1)
                    dup2(pipes[k][1], 1);
                for (int j = 0; j < nseg - 1; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                int rc = execute_command_node(node->children[k]);
                exit(rc);
            }
        }
        for (int j = 0; j < nseg - 1; j++) {
            close(pipes[j][0]);
            close(pipes[j][1]);
        }
        for (int k = 0; k < nseg; k++) {
            int st = 0;
            waitpid(pids[k], &st, 0);
            int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            if (k == nseg - 1)
                first_code = rc;
            else if (opt_pipefail && rc != 0)
                first_code = rc;
        }
        return first_code;
    }
    case NODE_COMMAND:
        return execute_command_node(node);
    case NODE_SUBSHELL: {
        int pid = fork();
        if (pid == 0) {
            int r = run_line(node->body);
            exit(r);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    case NODE_IF: {
        int cond_code = run_line(node->cond);
        if (cond_code == 0)
            return run_line(node->body);
        if (node->else_body)
            return run_line(node->else_body);
        return 0;
    }
    case NODE_WHILE:
    case NODE_UNTIL: {
        int code = 0;
        while (1) {
            g_loop_ctl = 0;
            int cc = run_line(node->cond);
            int take = (node->type == NODE_WHILE) ? (cc == 0) : (cc != 0);
            if (!take)
                break;
            g_loop_ctl = 0;
            code = run_line(node->body);
            if (g_loop_ctl == 1)
                break; /* break */
            if (g_did_return)
                break;
        }
        g_loop_ctl = 0;
        return code;
    }
    case NODE_FOR: {
        int code = 0;
        for (int w = 0; w < node->for_nwords; w++) {
            g_loop_ctl = 0;
            sh_setenv(node->for_var, node->for_words[w]);
            code = run_line(node->body);
            if (g_loop_ctl == 1)
                break;
            if (g_did_return)
                break;
        }
        g_loop_ctl = 0;
        return code;
    }
    case NODE_CASE: {
        char *val = expand_word(node->case_word, &(int){0});
        for (int c = 0; c < node->case_n; c++) {
            if (fnmatch_simple(node->case_pat[c], val ? val : "")) {
                int r = run_line(node->case_act[c]);
                if (val)
                    free(val);
                return r;
            }
        }
        if (val)
            free(val);
        return 0;
    }
    case NODE_GROUP:
        return run_line(node->body);
    case NODE_FUNCTION: {
        struct shell_func *f = 0;
        for (int i = 0; i < MAX_FUNCS; i++)
            if (!shell_funcs[i].has_body) {
                f = &shell_funcs[i];
                break;
            }
        if (!f)
            return 1;
        strncpy(f->name, node->func_name, 63);
        f->name[63] = '\0';
        strncpy(f->body, node->func_body, 2047);
        f->body[2047] = '\0';
        f->has_body = 1;
        return 0;
    }
    }
    return 0;
}

static int execute_command_node(struct ast_node *node) {
    /* assignments first */
    for (int a = 0; a < node->nassigns; a++) {
        char *v = expand_word(node->assigns[a][1], &(int){0});
        sh_setenv(node->assigns[a][0], v ? v : "");
        if (v)
            free(v);
    }
    if (node->argc == 0) {
        /* only assignments/redirections */
        if (node->nredirs) {
            if (apply_redirs(node->redirs, node->nredirs) < 0)
                return 1;
        }
        return 0;
    }
    /* expand argv + glob */
    char *argv[MAX_ARGS];
    int argc = 0;
    for (int a = 0; a < node->argc; a++) {
        int fail = 0;
        char *exp = expand_word(node->argv[a], &fail);
        if (fail) {
            printf("sh: %s: unbound variable\n", node->argv[a]);
            if (exp)
                free(exp);
            return 1;
        }
        char *globs[64];
        int gn = glob_word(exp ? exp : "", globs, 64);
        for (int g = 0; g < gn && argc < MAX_ARGS - 1; g++)
            argv[argc++] = globs[g];
        if (exp)
            free(exp);
    }
    argv[argc] = 0;
    /* apply redirs */
    if (node->nredirs) {
        if (apply_redirs(node->redirs, node->nredirs) < 0)
            return 1;
    }
    /* function? */
    if (func_find(argv[0])) {
        struct shell_func *f = func_find(argv[0]);
        int r = run_line(f->body);
        return r;
    }
    /* alias expansion (top-level command only) */
    for (int i = 0; i < sh_alias_count; i++) {
        if (strcmp(shell_aliases[i].name, argv[0]) == 0) {
            int r = run_line(shell_aliases[i].value);
            /* free expanded argv */
            for (int a = 0; a < argc; a++)
                free(argv[a]);
            return r;
        }
    }
    /* builtin? */
    int r = run_builtin(argc, argv);
    if (r >= 0) {
        for (int a = 0; a < argc; a++)
            free(argv[a]);
        return node->invert ? (r ? 0 : 1) : r;
    }
    /* external */
    int pid = sh_exec_ext(argv);
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        for (int a = 0; a < argc; a++)
            free(argv[a]);
        return node->invert ? (code ? 0 : 1) : code;
    }
    printf("sh: %s: command not found\n", argv[0]);
    for (int a = 0; a < argc; a++)
        free(argv[a]);
    return 127;
}

/* Line dispatcher ──────────────────────────────────────────── */
static int run_line(const char *line) {
    if (!line || !*line)
        return 0;
    /* strip leading/trailing ws */
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#')
        return 0;
    /* function definition shorthand: name() { ... } */
    char tmp[4096];
    strncpy(tmp, p, 4095);
    tmp[4095] = '\0';
    struct ast_node *n = parse_list(tmp);
    if (!n)
        return 0;
    int code = execute_node(n);
    /* free tree (best-effort; leak acceptable in shell) */
    last_exit_code = code;
    if (opt_errexit && code != 0 &&
        !(n->type == NODE_LIST && (n->ops[0] == '|' || n->ops[0] == '&'))) {
        /* errexit: but don't exit for list ops */
    }
    return code;
}

static int execute_line_raw(const char *line) {
    int code = run_line(line);
    last_exit_code = code;
    if (opt_errexit && code != 0) {
        /* caller (script) checks g_errexit_abort */
    }
    return code;
}

/* ── Line input ──────────────────────────────────────────────── */
#ifndef SH_UNIT_TEST
/* ── Tab completion (D280 task 18) ───────────────────────────── */
#define COMPLETE_CANDS 128

/*
 * Fill `cands` with command names (builtins, aliases, or binaries under
 * /bin) that have the non-empty `word` as a prefix.  Names are copied
 * into `cands` so they outlive this call.  Returns the number of unique
 * matches (0 when `word` is empty or nothing matches).
 */
static int sh_complete_fill(const char *word, char cands[][128], int maxcands) {
    int ncand = 0;
    int wlen = 0;
    while (word[wlen])
        wlen++;
    if (wlen <= 0)
        return 0;

    /* builtins */
    for (int i = 0; sh_builtins[i] && ncand < maxcands; i++) {
        if (strncmp(sh_builtins[i], word, (unsigned long)wlen) == 0) {
            int dup = 0;
            for (int c = 0; c < ncand; c++)
                if (strcmp(cands[c], sh_builtins[i]) == 0) {
                    dup = 1;
                    break;
                }
            if (!dup)
                strncpy(cands[ncand++], sh_builtins[i], 127);
        }
    }
    /* aliases */
    for (int i = 0; i < sh_alias_count && ncand < maxcands; i++) {
        if (shell_aliases[i].used &&
            strncmp(shell_aliases[i].name, word, (unsigned long)wlen) == 0) {
            int dup = 0;
            for (int c = 0; c < ncand; c++)
                if (strcmp(cands[c], shell_aliases[i].name) == 0) {
                    dup = 1;
                    break;
                }
            if (!dup)
                strncpy(cands[ncand++], shell_aliases[i].name, 127);
        }
    }
    /* binaries under /bin */
    int fd = open("/bin", O_RDONLY, 0);
    if (fd >= 0) {
        char dbuf[2048];
        int n = getdents64(fd, dbuf, sizeof dbuf);
        close(fd);
        int pos = 0;
        while (pos < n && ncand < maxcands) {
            struct dirent *d = (struct dirent *)(dbuf + pos);
            if (d->d_name[0] && d->d_name[0] != '.' &&
                strncmp(d->d_name, word, (unsigned long)wlen) == 0) {
                int dup = 0;
                for (int c = 0; c < ncand; c++)
                    if (strcmp(cands[c], d->d_name) == 0) {
                        dup = 1;
                        break;
                    }
                if (!dup)
                    strncpy(cands[ncand++], d->d_name, 127);
            }
            pos += d->d_reclen;
        }
    }
    return ncand;
}

/*
 * Complete the word under the cursor: on a unique match, append the
 * completing suffix to `buf` and echo it; otherwise list the matches.
 */
static void sh_complete_word(char *buf, int *len, int max) {
    int start = *len;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t' && buf[start - 1] != '|' &&
           buf[start - 1] != ';' && buf[start - 1] != '&' && buf[start - 1] != '<' &&
           buf[start - 1] != '>')
        start--;
    int wlen = *len - start;
    char word[128];
    if (wlen >= (int)sizeof word)
        wlen = sizeof word - 1;
    for (int k = 0; k < wlen; k++)
        word[k] = buf[start + k];
    word[wlen] = '\0';

    char cands[COMPLETE_CANDS][128];
    int ncand = sh_complete_fill(word, cands, COMPLETE_CANDS);
    if (ncand == 1) {
        const char *full = cands[0];
        int flen = 0;
        while (full[flen])
            flen++;
        if (wlen < flen && *len + (flen - wlen) < max) {
            for (int k = wlen; k < flen && *len < max - 1; k++)
                buf[(*len)++] = full[k];
            buf[*len] = '\0';
            write(1, full + wlen, flen - wlen);
        }
    } else if (ncand > 1) {
        write(1, "\n", 1);
        for (int i = 0; i < ncand; i++)
            printf("%s  ", cands[i]);
        write(1, "\nsh$ ", 5);
        write(1, buf, *len);
    }
}

static int sh_getline(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c;
        int n = read(0, &c, 1);
        if (n <= 0)
            break;
        if (c == '\n') {
            buf[i] = '\0';
            return i;
        }
        if (c == '\t')
            sh_complete_word(buf, &i, max);
        else if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                write(1, "\b \b", 3);
            }
        } else
            buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}
#endif /* !SH_UNIT_TEST */

/* ── command which ───────────────────────────────────────────── */
static int cmd_which(char **argv) {
    if (!argv[1]) {
        printf("usage: which <command>\n");
        return 1;
    }
    const char *name = argv[1];
    for (int i = 0; sh_builtins[i]; i++)
        if (strcmp(name, sh_builtins[i]) == 0) {
            printf("%s: shell built-in\n", name);
            return 0;
        }
    char *path = sh_getenv("PATH");
    if (!path)
        path = "/bin";
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX);
    path_copy[PATH_MAX - 1] = '\0';
    char *dir = path_copy;
    while (dir) {
        char *next = sh_strchr(dir, ':');
        if (next)
            *next++ = '\0';
        char full[PATH_MAX];
        unsigned long pos = 0;
        while (dir[pos]) {
            full[pos] = dir[pos];
            pos++;
        }
        full[pos++] = '/';
        unsigned long j = 0;
        while (name[j]) {
            full[pos] = name[j];
            pos++;
            j++;
        }
        full[pos] = '\0';
        struct stat st;
        if (stat(full, &st) == 0) {
            printf("%s\n", full);
            return 0;
        }
        dir = next;
    }
    printf("which: %s: not found\n", name);
    return 1;
}

/* ── type builtin: display how a command would be interpreted ── */
static int cmd_type(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: type <command>...\n");
        return 1;
    }
    int rc = 0;
    for (int ai = 1; ai < argc; ai++) {
        const char *name = argv[ai];
        /* alias? */
        int found = 0;
        for (int i = 0; i < sh_alias_count; i++)
            if (strcmp(shell_aliases[i].name, name) == 0) {
                printf("%s is an alias for %s\n", name, shell_aliases[i].value);
                found = 1;
                break;
            }
        if (found)
            continue;
        /* function? */
        if (func_find(name)) {
            printf("%s is a function\n", name);
            continue;
        }
        /* shell builtin? */
        for (int i = 0; sh_builtins[i]; i++)
            if (strcmp(name, sh_builtins[i]) == 0) {
                printf("%s is a shell builtin\n", name);
                found = 1;
                break;
            }
        if (found)
            continue;
        /* executable on PATH? */
        char *path = sh_getenv("PATH");
        if (!path)
            path = "/bin";
        char path_copy[PATH_MAX];
        strncpy(path_copy, path, PATH_MAX);
        path_copy[PATH_MAX - 1] = '\0';
        char *dir = path_copy;
        while (dir) {
            char *next = sh_strchr(dir, ':');
            if (next)
                *next++ = '\0';
            char full[PATH_MAX];
            unsigned long pos = 0;
            while (dir[pos]) {
                full[pos] = dir[pos];
                pos++;
            }
            full[pos++] = '/';
            unsigned long j = 0;
            while (name[j]) {
                full[pos] = name[j];
                pos++;
                j++;
            }
            full[pos] = '\0';
            struct stat st;
            if (stat(full, &st) == 0) {
                printf("%s is %s\n", name, full);
                found = 1;
                break;
            }
            dir = next;
        }
        if (!found) {
            printf("type: %s: not found\n", name);
            rc = 1;
        }
    }
    return rc;
}

/* ── ulimit builtin: get/set resource limits via prlimit64 ────── */
/* Kernel RLIMIT_* indices (src/include/syscall.h, custom ABI) */
#define UL_AS 0
#define UL_CORE 1
#define UL_CPU 2
#define UL_DATA 3
#define UL_FSIZE 4
#define UL_NOFILE 5
#define UL_STACK 6
#define UL_NPROC 7
#define UL_MEMLOCK 8
#define UL_LOCKS 9
#define UL_NLIMITS 10

struct ul_rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

static int ul_limit_get_set(int resource, struct ul_rlimit *rlim, int do_set) {
    long res;
    long pid_self = 0;
    long new_arg = do_set ? (unsigned long)rlim : 0;
    long old_arg = do_set ? 0 : (unsigned long)rlim;
    asm volatile("mov $302, %%rax\n\t"
                 "mov %1, %%rdi\n\t" /* pid=0 (self) */
                 "mov %2, %%rsi\n\t" /* resource */
                 "mov %3, %%rdx\n\t" /* new (or 0) */
                 "mov %4, %%r10\n\t" /* old (or 0) */
                 "syscall"
                 : "=a"(res)
                 : "r"(pid_self), "r"((long)resource), "r"(new_arg), "r"(old_arg)
                 : "rcx", "r11", "memory");
    return (int)res;
}

static const char *ul_soft_names[] = {"address space (bytes)", "core file size (bytes)",
                                      "cpu time (sec)",        "data seg size (bytes)",
                                      "file size (bytes)",     "open files",
                                      "stack size (bytes)",    "max processes",
                                      "locked memory (bytes)", "file locks"};
#define UL_SOFT_COUNT (sizeof ul_soft_names / sizeof ul_soft_names[0])

static int cmd_ulimit(int argc, char **argv) {
    int opt = UL_FSIZE; /* default: file size */
    int hard = 0;
    int show_all = 0;
    int ai = 1;
    if (argc > 1 && argv[1][0] == '-') {
        const char *o = argv[1];
        if (o[1] == 'H' || o[1] == 'S') {
            hard = (o[1] == 'H');
            o += 2;
        }
        if (o[1] == 'a') {
            show_all = 1;
            ai = 2;
        } else {
            if (o[1] == 'c')
                opt = UL_CORE;
            else if (o[1] == 'd')
                opt = UL_DATA;
            else if (o[1] == 'f')
                opt = UL_FSIZE;
            else if (o[1] == 'l')
                opt = UL_MEMLOCK;
            else if (o[1] == 'n')
                opt = UL_NOFILE;
            else if (o[1] == 's')
                opt = UL_STACK;
            else if (o[1] == 't')
                opt = UL_CPU;
            else if (o[1] == 'u')
                opt = UL_NPROC;
            else if (o[1] == 'v')
                opt = UL_AS;
            ai = 2;
        }
    }
    struct ul_rlimit rlim;

    if (show_all) {
        for (int i = 0; i < (int)UL_SOFT_COUNT; i++) {
            if (ul_limit_get_set(i, &rlim, 0) == 0)
                printf("%-28s %slimit %lu\n", ul_soft_names[i], hard ? "hard " : "soft ",
                       hard ? rlim.rlim_max : rlim.rlim_cur);
        }
        return 0;
    }

    /* with a value argument -> set */
    if (ai < argc) {
        long val = atoi(argv[ai]);
        rlim.rlim_cur = (unsigned long)val;
        rlim.rlim_max = (unsigned long)val;
        if (ul_limit_get_set(opt, &rlim, 1) != 0) {
            printf("ulimit: cannot set limit\n");
            return 1;
        }
        return 0;
    }
    /* no argument -> query */
    if (ul_limit_get_set(opt, &rlim, 0) != 0) {
        printf("ulimit: no limit available\n");
        return 1;
    }
    printf("%lu\n", hard ? rlim.rlim_max : rlim.rlim_cur);
    return 0;
}

/* ── umask builtin: get/set file mode creation mask ──────────── */
static int cmd_umask(int argc, char **argv) {
    if (argc < 2) {
        /* query without changing: read then restore */
        unsigned int cur = umask(0);
        umask(cur);
        printf("%03o\n", cur);
        return 0;
    }
    char *p = argv[1];
    unsigned long newmask = 0;
    if (*p == '\0') {
        printf("umask: invalid mask: %s\n", argv[1]);
        return 1;
    }
    while (*p) {
        if (*p < '0' || *p > '7') {
            printf("umask: invalid mask: %s\n", argv[1]);
            return 1;
        }
        newmask = (newmask << 3) | (unsigned long)(*p - '0');
        p++;
    }
    umask((unsigned int)(newmask & 0777));
    return 0;
}

/* ── times builtin: report shell process CPU time ─────────────── */
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif
static int cmd_times(int argc, char **argv) {
    (void)argc;
    (void)argv;
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        printf("0m0s\n");
        return 1;
    }
    long long total_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    printf("%lldm%lld.%03llds\n", total_ms / 60000, (total_ms % 60000) / 1000, total_ms % 1000);
    return 0;
}

/* ── readonly builtin: mark variables readonly / list them ────── */
static int cmd_readonly(int argc, char **argv) {
    if (argc < 2) {
        /* list readonly variables with values */
        for (int i = 0; i < readonly_count; i++) {
            const char *val = sh_getenv(readonly_names[i]);
            printf("readonly %s=%s\n", readonly_names[i], val ? val : "");
        }
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char namebuf[64];
        const char *name = argv[i];
        const char *eq = sh_strchr(name, '=');
        if (eq) {
            unsigned long nlen = (unsigned long)(eq - name);
            if (nlen >= sizeof namebuf)
                nlen = sizeof namebuf - 1;
            memcpy(namebuf, name, nlen);
            namebuf[nlen] = '\0';
            if (!var_is_readonly(namebuf))
                sh_setenv(namebuf, eq + 1);
            var_mark_readonly(namebuf);
        } else {
            var_mark_readonly(name);
        }
    }
    return rc;
}

/* ── ps / free / uptime / uname / time / help ────────────────── */
static int cmd_ps(void) {
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) {
        printf("ps: cannot open /proc\n");
        return 1;
    }
    char buf[4096];
    int n = getdents64(fd, buf, 4096);
    close(fd);
    if (n <= 0) {
        printf("ps: no entries\n");
        return 1;
    }
    printf("  PID  PRI NAME\n");
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        int is_num = 1;
        char *pp = d->d_name;
        while (*pp) {
            if (*pp < '0' || *pp > '9') {
                is_num = 0;
                break;
            }
            pp++;
        }
        if (is_num) {
            int prio = 0;
            char statpath[64];
            snprintf(statpath, 64, "/proc/%s/stat", d->d_name);
            int sfd = open(statpath, O_RDONLY, 0);
            if (sfd >= 0) {
                char sbuf[128];
                int sr = read(sfd, sbuf, sizeof sbuf - 1);
                close(sfd);
                if (sr > 0) {
                    sbuf[sr] = '\0';
                    char *sp = sbuf;
                    char *last = sbuf;
                    while (*sp) {
                        if (*sp != ' ' && *sp != '\n')
                            last = sp;
                        sp++;
                    }
                    while (last > sbuf && last[-1] != ' ')
                        last--;
                    prio = atoi(last);
                }
            }
            char procpath[64];
            snprintf(procpath, 64, "/proc/%s/cmdline", d->d_name);
            int pfd = open(procpath, O_RDONLY, 0);
            if (pfd >= 0) {
                char cmdline[256];
                int r = read(pfd, cmdline, 255);
                close(pfd);
                if (r > 0) {
                    cmdline[r] = '\0';
                    for (int i = 0; i < r; i++)
                        if (cmdline[i] == '\0')
                            cmdline[i] = ' ';
                    printf("%5s %4d %s\n", d->d_name, prio, cmdline);
                } else
                    printf("%5s %4d %s\n", d->d_name, prio, d->d_name);
            } else
                printf("%5s %4d %s\n", d->d_name, prio, d->d_name);
        }
        pos += d->d_reclen;
    }
    return 0;
}
static int cmd_free(void) {
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) {
        printf("free: cannot open /proc/meminfo\n");
        return 1;
    }
    char buf[1024];
    int r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r > 0) {
        buf[r] = '\0';
        printf("%s", buf);
    }
    return 0;
}
static int cmd_uptime(void) {
    int fd = open("/proc/uptime", O_RDONLY, 0);
    if (fd < 0) {
        printf("uptime: cannot open /proc/uptime\n");
        return 1;
    }
    char buf[128];
    int r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r > 0) {
        buf[r] = '\0';
        printf("%s", buf);
    }
    return 0;
}
static int cmd_uname(int argc, char **argv) {
    struct utsname u;
    if (uname(&u) < 0) {
        printf("uname: failed\n");
        return 1;
    }
    if (argc < 2 || argv[1][0] != '-') {
        printf("%s\n", u.sysname);
        return 0;
    }
    int all = 0;
    const char *a = argv[1];
    for (int i = 1; a[i]; i++)
        if (a[i] == 'a')
            all = 1;
    if (all)
        printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
    else {
        for (int i = 1; a[i]; i++)
            switch (a[i]) {
            case 's':
                printf("%s ", u.sysname);
                break;
            case 'n':
                printf("%s ", u.nodename);
                break;
            case 'r':
                printf("%s ", u.release);
                break;
            case 'v':
                printf("%s ", u.version);
                break;
            case 'm':
                printf("%s ", u.machine);
                break;
            }
        printf("\n");
    }
    return 0;
}
static int cmd_time(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: time <command> [args...]\n");
        return 1;
    }
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int pid = sh_exec_ext(argv + 1);
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st))
            last_exit_code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st))
            last_exit_code = 128 + WTERMSIG(st);
    } else {
        printf("time: command not found: %s\n", argv[1]);
        last_exit_code = 127;
        return 127;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    long long sec = ts1.tv_sec - ts0.tv_sec;
    long long ms = (ts1.tv_nsec - ts0.tv_nsec) / 1000000;
    if (ms < 0) {
        sec--;
        ms += 1000;
    }
    printf("\nreal\t%lld.%03llds\n", sec, ms);
    return last_exit_code;
}
static int cmd_help(void) {
    printf("Hermes shell built-ins:\n");
    printf("  cd pwd exit echo clear export unset local set alias unalias\n");
    printf("  jobs fg bg kill source . trap read which ps free uptime uname time\n");
    printf("  test [ true false break continue return help\n");
    printf("  Control: if/then/else fi, for/in, while/until, case/esac,\n");
    printf("          { } groups, ( ) subshells, functions\n");
    printf("  Expansion: $VAR ${VAR:-x} ${VAR#p} $((a+b)) arrays, globbing\n");
    printf("  Options: set -e -u -o pipefail\n");
    return 0;
}

/* ── getpid wrapper (kernel has no getpid libc? use syscall) ──── */
static int getpid_impl(void) {
    /* syscall 212 = SYS_GETPID if defined, else read from /proc/self */
    long pid = 0;
    asm volatile("movl $212, %%eax; syscall" : "=a"(pid) : : "rcx", "r11");
    if (pid == 0)
        pid = 1;
    return (int)pid;
}

/* ── Main ─────────────────────────────────────────────────────── */
#ifndef SH_UNIT_TEST
int main(int argc, char *argv[]) {
    sh_init_env();
    signal(SIGPIPE, SIG_IGN);

    /* Script mode: sh -c "script" or sh <file> */
    if (argc >= 2) {
        if (strcmp(argv[1], "-c") == 0 && argc >= 3) {
            int code = run_line(argv[2]);
            return code;
        }
        struct stat st;
        if (stat(argv[1], &st) == 0 && S_ISREG(st.st_mode)) {
            return source_file(argv[1]);
        }
        /* else run argv[1] as a command with argv[1..] */
        char *cmd_argv[MAX_ARGS];
        int cmd_argc = argc - 1;
        for (int i = 0; i < cmd_argc && i < MAX_ARGS - 1; i++)
            cmd_argv[i] = argv[i + 1];
        cmd_argv[cmd_argc] = 0;
        int r = run_builtin(cmd_argc, cmd_argv);
        if (r >= 0)
            return r;
        int pid = sh_exec_ext(cmd_argv);
        if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        }
        printf("sh: %s: not found\n", cmd_argv[0]);
        return 127;
    }

    char line[MAX_LINE];
    while (1) {
        write(1, "\nsh$ ", 5);
        int n = sh_getline(line, MAX_LINE);
        if (n <= 0) {
            write(1, "\n", 1);
            break;
        }
        /* block joining for multi-line constructs */
        char joined[8192];
        strncpy(joined, line, 8191);
        joined[8191] = '\0';
        /* crude: if line opens a compound without closing, keep reading */
        if (strstr(joined, "if ") && !strstr(joined, "fi") && strstr(joined, "then")) {
            while (1) {
                write(1, "> ", 2);
                char more[MAX_LINE];
                int m = sh_getline(more, MAX_LINE);
                if (m <= 0)
                    break;
                strncat(joined, "\n", 8191);
                strncat(joined, more, 8191);
                if (strstr(more, "fi"))
                    break;
            }
        } else if (strstr(joined, "while ") && !strstr(joined, "done")) {
            while (1) {
                write(1, "> ", 2);
                char more[MAX_LINE];
                int m = sh_getline(more, MAX_LINE);
                if (m <= 0)
                    break;
                strncat(joined, "\n", 8191);
                strncat(joined, more, 8191);
                if (strstr(more, "done"))
                    break;
            }
        } else if (strstr(joined, "for ") && !strstr(joined, "done")) {
            while (1) {
                write(1, "> ", 2);
                char more[MAX_LINE];
                int m = sh_getline(more, MAX_LINE);
                if (m <= 0)
                    break;
                strncat(joined, "\n", 8191);
                strncat(joined, more, 8191);
                if (strstr(more, "done"))
                    break;
            }
        } else if (strstr(joined, "case ") && !strstr(joined, "esac")) {
            while (1) {
                write(1, "> ", 2);
                char more[MAX_LINE];
                int m = sh_getline(more, MAX_LINE);
                if (m <= 0)
                    break;
                strncat(joined, "\n", 8191);
                strncat(joined, more, 8191);
                if (strstr(more, "esac"))
                    break;
            }
        }
        /* array assignment NAME=(...) */
        if (sh_try_array_assign(joined)) {
            last_exit_code = 0;
            continue;
        }
        job_reap(WNOHANG);
        int code = execute_line_raw(joined);
        last_exit_code = code;
    }
    return last_exit_code;
}
#endif /* !SH_UNIT_TEST */

/* ── array assignment (kept from original) ───────────────────── */
#ifndef SH_UNIT_TEST
static int sh_try_array_assign(char *line) {
    char *eq = line;
    while (*eq && *eq != '=')
        eq++;
    if (*eq != '=')
        return 0;
    if (eq[1] != '(')
        return 0;
    char *name = line;
    if (name == eq)
        return 0;
    *eq = '\0';
    char *nn = name;
    while (nn < eq && (*nn == ' ' || *nn == '\t'))
        nn++;
    if (!*nn) {
        *eq = '=';
        return 0;
    }
    struct shell_array *a = array_get_or_create(nn);
    if (!a) {
        *eq = '=';
        return 0;
    }
    a->count = 0;
    char *p = eq + 2;
    char *close = p;
    while (*close && *close != ')')
        close++;
    if (*close == ')')
        *close = '\0';
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *elem = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
        if (a->count < MAX_ARRAY_ELEMS) {
            strncpy(a->elems[a->count], elem, MAX_ARRAY_ELEM_LEN - 1);
            a->elems[a->count][MAX_ARRAY_ELEM_LEN - 1] = '\0';
            a->count++;
        }
    }
    return 1;
}
#endif /* !SH_UNIT_TEST */
