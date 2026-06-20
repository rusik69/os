#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

void cmd_devmem2(const char *args)
{
    unsigned long addr = 0;
    if (args && args[0]) {
        addr = strtoul(args, NULL, 0);
    }
    kprintf("devmem2: physical memory access\n");
    kprintf("  Usage: devmem2 <phys_addr> [width]\n");
    if (addr) {
        kprintf("  Reading 0x%lx...\n", addr);
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
        kprintf("  Value at 0x%lx: 0x%08x\n", addr, *p);
    }
}
