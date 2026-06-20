#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
void cmd_lspci(void) {
    extern void pci_list(void);
    pci_list();
}
