/* cmd_dirs.c — directory stack builtins: dirs, pushd, popd */
#include "shell_cmds.h"
#include "shell.h"
#include "printf.h"
#include "string.h"

#define DIR_STACK_MAX 32

static char dir_stack[DIR_STACK_MAX][256];
static int  dir_stack_depth = 0;

void dir_stack_init(void)
{
    const char *cwd = shell_var_get("PWD");
    if (cwd && *cwd) {
        strncpy(dir_stack[0], cwd, 255);
        dir_stack[0][255] = '\0';
        dir_stack_depth = 1;
    }
}

void cmd_dirs(const char *args) {
    (void)args;
    const char *home = shell_var_get("HOME");
    int home_len = home ? (int)strlen(home) : 0;

    for (int i = 0; i < dir_stack_depth; i++) {
        const char *dir = dir_stack[i];
        if (home_len > 0 && strncmp(dir, home, (size_t)home_len) == 0 &&
            (dir[home_len] == '\0' || dir[home_len] == '/')) {
            kprintf("~%s", dir + home_len);
        } else {
            kprintf("%s", dir);
        }
        if (i < dir_stack_depth - 1) kprintf(" ");
    }
    kprintf("\n");
}

void cmd_pushd(const char *args) {
    if (!args || !*args) {
        if (dir_stack_depth < 2) {
            kprintf("pushd: no other directory\n");
            return;
        }
        char tmp[256];
        memcpy(tmp, dir_stack[0], 256);
        memcpy(dir_stack[0], dir_stack[1], 256);
        memcpy(dir_stack[1], tmp, 256);
        shell_var_set("PWD", dir_stack[0]);
        cmd_dirs(NULL);
        return;
    }

    if (dir_stack_depth >= DIR_STACK_MAX) {
        kprintf("pushd: directory stack full\n");
        return;
    }

    for (int i = dir_stack_depth; i > 0; i--)
        memcpy(dir_stack[i], dir_stack[i - 1], 256);
    dir_stack_depth++;

    strncpy(dir_stack[0], args, 255);
    dir_stack[0][255] = '\0';
    shell_var_set("PWD", dir_stack[0]);
    cmd_dirs(NULL);
}

void cmd_popd(const char *args) {
    (void)args;
    if (dir_stack_depth <= 1) {
        kprintf("popd: directory stack empty\n");
        return;
    }

    for (int i = 0; i < dir_stack_depth - 1; i++)
        memcpy(dir_stack[i], dir_stack[i + 1], 256);
    dir_stack_depth--;
    shell_var_set("PWD", dir_stack[0]);
    cmd_dirs(NULL);
}
