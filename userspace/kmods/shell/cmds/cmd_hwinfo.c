#include "shell.h"
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

/* Read the CPU vendor string via CPUID leaf 0 (ebx:edx:ecx). */
static void cpu_vendor(char *out, int n) {
    uint32_t ebx = 0, ecx = 0, edx = 0;
    out[0] = '\0';
    __asm__ volatile("cpuid"
                     : "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    int i = 0;
    unsigned char *p = (unsigned char *)&ebx;
    for (int k = 0; k < 4 && i < n - 1; k++) out[i++] = (char)p[k];
    p = (unsigned char *)&edx;
    for (int k = 0; k < 4 && i < n - 1; k++) out[i++] = (char)p[k];
    p = (unsigned char *)&ecx;
    for (int k = 0; k < 4 && i < n - 1; k++) out[i++] = (char)p[k];
    out[i] = '\0';
}

void cmd_hwinfo(void) {
    struct libc_pmm_stats mem;
    char vendor[16];
    cpu_vendor(vendor, sizeof(vendor));

    kprintf("=== Hardware Info ===\n");
    kprintf("CPU vendor: %s\n", vendor[0] ? vendor : "unknown");
    if (libc_pmm_get_stats(&mem) == 0) {
        kprintf("Memory: %u MB total, %u MB used, %u MB free\n",
                mem.total_pages * 4 / 1024,
                mem.used_pages * 4 / 1024,
                mem.free_pages * 4 / 1024);
    }
    kprintf("PCI devices:\n");
    libc_pci_list();
}
