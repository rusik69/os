/* cmd_env.c — env command: list shell environment variables */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

/* shell_var API — declared in shell.h, implemented in shell_vars.c */
extern int      shell_var_count(void);
extern const char *shell_var_name(int i);
extern const char *shell_var_value(int i);

/* Export flag tracking: which variables are marked for export to child
 * processes.  Variables without the export flag are still shell variables
 * but are not inherited by programs launched from the shell. */
#define MAX_EXPORTED 32
static char exported_names[MAX_EXPORTED][32];
static int  exported_count = 0;

/* Mark a variable as exported */
void shell_var_export(const char *name) {
    if (!name || !*name) return;
    /* Check if already exported */
    for (int i = 0; i < exported_count; i++) {
        if (strcmp(exported_names[i], name) == 0)
            return;
    }
    if (exported_count < MAX_EXPORTED) {
        strncpy(exported_names[exported_count], name, 31);
        exported_names[exported_count][31] = '\0';
        exported_count++;
    }
}

/* Remove a variable's export flag */
void shell_var_export_no(const char *name) {
    if (!name || !*name) return;
    for (int i = 0; i < exported_count; i++) {
        if (strcmp(exported_names[i], name) == 0) {
            /* Shift remaining entries down */
            for (int j = i; j < exported_count - 1; j++)
                strncpy(exported_names[j], exported_names[j + 1], 31);
            exported_count--;
            return;
        }
    }
}

/* Check if a variable is exported */
int shell_var_is_exported(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < exported_count; i++) {
        if (strcmp(exported_names[i], name) == 0)
            return 1;
    }
    return 0;
}

/* Remove a shell variable entirely (unset) */
int shell_var_unset(const char *name) {
    if (!name || !*name) return -1;
    int total = shell_var_count();
    for (int i = 0; i < total; i++) {
        if (strcmp(shell_var_name(i), name) == 0) {
            /* Clear the export flag first */
            shell_var_export_no(name);
            /* Clear variable by setting to empty string */
            /* We can't remove entries from shell_vars.c array easily,
             * so set it to empty marking it as "unset" */
            /* shell_var_set doesn't support deletion, but we can
             * work around by setting value to empty. Callers check
             * shell_var_get returning "" for missing vars. */
            /* Overwrite the variable with empty value */
            /* We use a direct approach: the shell_vars.c module uses
             * a static array. We clear it via set. */
            extern void shell_var_set(const char *name, const char *value);
            shell_var_set(name, "");
            return 0;
        }
    }
    return -1;
}

/* ── env command: list all exported environment variables ──────── */
void cmd_env(void) {
    int count = shell_var_count();

    if (count == 0) {
        kprintf("(no environment variables set)\n");
        return;
    }

    /* Show all shell variables, marking exported ones with "export" prefix */
    for (int i = 0; i < count; i++) {
        const char *name  = shell_var_name(i);
        const char *value = shell_var_value(i);

        if (!name || !*name) continue;

        /* Skip empty (unset) variables */
        if (value && *value) {
            if (shell_var_is_exported(name))
                kprintf("export ");
            kprintf("%s=%s\n", name, value);
        }
    }
}

/* ── export command: mark variable for export ──────────────────── */
void cmd_export(const char *args) {
    if (!args || !*args) {
        /* No args: list all exported variables with their values */
        for (int i = 0; i < exported_count; i++) {
            const char *val = NULL;
            /* Find value from shell vars */
            int total = shell_var_count();
            for (int j = 0; j < total; j++) {
                if (strcmp(shell_var_name(j), exported_names[i]) == 0) {
                    val = shell_var_value(j);
                    break;
                }
            }
            kprintf("export %s=%s\n", exported_names[i], val ? val : "");
        }
        return;
    }

    /* Parse "NAME=VALUE" or just "NAME" */
    const char *eq = strchr(args, '=');
    if (eq) {
        /* NAME=VALUE: set and export */
        char name[32];
        int nlen = (int)(eq - args);
        if (nlen > 31) nlen = 31;
        memcpy(name, args, nlen);
        name[nlen] = '\0';

        extern void shell_var_set(const char *name, const char *value);
        shell_var_set(name, eq + 1);
        shell_var_export(name);
    } else {
        /* Just NAME: export existing variable */
        shell_var_export(args);
    }
}

/* ── unset command: remove variable ────────────────────────────── */
void cmd_unset(const char *args) {
    if (!args || !*args) {
        kprintf("usage: unset <name>\n");
        return;
    }
    shell_var_unset(args);
}
