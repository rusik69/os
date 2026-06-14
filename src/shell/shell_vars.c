#include "shell.h"
#include "string.h"

#define MAX_VARS      32
#define MAX_VAR_NAME  32
#define MAX_VAR_VALUE 128

static char var_names [MAX_VARS][MAX_VAR_NAME];
static char var_values[MAX_VARS][MAX_VAR_VALUE];
static int  var_count = 0;

void shell_var_set(const char *name, const char *value) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(var_names[i], name) == 0) {
            strncpy(var_values[i], value, MAX_VAR_VALUE - 1);
            var_values[i][MAX_VAR_VALUE - 1] = '\0';
            return;
        }
    }
    if (var_count < MAX_VARS) {
        strncpy(var_names[var_count],  name,  MAX_VAR_NAME  - 1);
        strncpy(var_values[var_count], value, MAX_VAR_VALUE - 1);
        var_names [var_count][MAX_VAR_NAME  - 1] = '\0';
        var_values[var_count][MAX_VAR_VALUE - 1] = '\0';
        var_count++;
    }
}

const char *shell_var_get(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_names[i], name) == 0)
            return var_values[i];
    return "";
}

int shell_var_count(void) { return var_count; }

const char *shell_var_name(int i) {
    return (i >= 0 && i < var_count) ? var_names[i] : "";
}

const char *shell_var_value(int i) {
    return (i >= 0 && i < var_count) ? var_values[i] : "";
}

/* ── Variable expansion helpers ────────────────────────────────────── */

/* shell_var_expand: Expand ${name}, ${name:-default}, ${name:+alt},
 * and ${#name} syntax. Returns -1 on malformed input, 0 on success.
 * 'dst' is written up to 'dst_size' bytes (including NUL). */
int shell_var_expand(const char *src, char *dst, int dst_size)
{
    if (!src || !dst || dst_size <= 0) return -1;
    int di = 0;

    while (*src && di < dst_size - 1) {
        if (*src == '$' && src[1] == '{') {
            src += 2; /* skip "${" */

            /* ${#name} — length expansion */
            if (*src == '#') {
                src++;
                char name[MAX_VAR_NAME];
                int ni = 0;
                while (*src && *src != ':' && *src != '}' && ni < MAX_VAR_NAME - 1)
                    name[ni++] = *src++;
                name[ni] = '\0';
                /* skip past '}' */
                if (*src == '}') src++;
                const char *val = shell_var_get(name);
                int slen = (val && *val) ? (int)strlen(val) : 0;
                /* convert slen to decimal string */
                char tmp[16];
                int ti = 0;
                if (slen == 0) {
                    if (di < dst_size - 1) dst[di++] = '0';
                } else {
                    while (slen > 0 && ti < 14) {
                        tmp[ti++] = '0' + (slen % 10);
                        slen /= 10;
                    }
                    while (ti > 0 && di < dst_size - 1)
                        dst[di++] = tmp[--ti];
                }
                continue;
            }

            /* ${name:-default} or ${name:+alt} */
            char name[MAX_VAR_NAME];
            int ni = 0;
            while (*src && *src != ':' && *src != '}' && ni < MAX_VAR_NAME - 1)
                name[ni++] = *src++;
            name[ni] = '\0';

            if (*src == ':') {
                src++;
                char op = *src; /* '-' or '+' */
                src++;
                char defval[128];
                int dvi = 0;
                while (*src && *src != '}' && dvi < (int)sizeof(defval) - 1)
                    defval[dvi++] = *src++;
                defval[dvi] = '\0';
                if (*src == '}') src++;

                const char *val = shell_var_get(name);
                if (op == '-') {
                    /* Use default if unset or empty */
                    if (!val || !*val) {
                        const char *dp = defval;
                        while (*dp && di < dst_size - 1) dst[di++] = *dp++;
                    } else {
                        while (*val && di < dst_size - 1) dst[di++] = *val++;
                    }
                } else if (op == '+') {
                    /* Use alternate if set and non-empty */
                    if (val && *val) {
                        const char *dp = defval;
                        while (*dp && di < dst_size - 1) dst[di++] = *dp++;
                    }
                }
                continue;
            }

            /* ${name} — plain variable */
            if (*src == '}') src++;
            const char *val = shell_var_get(name);
            if (val && *val) {
                while (*val && di < dst_size - 1) dst[di++] = *val++;
            }
            continue;
        }

        /* $? — last exit status (handled elsewhere, skip) */
        if (*src == '$' && src[1] == '?') {
            src += 2;
            continue;
        }

        /* $NAME — simple variable (no braces) */
        if (*src == '$' && ((src[1] >= 'A' && src[1] <= 'Z') ||
                            (src[1] >= 'a' && src[1] <= 'z') ||
                            src[1] == '_')) {
            src++;
            char name[MAX_VAR_NAME];
            int ni = 0;
            while (*src && ni < MAX_VAR_NAME - 1 &&
                   ((*src >= 'A' && *src <= 'Z') ||
                    (*src >= 'a' && *src <= 'z') ||
                    (*src >= '0' && *src <= '9') ||
                    *src == '_'))
                name[ni++] = *src++;
            name[ni] = '\0';
            const char *val = shell_var_get(name);
            if (val && *val) {
                while (*val && di < dst_size - 1) dst[di++] = *val++;
            }
            continue;
        }

        dst[di++] = *src++;
    }
    dst[di] = '\0';
    return 0;
}
