/* cmd_lspci.c — lspci command */
#include "shell_cmds.h"
#include "pci.h"

void cmd_lspci(void) {
    pci_list();
}
