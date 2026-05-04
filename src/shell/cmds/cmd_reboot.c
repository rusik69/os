/* cmd_reboot.c — reboot command */
#include "shell_cmds.h"
#include "printf.h"

void cmd_reboot(void) {
    kprintf("Rebooting...\n");
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0; int $0" : : "m"(null_idt));
}
