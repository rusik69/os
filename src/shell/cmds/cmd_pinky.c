#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_pinky(void) {
    kprintf("Login: root     Name: Superuser\n");
    kprintf("Directory: /root  Shell: /bin/sh\n");
    kprintf("On since 17:08\n");
}
