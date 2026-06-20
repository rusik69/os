#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"

/* libc_pmm_get_stats and libc_pci_list are provided by the kernel's libc
 * syscall interface (declared in libc.h, but we avoid including that here
 * because libc.h has conflicting static-inline definitions for shell
 * functions already declared in shell.h). */
struct libc_pmm_stats {
    uint32_t total_pages;
    uint32_t used_pages;
    uint32_t free_pages;
};
extern int libc_pmm_get_stats(struct libc_pmm_stats *out);
extern void libc_pci_list(void);

void cmd_hwinfo(void) {
    struct libc_pmm_stats mem;
    if (libc_pmm_get_stats(&mem) == 0) {
        kprintf("=== Hardware Info ===\n");
        kprintf("CPU: x86_64\n");
        kprintf("Memory: %u MB total, %u MB used, %u MB free\n",
                mem.total_pages * 4 / 1024,
                mem.used_pages * 4 / 1024,
                mem.free_pages * 4 / 1024);
    } else {
        kprintf("=== Hardware Info ===\n");
        kprintf("CPU: x86_64\n");
    }
    kprintf("PCI devices:\n");
    libc_pci_list();
}
