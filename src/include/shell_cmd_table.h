#ifndef SHELL_CMD_TABLE_H
#define SHELL_CMD_TABLE_H

typedef void (*shell_cmd_fn)(const char *args);

typedef struct {
    const char *name;
    const char *desc;
    shell_cmd_fn fn;
} shell_cmd_entry_t;

int shell_cmd_count(void);
const shell_cmd_entry_t *shell_cmd_entry(int idx);
const char *shell_cmd_lookup_desc(const char *name);
shell_cmd_fn shell_cmd_lookup_fn(const char *name);
int shell_cmd_exists(const char *name);

#endif
