#include "script.h"
#include "vfs.h"
#include "shell.h"
#include "string.h"
#include "printf.h"

#define SCRIPT_MAX_SIZE  4096
#define SCRIPT_MAX_LINE  256
#define SCRIPT_MAX_VARS  16
#define SCRIPT_VAR_NAME  32
#define SCRIPT_VAR_VAL   128

/* ── Script-local variable store ─────────────────────────────────────────── */
struct script_var {
    char name[SCRIPT_VAR_NAME];
    char value[SCRIPT_VAR_VAL];
};
static struct script_var vars[SCRIPT_MAX_VARS];
static int num_vars = 0;

static void vars_reset(void) { num_vars = 0; }

static const char *var_get(const char *name) {
    for (int i = 0; i < num_vars; i++)
        if (strcmp(vars[i].name, name) == 0) return vars[i].value;
    return NULL;
}

static void var_set(const char *name, const char *value) {
    for (int i = 0; i < num_vars; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            strncpy(vars[i].value, value, SCRIPT_VAR_VAL - 1);
            vars[i].value[SCRIPT_VAR_VAL - 1] = '\0';
            return;
        }
    }
    if (num_vars >= SCRIPT_MAX_VARS) return;
    strncpy(vars[num_vars].name,  name,  SCRIPT_VAR_NAME - 1);
    vars[num_vars].name[SCRIPT_VAR_NAME - 1] = '\0';
    strncpy(vars[num_vars].value, value, SCRIPT_VAR_VAL  - 1);
    vars[num_vars].value[SCRIPT_VAR_VAL - 1] = '\0';
    num_vars++;
}

