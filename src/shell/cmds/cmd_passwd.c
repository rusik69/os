/* cmd_passwd.c — passwd command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "keyboard.h"
#include "string.h"

void cmd_passwd(const char *args) {
    struct libc_user_session *session = libc_session_get();
    if (!session) {
        kprintf("Error: No active session\n");
        return;
    }

    char target_user[USER_MAX_NAME];
    if (args && *args) {
        strncpy(target_user, args, USER_MAX_NAME - 1);
        target_user[USER_MAX_NAME - 1] = '\0';
    } else {
        strncpy(target_user, session->username, USER_MAX_NAME - 1);
        target_user[USER_MAX_NAME - 1] = '\0';
    }

    /* Authorization check */
    if (!libc_session_is_root()) {
        if (strcmp(target_user, session->username) != 0) {
            kprintf("Error: You can only change your own password\n");
            return;
        }
    }

    /* Get new password */
    char new_pass[USER_MAX_PASS];
    int len = 0;
    kprintf("Enter new password for %s: ", target_user);

    /* Simple password input (no masking for now, as it's hard with kprintf) */
    while (1) {
        char c = keyboard_getchar();
        if (c == '\n') {
            new_pass[len] = '\0';
            break;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                kprintf("\b \b");
            }
        } else if (len < USER_MAX_PASS - 1) {
            new_pass[len++] = c;
            kprintf("*");
        }
    }
    kprintf("\n");

    if (libc_user_passwd(target_user, new_pass) == 0) {
        kprintf("Password updated successfully for %s\n", target_user);
    } else {
        kprintf("Error updating password for %s\n", target_user);
    }
}
