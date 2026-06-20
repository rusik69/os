#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_hostid(void) {
    const char *hn = shell_var_get("HOSTNAME");
    if (!hn) hn = "localhost";
    unsigned long id = 0;
    for (int i = 0; hn[i]; i++) id = id * 31 + hn[i];
    kprintf("%08lx\n", id);
}
