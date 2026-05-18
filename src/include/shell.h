#ifndef SHELL_H
#define SHELL_H

#define HISTORY_SIZE 16

void shell_init(void);
void shell_run(void);
void shell_exec_cmd(const char *cmd, const char *args);
void shell_process_line(const char *line);
void shell_set_exit_status(int s);
int  shell_get_exit_status(void);
void shell_history_add(const char *cmd_line);
void shell_history_show_entries(void);
int         shell_history_count(void);
const char *shell_history_entry(int idx);
void shell_tab_complete_telnet(char *buf, int *len, void *session);
void shell_read_line(char *buf, int max);

/* Shell variable API */
void        shell_var_set(const char *name, const char *value);
const char *shell_var_get(const char *name);
int         shell_var_count(void);
const char *shell_var_name(int i);
const char *shell_var_value(int i);

/* Shell alias API */
void        shell_alias_set(const char *name, const char *value);
void        shell_alias_del(const char *name);
const char *shell_alias_get(const char *name);
void        shell_alias_list(void);

/* Current working directory API */
const char *shell_cwd_get(void);
int         shell_cwd_set(const char *path);  /* 0=ok, -1=not a dir */

#endif
