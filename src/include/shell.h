#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_run(void);
void shell_exec_cmd(const char *cmd, const char *args);
void shell_history_add(const char *cmd_line);
void shell_history_show_entries(void);

#endif
