#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_diff3(const char *args) {
    if (!args) { kprintf("Usage: diff3 <file1> <file2> <file3>\n"); return; }
    kprintf("diff3: comparing '%s'\n", args);
}
