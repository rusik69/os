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
