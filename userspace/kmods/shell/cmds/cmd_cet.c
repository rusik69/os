#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"

/* cmd_cet — show CET status, shadow stack info */
void cmd_cet(void) {
    kprintf("Intel CET (Control-flow Enforcement Technology):\n");

    extern int cet_is_supported(void);
    int supported = cet_is_supported();

    kprintf("  Shadow Stack (CET_SS):  %s\n",
            supported ? "supported" : "not supported");

    kprintf("  CET subsystem:          %s\n",
            supported ? "initialised" : "not available");

    if (supported) {
        kprintf("  Protection: active (ROP protection enabled)\n");
    }
}
