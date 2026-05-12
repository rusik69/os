/* cmd_top.c — top command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "keyboard.h"

void cmd_top(void) {
    struct libc_process_info procs[PROCESS_MAX];
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };

    kprintf("Starting top. Press 'q' to quit.\n");
    libc_sleep_ticks(10); // Give user a moment to see the message

    while (1) {
        vga_clear();
        vga_set_cursor(0, 0);

        kprintf("OS Top - Press 'q' to quit\n");
        kprintf("--------------------------------------------------------------------------------\n");
        kprintf("%-6s %-6s %-10s %-8s %-10s %s\n", "PID", "PPID", "STATE", "MODE", "USER", "NAME");
        kprintf("--------------------------------------------------------------------------------\n");

        int n = libc_process_list(procs, PROCESS_MAX);
        for (int i = 0; i < n; i++) {
            uint8_t st = procs[i].state;
            if (st > 4) st = 0;

            kprintf("%-6u %-6u %-10s %-8s %-10s %s\n",
                    (uint64_t)procs[i].pid,
                    (uint64_t)procs[i].ppid,
                    state_names[st],
                    procs[i].is_user ? "user" : "kernel",
                    procs[i].is_user ? "user" : "root", // Simplification: user vs root
                    procs[i].name);
        }

        /* Check for quit command */
        if (keyboard_has_input()) {
            if (keyboard_getchar() == 'q') {
                break;
            }
        }

        libc_sleep_ticks(TIMER_FREQ / 2); // Update every 0.5 seconds
    }
    kprintf("\nExited top.\n");
}
