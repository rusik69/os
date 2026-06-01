#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
void cmd_hwinfo(void) {
    
    kprintf("=== Hardware Info ===\n");
    kprintf("CPU: x86_64\n");
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    kprintf("Memory: %llu MB total, %llu MB used\n", total * 4 / 1024, used * 4 / 1024);
    kprintf("PCI devices:\n");
    pci_list();
}
