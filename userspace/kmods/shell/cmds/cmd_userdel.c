/*
 * cmd_userdel.c — Delete a system user account.
 *
 * Usage:  userdel [OPTIONS] <username>
 *
 * Options:
 *   -r     Remove the user's home directory and mailbox
 *
 * This command deletes a user from the system's user table.
 * Root privileges are required.
 *
 * Corresponds to the POSIX userdel utility (Item U28).
 */
#include "shell_cmds.h"
#include "libc.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

void cmd_userdel(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: userdel [-r] <username>\n");
        return;
    }

    /* Require root privileges */
    if (!session_is_root()) {
        kprintf("userdel: permission denied (root only)\n");
        return;
    }

    /* Parse arguments */
    int remove_home = 0;
    const char *p = args;

    /* Skip leading whitespace */
    while (*p == ' ') p++;

    /* Check for -r flag */
    if (*p == '-') {
        p++;
        if (*p == 'r') {
            remove_home = 1;
            p++;
        }
        /* Skip any remaining flag characters and whitespace */
        while (*p == 'r' || *p == ' ') p++;
    }

    /* Extract username (remainder of args, trimmed) */
    char username[USER_MAX_NAME];
    int i = 0;
    while (*p && *p != ' ' && i < USER_MAX_NAME - 1)
        username[i++] = *p++;
    username[i] = '\0';

    if (i == 0) {
        kprintf("userdel: no username specified\n");
        return;
    }

    /* Prevent deleting root */
    if (strcmp(username, "root") == 0) {
        kprintf("userdel: cannot delete root account\n");
        return;
    }

    /* Look up the user first to get home directory */
    struct libc_user_entry ue;
    int found = user_find(username, &ue);
    if (found != 0) {
        kprintf("userdel: user '%s' does not exist\n", username);
        return;
    }

    kprintf("userdel: removing user '%s' (uid=%lu)...\n",
            username, (unsigned long)ue.uid);

    /* Delete the user entry */
    int rc = libc_user_delete(username);
    if (rc != 0) {
        if (rc == -ENOENT)
            kprintf("userdel: user '%s' does not exist\n", username);
        else if (rc == -EPERM)
            kprintf("userdel: permission denied\n");
        else
            kprintf("userdel: failed to delete user '%s' (error %d)\n",
                    username, -rc);
        return;
    }

    /* Optionally remove home directory */
    if (remove_home && ue.home[0]) {
        char rm_path[USER_MAX_HOME];
        int n = snprintf(rm_path, sizeof(rm_path),
                         "rm -rf %s", ue.home);
        if (n > 0 && n < (int)sizeof(rm_path)) {
            kprintf("userdel: removing home directory '%s'...\n", ue.home);
            /* Execute rm recursively via shell command parser */
            /* We call cmd_rm with just the target path */
            cmd_rm(ue.home);
        }

        /* Also try to remove /var/mail/<username> if it exists */
        char mail_path[USER_MAX_HOME + 16];
        n = snprintf(mail_path, sizeof(mail_path),
                     "/var/mail/%s", username);
        if (n > 0 && n < (int)sizeof(mail_path)) {
            cmd_rm(mail_path);
        }
    }

    kprintf("userdel: user '%s' deleted\n", username);
}
