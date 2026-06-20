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
