#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_dir(const char *args) {
    const char *path = args ? args : ".";
    kprintf("Directory: %s\n", path);
}
