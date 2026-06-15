#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_uname(void) {
    kprintf("OS x86_64 GNU/OS (%s %s)\n", __DATE__, __TIME__);
}
