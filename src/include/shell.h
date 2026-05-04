#ifndef SHELL_H
#define SHELL_H

#define HISTORY_SIZE 16

void shell_init(void);
void shell_run(void);
void shell_exec_cmd(const char *cmd, const char *args);
void shell_history_add(const char *cmd_line);
void shell_history_show_entries(void);
int         shell_history_count(void);
const char *shell_history_entry(int idx);
void shell_tab_complete_telnet(char *buf, int *len, void *session);

#endif
