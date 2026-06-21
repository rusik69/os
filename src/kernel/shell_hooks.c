#include "shell.h"
#include "script.h"

void (*shell_process_line_ptr)(const char *line) = NULL;
void (*shell_history_add_ptr)(const char *cmd_line) = NULL;
int (*script_exec_ptr)(const char *path) = NULL;

void shell_register_hooks(void (*process_line)(const char *), void (*history_add)(const char *))
{
    shell_process_line_ptr = process_line;
    shell_history_add_ptr = history_add;
}

void shell_register_script_exec(int (*fn)(const char *))
{
    script_exec_ptr = fn;
}

/* ── Stub: shell_hook_register ─────────────────────────────── */
int shell_hook_register(const char *name, void *fn)
{
    (void)name;
    (void)fn;
    kprintf("[shell] shell_hook_register: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: shell_hook_unregister ─────────────────────────────── */
int shell_hook_unregister(const char *name)
{
    (void)name;
    kprintf("[shell] shell_hook_unregister: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: shell_hook_execute ─────────────────────────────── */
int shell_hook_execute(const char *cmd, int argc, char **argv)
{
    (void)cmd;
    (void)argc;
    (void)argv;
    kprintf("[shell] shell_hook_execute: not yet implemented\n");
    return -ENOSYS;
}
