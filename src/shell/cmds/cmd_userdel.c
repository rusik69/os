/* cmd_userdel.c — Delete a user from the system */

#include "shell_cmds.h"
#include "users.h"
#include "string.h"
#include "printf.h"

void cmd_userdel(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: userdel <username>\n");
        return;
    }

    /* Only root can delete users */
    if (!session_is_root()) {
        kprintf("userdel: permission denied (root only)\n");
        return;
    }

    /* Parse the username (take the first word only) */
    char username[USER_MAX_NAME];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < USER_MAX_NAME - 1)
        username[i++] = *p++;
    username[i] = '\0';

    if (i == 0) {
        kprintf("userdel: no username specified\n");
        return;
    }

    /* Protect root account */
    if (strcmp(username, "root") == 0) {
        kprintf("userdel: cannot delete root account\n");
        return;
    }

    int ret = user_delete(username);
    switch (ret) {
    case 0:
        kprintf("userdel: user '%s' deleted\n", username);
        break;
    case -1:
        kprintf("userdel: cannot delete root account\n");
        break;
    case -2:
        kprintf("userdel: user '%s' not found\n", username);
        break;
    default:
        kprintf("userdel: failed to delete user '%s' (error %d)\n",
                username, ret);
        break;
    }
}