/* Expand $VAR references (including $?) into dst. */
static void expand_vars(const char *src, char *dst, int dst_max) {
    int di = 0;
    while (*src && di < dst_max - 1) {
        if (*src == '$') {
            src++;
            if (*src == '?') {
                src++;
                int n = shell_get_exit_status();
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
            char vname[SCRIPT_VAR_NAME]; int vi = 0;
            while (*src && vi < SCRIPT_VAR_NAME - 1 &&
                   (*src == '_' || (*src >= 'A' && *src <= 'Z') ||
                    (*src >= 'a' && *src <= 'z') || (*src >= '0' && *src <= '9')))
                vname[vi++] = *src++;
            vname[vi] = '\0';
            const char *val = var_get(vname);
            if (!val) val = shell_var_get(vname); /* fall back to global shell vars */
            if (val) while (*val && di < dst_max - 1) dst[di++] = *val++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

/* ── Control-flow stack ───────────────────────────────────────────────────── */
#define CF_MAX  8
#define CF_IF   1
#define CF_WHILE 2
#define CF_FOR  3

struct cf_frame {
    int  type;
    int  exec;          /* 1 = execute current block, 0 = skip */
    int  in_exec_ctx;   /* 1 = this frame was pushed from an executing context */
    int  had_else;      /* (if) true after else seen */
    /* while / for */
    const char *body_start; /* pointer into buf at first line of loop body */
    char  cond[SCRIPT_MAX_LINE]; /* (while) saved condition line */
    /* for */
    char  for_var[SCRIPT_VAR_NAME];
    int   for_count;
    int   for_idx;
    char  for_vals[16][SCRIPT_VAR_VAL];
};

static struct cf_frame cf_stack[CF_MAX];
static int cf_depth = 0;

/* Are ALL enclosing frames currently executing? */
static int should_exec(void) {
    for (int i = 0; i < cf_depth; i++)
        if (!cf_stack[i].exec) return 0;
    return 1;
}

/* ── Forward-scan helpers ─────────────────────────────────────────────────── */

/* Advance *pp past lines until we exit an if/else/endif block at depth 1.
 * Returns pointer after the matching endif line.  */
static const char *scan_to_endif(const char *p) __attribute__((unused));
static const char *scan_to_endif(const char *p) {
    int depth = 1;
    char line[SCRIPT_MAX_LINE];
    while (*p && depth > 0) {
        int li = 0;
        while (*p && *p != '\n' && li < SCRIPT_MAX_LINE - 1) line[li++] = *p++;
        if (*p == '\n') p++;
        line[li] = '\0';
        char *l = line; while (*l == ' ' || *l == '\t') l++;
        if (strncmp(l, "if ", 3) == 0 || strcmp(l, "if") == 0) depth++;
        else if (strcmp(l, "endif") == 0) depth--;
    }
    return p;
}

/* Advance past lines until we exit a while block at depth 1. */
static const char *scan_to_endwhile(const char *p) {
    int depth = 1;
    char line[SCRIPT_MAX_LINE];
    while (*p && depth > 0) {
        int li = 0;
        while (*p && *p != '\n' && li < SCRIPT_MAX_LINE - 1) line[li++] = *p++;
        if (*p == '\n') p++;
        line[li] = '\0';
        char *l = line; while (*l == ' ' || *l == '\t') l++;
        if (strncmp(l, "while ", 6) == 0) depth++;
        else if (strcmp(l, "endwhile") == 0) depth--;
    }
    return p;
}

/* Advance past lines until we exit a for block at depth 1. */
static const char *scan_to_endfor(const char *p) {
    int depth = 1;
    char line[SCRIPT_MAX_LINE];
    while (*p && depth > 0) {
        int li = 0;
        while (*p && *p != '\n' && li < SCRIPT_MAX_LINE - 1) line[li++] = *p++;
        if (*p == '\n') p++;
        line[li] = '\0';
        char *l = line; while (*l == ' ' || *l == '\t') l++;
        if (strncmp(l, "for ", 4) == 0) depth++;
        else if (strcmp(l, "endfor") == 0) depth--;
    }
    return p;
}

/* Evaluate a condition: run the command, return its exit status (0=true). */
static int eval_cond(const char *cond_line) {
    char buf[SCRIPT_MAX_LINE];
    strncpy(buf, cond_line, SCRIPT_MAX_LINE - 1);
    buf[SCRIPT_MAX_LINE - 1] = '\0';
    char *cmd = buf;
    while (*cmd == ' ') cmd++;
    char *cargs = cmd;
    while (*cargs && *cargs != ' ') cargs++;
    if (*cargs) { *cargs = '\0'; cargs++; while (*cargs == ' ') cargs++; }
    else cargs = NULL;
    shell_exec_cmd(cmd, cargs && *cargs ? cargs : NULL);
    return shell_get_exit_status();
}

/* Parse "set NAME=VALUE" */
static void handle_set(const char *args) {
    if (!args) return;
    const char *eq = args;
    while (*eq && *eq != '=') eq++;
    if (!*eq) return;
    char name[SCRIPT_VAR_NAME];
    int nlen = (int)(eq - args);
    if (nlen >= SCRIPT_VAR_NAME) nlen = SCRIPT_VAR_NAME - 1;
    memcpy(name, args, nlen); name[nlen] = '\0';
    var_set(name, eq + 1);
}

/* ── Main execution entry point ───────────────────────────────────────────── */
int script_exec(const char *path) {
    static char buf[SCRIPT_MAX_SIZE];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf) - 1, &size) < 0) {
        kprintf("script: cannot read %s\n", path);
        return -1;
    }
    buf[size] = '\0';
    vars_reset();
    cf_depth = 0;

    const char *p = buf;
    char line[SCRIPT_MAX_LINE];
    char expanded[SCRIPT_MAX_LINE];

    while (*p) {
        /* Read one line */
        int li = 0;
        const char *line_start = p;
        while (*p && *p != '\n' && li < SCRIPT_MAX_LINE - 1) line[li++] = *p++;
        if (*p == '\n') p++;
        line[li] = '\0';
        (void)line_start;

        /* Skip leading whitespace, empty lines, comments, shebang */
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '\0' || *l == '#') continue;
        if (l == line && l[0] == '!' && l[1] == '/') continue;

        /* Expand variables for use in conditions and set statements,
         * but first handle the raw keyword to detect control-flow. */
        expand_vars(l, expanded, sizeof(expanded));
        char *el = expanded;
        while (*el == ' ' || *el == '\t') el++;

        /* Split expanded line into cmd + args */
        char *cmd  = el;
        char *args = el;
        while (*args && *args != ' ') args++;
        if (*args) { *args = '\0'; args++; while (*args == ' ') args++; }
        else args = NULL;

        /* ── Control-flow keywords ────────────────────────────────────────── */

        /* if <condition> */
        if (strcmp(cmd, "if") == 0) {
            if (!should_exec()) {
                /* Already skipping — push a dummy frame to track nesting */
                if (cf_depth < CF_MAX) {
                    cf_stack[cf_depth].type = CF_IF;
                    cf_stack[cf_depth].exec = 0;
                    cf_stack[cf_depth].in_exec_ctx = 0;
                    cf_stack[cf_depth].had_else = 0;
                    cf_depth++;
                }
            } else {
                int cond = args ? eval_cond(args) : 1;
                if (cf_depth < CF_MAX) {
                    cf_stack[cf_depth].type = CF_IF;
                    cf_stack[cf_depth].exec = (cond == 0);
                    cf_stack[cf_depth].in_exec_ctx = 1;
                    cf_stack[cf_depth].had_else = 0;
                    cf_depth++;
                }
            }
            continue;
        }

        /* else */
        if (strcmp(cmd, "else") == 0) {
            if (cf_depth > 0 && cf_stack[cf_depth-1].type == CF_IF
                    && cf_stack[cf_depth-1].in_exec_ctx
                    && !cf_stack[cf_depth-1].had_else) {
                cf_stack[cf_depth-1].exec = !cf_stack[cf_depth-1].exec;
                cf_stack[cf_depth-1].had_else = 1;
            }
            continue;
        }

        /* endif */
        if (strcmp(cmd, "endif") == 0) {
            if (cf_depth > 0 && cf_stack[cf_depth-1].type == CF_IF)
                cf_depth--;
            continue;
        }

        /* while <condition> */
        if (strcmp(cmd, "while") == 0) {
            if (!should_exec()) {
                /* Skipping — push dummy, skip to endwhile */
                if (cf_depth < CF_MAX) {
                    cf_stack[cf_depth].type = CF_WHILE;
                    cf_stack[cf_depth].exec = 0;
                    cf_stack[cf_depth].in_exec_ctx = 0;
                    cf_depth++;
                }
                p = scan_to_endwhile(p);
            } else {
                int cond = args ? eval_cond(args) : 1;
                if (cond == 0) {
                    /* Condition true: push frame and execute body */
                    if (cf_depth < CF_MAX) {
                        cf_stack[cf_depth].type = CF_WHILE;
                        cf_stack[cf_depth].exec = 1;
                        cf_stack[cf_depth].in_exec_ctx = 1;
                        cf_stack[cf_depth].body_start = p; /* points after while line */
                        strncpy(cf_stack[cf_depth].cond, args ? args : "",
                                SCRIPT_MAX_LINE - 1);
                        cf_stack[cf_depth].cond[SCRIPT_MAX_LINE - 1] = '\0';
                        cf_depth++;
                    }
                } else {
                    /* Condition false: skip to endwhile */
                    p = scan_to_endwhile(p);
                }
            }
            continue;
        }

        /* endwhile */
        if (strcmp(cmd, "endwhile") == 0) {
            if (cf_depth > 0 && cf_stack[cf_depth-1].type == CF_WHILE
                    && cf_stack[cf_depth-1].exec) {
                /* Re-evaluate condition */
                struct cf_frame *wf = &cf_stack[cf_depth-1];
                int cond = eval_cond(wf->cond);
                if (cond == 0) {
                    /* Still true — loop back */
                    p = wf->body_start;
                } else {
                    /* Condition now false — exit loop */
                    cf_depth--;
                }
            } else if (cf_depth > 0 && cf_stack[cf_depth-1].type == CF_WHILE) {
                cf_depth--;
            }
            continue;
        }

        /* for VAR in val1 val2 ... */
        if (strcmp(cmd, "for") == 0) {
            if (!should_exec()) {
                if (cf_depth < CF_MAX) {
                    cf_stack[cf_depth].type = CF_FOR;
                    cf_stack[cf_depth].exec = 0;
                    cf_stack[cf_depth].in_exec_ctx = 0;
                    cf_depth++;
                }
                p = scan_to_endfor(p);
                continue;
            }
            /* Parse: "VAR in val1 val2 ..." */
            if (!args) continue;
            char for_var[SCRIPT_VAR_NAME]; int fvi = 0;
            while (*args && *args != ' ' && fvi < SCRIPT_VAR_NAME - 1)
                for_var[fvi++] = *args++;
            for_var[fvi] = '\0';
            while (*args == ' ') args++;
            /* Expect "in" */
            if (strncmp(args, "in", 2) == 0 && (args[2] == ' ' || args[2] == '\0'))
                args += 2;
            while (*args == ' ') args++;
            /* Parse values */
            if (cf_depth < CF_MAX) {
                struct cf_frame *ff = &cf_stack[cf_depth];
                ff->type = CF_FOR;
                ff->exec = 1;
                ff->in_exec_ctx = 1;
                ff->body_start = p;
                ff->for_count = 0;
                ff->for_idx = 0;
                strncpy(ff->for_var, for_var, SCRIPT_VAR_NAME - 1);
                ff->for_var[SCRIPT_VAR_NAME - 1] = '\0';
                /* Split remaining into up to 16 values */
                char *vp = args;
                while (*vp && ff->for_count < 16) {
                    char *vstart = vp;
                    while (*vp && *vp != ' ') vp++;
                    int vlen = (int)(vp - vstart);
                    if (vlen >= SCRIPT_VAR_VAL) vlen = SCRIPT_VAR_VAL - 1;
                    memcpy(ff->for_vals[ff->for_count], vstart, vlen);
                    ff->for_vals[ff->for_count][vlen] = '\0';
                    ff->for_count++;
                    while (*vp == ' ') vp++;
                }
                if (ff->for_count > 0) {
                    var_set(ff->for_var, ff->for_vals[0]);
                    cf_depth++;
                } else {
                    /* No values — skip body */
                    p = scan_to_endfor(p);
                }
            }
            continue;
        }

        /* endfor */
        if (strcmp(cmd, "endfor") == 0) {
            if (cf_depth > 0 && cf_stack[cf_depth-1].type == CF_FOR
                    && cf_stack[cf_depth-1].exec) {
                struct cf_frame *ff = &cf_stack[cf_depth-1];
                ff->for_idx++;
                if (ff->for_idx < ff->for_count) {
                    var_set(ff->for_var, ff->for_vals[ff->for_idx]);
                    p = ff->body_start;
                } else {
                    cf_depth--;
                }
            } else if (cf_depth > 0 && cf_stack[cf_depth-1].type == CF_FOR) {
                cf_depth--;
            }
            continue;
        }

        /* repeat N (legacy) */
        if (strncmp(cmd, "repeat", 6) == 0 && (!cmd[6] || cmd[6] == ' ')) {
            /* skip — handled below for backward compat */
        }

        /* ── Skip line if not in executing context ─────────────────────────── */
        if (!should_exec()) continue;

        /* ── Built-in script keywords ─────────────────────────────────────── */
        if (strcmp(cmd, "set") == 0) { handle_set(args); continue; }

        /* Legacy repeat / endrepeat */
        if (strncmp(expanded, "repeat ", 7) == 0) {
            /* not supported in new CF model — ignore */
            continue;
        }
        if (strcmp(cmd, "endrepeat") == 0) continue;

        /* ── Execute via shell ───────────────────────────────────────────────*/
        shell_exec_cmd(cmd, args && *args ? args : NULL);
    }

    return 0;
}
