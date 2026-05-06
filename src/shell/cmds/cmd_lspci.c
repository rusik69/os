/* cmd_lspci.c — lspci command */
#include "shell_cmds.h"
#include "libc.h"

void cmd_lspci(void) {
    libc_pci_list();
}
