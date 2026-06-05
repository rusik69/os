#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_zforce(const char *args) {
    if (!args) { kprintf("Usage: zforce <file>\n"); return; }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", args);
    char *dot = strstr(buf, ".");
    if (!dot) { strcat(buf, ".gz"); kprintf("zforce: renamed to %s\n", buf); }
    else kprintf("zforce: %s already has extension\n", buf);
}
