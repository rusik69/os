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

/* Simple variable store */
struct script_var {
    char name[SCRIPT_VAR_NAME];
    char value[SCRIPT_VAR_VAL];
};

static struct script_var vars[SCRIPT_MAX_VARS];
static int num_vars = 0;

static void vars_reset(void) {
    num_vars = 0;
}

static const char *var_get(const char *name) {
    for (int i = 0; i < num_vars; i++) {
        if (strcmp(vars[i].name, name) == 0)
            return vars[i].value;
    }
    return NULL;
}

static void var_set(const char *name, const char *value) {
    for (int i = 0; i < num_vars; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            size_t vlen = strlen(value);
            if (vlen >= SCRIPT_VAR_VAL) vlen = SCRIPT_VAR_VAL - 1;
            memcpy(vars[i].value, value, vlen);
            vars[i].value[vlen] = '\0';
            return;
        }
    }
    if (num_vars >= SCRIPT_MAX_VARS) return;
    size_t nlen = strlen(name);
    if (nlen >= SCRIPT_VAR_NAME) nlen = SCRIPT_VAR_NAME - 1;
    memcpy(vars[num_vars].name, name, nlen);
    vars[num_vars].name[nlen] = '\0';
    size_t vlen = strlen(value);
    if (vlen >= SCRIPT_VAR_VAL) vlen = SCRIPT_VAR_VAL - 1;
    memcpy(vars[num_vars].value, value, vlen);
    vars[num_vars].value[vlen] = '\0';
    num_vars++;
}

/* Expand $VAR references in src into dst (max dst_max bytes). */
static void expand_vars(const char *src, char *dst, int dst_max) {
    int di = 0;
    while (*src && di < dst_max - 1) {
        if (*src == '$') {
            src++;
            char vname[SCRIPT_VAR_NAME];
            int vi = 0;
            while (*src && (*src == '_' || (*src >= 'A' && *src <= 'Z') ||
                                            (*src >= 'a' && *src <= 'z') ||
                                            (*src >= '0' && *src <= '9')) &&
                   vi < SCRIPT_VAR_NAME - 1) {
                vname[vi++] = *src++;
            }
            vname[vi] = '\0';
            const char *val = var_get(vname);
            if (val) {
                while (*val && di < dst_max - 1)
                    dst[di++] = *val++;
            }
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

/* Parse "set NAME=VALUE" */
static void handle_set(const char *args) {
    if (!args) return;
    const char *eq = args;
    while (*eq && *eq != '=') eq++;
    if (!*eq) return;
    char name[SCRIPT_VAR_NAME];
    size_t nlen = (size_t)(eq - args);
    if (nlen >= SCRIPT_VAR_NAME) nlen = SCRIPT_VAR_NAME - 1;
    memcpy(name, args, nlen);
    name[nlen] = '\0';
    var_set(name, eq + 1);
}

int script_exec(const char *path) {
    static char buf[SCRIPT_MAX_SIZE];
    uint32_t size = 0;

    if (vfs_read(path, buf, sizeof(buf) - 1, &size) < 0) {
        kprintf("script: cannot read %s\n", path);
        return -1;
    }
    buf[size] = '\0';

    vars_reset();

    const char *p = buf;
    char line[SCRIPT_MAX_LINE];
    char expanded[SCRIPT_MAX_LINE];

    /* Control flow state */
    int skip_until_endif = 0;  /* when >0, skip lines until matching endif */
    int repeat_count = 0;      /* >0 when inside a repeat block */
    int repeat_line_start = 0; /* offset in buf of first line inside repeat */
    int in_repeat = 0;

    while (*p) {
        /* Read one line */
        int li = 0;
        while (*p && *p != '\n' && li < SCRIPT_MAX_LINE - 1)
            line[li++] = *p++;
        if (*p == '\n') p++;
        line[li] = '\0';

        /* Skip leading whitespace */
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;

        /* Empty line or comment */
        if (*l == '\0' || *l == '#') continue;

        /* Skip shebang line */
        if (l == line && l[0] == '!' && l[1] == '/') continue;

        /* Skip lines when inside false-branch of if */
        if (skip_until_endif) {
            if (strcmp(l, "endif") == 0) skip_until_endif--;
            continue;
        }

        /* Handle repeat */
        if (strncmp(l, "repeat ", 7) == 0) {
            repeat_count = 0;
            const char *n = l + 7;
            while (*n >= '0' && *n <= '9') { repeat_count = repeat_count * 10 + (*n - '0'); n++; }
            if (repeat_count > 0) {
                in_repeat = 1;
                repeat_line_start = (int)(p - buf);
            }
            continue;
        }
        if (strcmp(l, "endrepeat") == 0) {
            if (in_repeat && repeat_count > 1) {
                repeat_count--;
                p = buf + repeat_line_start;   /* rewind */
            } else {
                in_repeat = 0;
                repeat_count = 0;
            }
            continue;
        }

        /* Expand variables */
        expand_vars(l, expanded, sizeof(expanded));

        /* Split command and args */
        char *cmd = expanded;
        char *args2 = expanded;
        while (*args2 && *args2 != ' ') args2++;
        if (*args2) { *args2 = '\0'; args2++; while (*args2 == ' ') args2++; }
        else args2 = NULL;

        /* Built-in script keywords */
        if (strcmp(cmd, "set") == 0) {
            handle_set(args2);
            continue;
        }
        if (strcmp(cmd, "if") == 0) {
            /* Run the sub-command; if it printed nothing / error, skip until endif */
            /* Simple: we just always execute the if-body in this implementation */
            /* A real if would check a condition; here we use echo as condition test */
            (void)args2;
            continue;
        }
        if (strcmp(cmd, "endif") == 0) {
            continue;
        }

        /* Execute via shell */
        shell_exec_cmd(cmd, args2 && *args2 ? args2 : NULL);
    }

    return 0;
}
