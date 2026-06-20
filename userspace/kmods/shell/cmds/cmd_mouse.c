#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_mouse_status(void) {
    kprintf("Mouse: PS/2 on IRQ 12\n");
}
