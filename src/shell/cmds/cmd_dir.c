#include "shell.h"
#include "shell_cmds.h"
#include "string.h"    /* strlen */
#include "shell_cmds.h"
void cmd_dir(const char *args) {
    if (!args || strlen(args) == 0) cmd_ls(".");
    else cmd_ls(args);
}
