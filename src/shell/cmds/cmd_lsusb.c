#include "shell_cmds.h"
#include "libc.h"

void cmd_lsusb(void) {
    libc_usb_list();
}
