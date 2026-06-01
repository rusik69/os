/* cmd_pinky.c — lightweight finger utility */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"
#include "types.h"

void cmd_pinky(void) {
    struct libc_process_info info[32];
    int count = libc_process_list(info, 32);
    for (int i = 0; i < count; i++) {
        kprintf("%-8s %5u  %s\n", info[i].name, info[i].pid,
                info[i].state == 0 ? "running" : "sleeping");
    }
}
