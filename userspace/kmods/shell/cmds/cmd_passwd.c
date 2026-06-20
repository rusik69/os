/* cmd_passwd.c — passwd: change user password
 *
 * Usage:
 *   passwd              — change current user's password
 *   passwd username     — change specified user's password (root only)
 *
 * When a regular user runs passwd, they must provide their current
 * password for authentication, then enter the new password twice.
 * Root may change any user's password without providing the old one.
 *
 * The actual password hashing is performed by the kernel's user
 * management subsystem (djb2 hash via user_passwd()).  This command
 * is the interactive front-end.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

/* ── Read a single line of input (for password prompts) ────────── */
static void read_password(char *buf, int max)
{
    libc_shell_read_line(buf, max);
}

void cmd_passwd(const char *args)
{
    char target_user[USER_MAX_NAME];
    struct libc_user_entry ue;
    struct libc_user_session *cur = libc_session_get();
    int is_root = (cur && cur->uid == 0);

    /* ── Parse optional username argument ──────────────────────── */
    target_user[0] = '\0';
    if (args) {
        const char *p = args;
        while (*p == ' ') p++;
        if (*p) {
            int i = 0;
            while (*p && *p != ' ' && i < (int)sizeof(target_user) - 1)
                target_user[i++] = *p++;
            target_user[i] = '\0';
        }
    }

    /* Default to current user if no argument given */
    if (!target_user[0]) {
        if (!cur || !cur->logged_in) {
            kprintf("passwd: not logged in\n");
            return;
        }
        memcpy(target_user, cur->username, sizeof(target_user));
    }

    /* ── Target user must exist ────────────────────────────────── */
    if (libc_user_find(target_user, &ue) != 0) {
        kprintf("passwd: unknown user '%s'\n", target_user);
        return;
    }

    /* ── Permission check ──────────────────────────────────────── */
    if (!is_root) {
        /* Non-root can only change their own password */
        if (!cur || strcmp(cur->username, target_user) != 0) {
            kprintf("passwd: only root may change another user's password\n");
            return;
        }

        /* Authenticate by asking for the current password */
        char current_pw[USER_MAX_PASS];
        kprintf("Current password: ");
        read_password(current_pw, (int)sizeof(current_pw));
        kprintf("\n");

        if (libc_session_login(target_user, current_pw) != 0) {
            kprintf("passwd: incorrect password\n");
            return;
        }
    }

    /* ── Prompt for new password (twice for confirmation) ──────── */
    char new_pw1[USER_MAX_PASS];
    char new_pw2[USER_MAX_PASS];

    kprintf("New password: ");
    read_password(new_pw1, (int)sizeof(new_pw1));
    kprintf("\n");

    kprintf("Retype new password: ");
    read_password(new_pw2, (int)sizeof(new_pw2));
    kprintf("\n");

    /* Validate: passwords must match and be non-empty */
    if (strcmp(new_pw1, new_pw2) != 0) {
        kprintf("passwd: passwords do not match\n");
        return;
    }

    if (!new_pw1[0]) {
        kprintf("passwd: password cannot be empty\n");
        return;
    }

    /* ── Update the password ───────────────────────────────────── */
    int ret = libc_user_passwd(target_user, new_pw1);
    if (ret == 0) {
        kprintf("passwd: password changed successfully for '%s'\n", target_user);
    } else {
        kprintf("passwd: failed to change password (err=%d)\n", ret);
    }
}
