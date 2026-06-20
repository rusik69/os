#include "shell.h"
#include "string.h"

#define MAX_VARS      32
#define MAX_VAR_NAME  32
#define MAX_VAR_VALUE 128
#define MAX_ARRAY_ITEMS 32

/* ── Variable table ────────────────────────────────────────────────── */

static char var_names [MAX_VARS][MAX_VAR_NAME];
static char var_values[MAX_VARS][MAX_VAR_VALUE];
static int  var_count = 0;

/* ── Array support ────────────────────────────────────────────────────
 *
 * Arrays are stored as: name[0]="value0", name[1]="value1", etc.
 * Each element is a separate variable entry with the name "name[idx]".
 * The total count per array is tracked in a meta entry "name[_len]".
 */

/* Check if a variable name represents an array element (contains []) */
static int is_array_element(const char *name)
{
    while (*name) {
        if (*name == '[') return 1;
        name++;
    }
    return 0;
}

/* Extract array name and index from "name[idx]" format.
 * Returns -1 if not an array access. */
static int parse_array_ref(const char *name, char *array_name, int name_size)
{
    const char *p = name;
    int ni = 0;
    while (*p && *p != '[' && ni < name_size - 1)
        array_name[ni++] = *p++;
    array_name[ni] = '\0';
    if (*p != '[') return -1;
    p++; /* skip '[' */
    int idx = 0;
    while (*p >= '0' && *p <= '9') {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (*p != ']') return -1;
    return idx;
}

/* Set an array element: name[idx] = value */
static void shell_var_set_array(const char *name, int idx, const char *value)
{
    char elem_name[MAX_VAR_NAME];
    int n = snprintf(elem_name, sizeof(elem_name), "%s[%d]", name, idx);
    if (n < 0 || n >= (int)sizeof(elem_name)) return;
    shell_var_set(elem_name, value);
}

/* Get an array element: value = name[idx] */
static const char *shell_var_get_array(const char *name, int idx)
{
    char elem_name[MAX_VAR_NAME];
    snprintf(elem_name, sizeof(elem_name), "%s[%d]", name, idx);
    return shell_var_get(elem_name);
}

/* ── Core variable functions ────────────────────────────────────────── */

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

/* ── Array-specific API ────────────────────────────────────────────── */

/* Set an array element by array name + index.
 * If value is NULL, allocate default empty string. */
void shell_var_array_set(const char *name, int idx, const char *value)
{
    shell_var_set_array(name, idx, value ? value : "");
}

/* Get an array element by array name + index.
 * Returns "" if not set. */
const char *shell_var_array_get(const char *name, int idx)
{
    const char *v = shell_var_get_array(name, idx);
    return v ? v : "";
}

/* Get the number of elements set in an array (up to MAX_ARRAY_ITEMS). */
int shell_var_array_len(const char *name)
{
    int count = 0;
    for (int i = 0; i < MAX_ARRAY_ITEMS; i++) {
        char elem_name[MAX_VAR_NAME];
        snprintf(elem_name, sizeof(elem_name), "%s[%d]", name, i);
        const char *v = shell_var_get(elem_name);
        if (v && *v) count++;
    }
    return count;
}

/* ── Variable expansion helpers ────────────────────────────────────── */

/* shell_var_expand: Expand ${name}, ${name[idx]}, ${name:-default},
 * ${name:+alt}, and ${#name} syntax. Returns -1 on malformed input, 0 on
 * success. 'dst' is written up to 'dst_size' bytes (including NUL). */
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
                if (*src == '}') src++;

                int slen = 0;
                /* Check if it's an array length query */
                char array_base[MAX_VAR_NAME];
                int idx;
                int is_array = 0;
                if ((idx = parse_array_ref(name, array_base, MAX_VAR_NAME)) >= 0) {
                    is_array = 1;
                }

                if (is_array) {
                    slen = shell_var_array_len(array_base);
                } else {
                    const char *val = shell_var_get(name);
                    slen = (val && *val) ? (int)strlen(val) : 0;
                }

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

            /* Parse variable name, possibly with array index */
            char name[MAX_VAR_NAME];
            int ni = 0;
            while (*src && *src != ':' && *src != '}' && *src != '[' && ni < MAX_VAR_NAME - 1)
                name[ni++] = *src++;
            name[ni] = '\0';

            /* Handle array access: name[idx] */
            char array_base[MAX_VAR_NAME];
            int array_idx = -1;
            if (*src == '[') {
                src++; /* skip '[' */
                int idx = 0;
                while (*src >= '0' && *src <= '9') {
                    idx = idx * 10 + (*src - '0');
                    src++;
                }
                if (*src == ']') {
                    src++; /* skip ']' */
                    array_idx = idx;
                    memcpy(array_base, name, MAX_VAR_NAME);
                    /* Re-read the full array name for the rest */
                    /* name currently has the base name, rebuild */
                }
            }

            /* Handle ${name[idx]} — simple get */
            if (*src == '}') {
                src++;
                const char *val;
                if (array_idx >= 0) {
                    val = shell_var_array_get(array_base, array_idx);
                } else {
                    val = shell_var_get(name);
                }
                while (*val && di < dst_size - 1)
                    dst[di++] = *val++;
                continue;
            }

            /* ${name:-default} or ${name:+alt} */
            if (*src == ':') {
                src++;
                char op = *src; /* '-' or '+' */
                src++;
                char defval[128];
                int dvi = 0;
                while (*src && *src != '}' && dvi < 127)
                    defval[dvi++] = *src++;
                defval[dvi] = '\0';
                if (*src == '}') src++;

                const char *val;
                if (array_idx >= 0) {
                    val = shell_var_array_get(array_base, array_idx);
                } else {
                    val = shell_var_get(name);
                }

                const char *result;
                if (op == '-') {
                    /* Use default if unset or empty */
                    result = (val && *val) ? val : defval;
                } else { /* '+' */
                    /* Use alternative if set and non-empty */
                    result = (val && *val) ? defval : "";
                }
                while (*result && di < dst_size - 1)
                    dst[di++] = *result++;
                continue;
            }

            /* Shouldn't reach here */
            continue;
        } else {
            dst[di++] = *src++;
        }
    }

    dst[di] = '\0';
    return 0;
}
