#include "shell_cmds.h"
#include "printf.h"

void cmd_cpufreq_info(const char *args)
{
    (void)args;
    kprintf("CPU Frequency Information:\n");
    kprintf("  Driver:         acpi-cpufreq\n");
    kprintf("  Governor:       performance\n");
    kprintf("  Available governors: performance, powersave, userspace, ondemand, conservative\n");
    kprintf("  Min frequency:  800 MHz\n");
    kprintf("  Max frequency:  3200 MHz\n");
    kprintf("  Current:        3200 MHz\n");
}
