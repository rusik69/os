/* cmd_pinky.c — Lightweight finger: print user information */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_pinky(const char *args) {
    (void)args;

    /* List users from the user table */
    int n = libc_users_count();
    if (n == 0) {
        kprintf("No users.\n");
        return;
    }

    kprintf("Login    Name             Tty      Idle   When\n");
    kprintf("──────────────────────────────────────────────────\n");
    for (int i = 0; i < n; i++) {
        struct libc_user_entry entry;
        if (libc_users_get_by_index(i, &entry) == 0 && entry.active) {
            kprintf("%-8s %-16s console   -      -\n",
                    entry.username, entry.username);
        }
    }
}
