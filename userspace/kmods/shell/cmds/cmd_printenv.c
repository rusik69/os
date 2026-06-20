/* cmd_printenv.c — print all shell environment variables */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/* shell_var API — declared in shell.h, implemented in shell_vars.c */
extern int      shell_var_count(void);
extern const char *shell_var_name(int i);
extern const char *shell_var_value(int i);

/* Export check */
extern int shell_var_is_exported(const char *name);

void cmd_printenv(const char *args) {
    (void)args;

    int count = shell_var_count();
    int printed = 0;

    for (int i = 0; i < count; i++) {
        const char *name  = shell_var_name(i);
        const char *value = shell_var_value(i);

        if (!name || !*name) continue;
        if (!value || !*value) continue;

        kprintf("%s=%s\n", name, value);
        printed++;
    }

    if (printed == 0) {
        kprintf("(no environment variables set)\n");
    }
}
