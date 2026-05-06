#include "shell_cmds.h"
#include "shell.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

/* useradd <username> [uid] */
void cmd_useradd(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: useradd <username> [uid]\n");
        return;
    }
    if (!session_is_root()) {
        kprintf("useradd: permission denied (root only)\n");
        return;
    }
    char username[USER_MAX_NAME];
    uint32_t uid = 1001;

    /* Parse username and optional uid */
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < USER_MAX_NAME - 1)
        username[i++] = *p++;
    username[i] = '\0';
    while (*p == ' ') p++;
    if (*p) {
        uid = 0;
        while (*p >= '0' && *p <= '9') uid = uid * 10 + (uint32_t)(*p++ - '0');
    } else {
        /* Auto-assign uid: find max+1 */
        struct user_entry *tbl = users_get_table();
        for (int j = 0; j < USER_MAX_ENTRIES; j++) {
            if (tbl[j].active && tbl[j].uid >= uid) uid = tbl[j].uid + 1;
        }
    }

    char pw[USER_MAX_PASS];
    kprintf("Initial password: ");
    shell_read_line(pw, USER_MAX_PASS);

    int rc = user_add(username, uid, pw);
    if (rc == 0)
        kprintf("User '%s' added (uid=%u)\n", username, (uint64_t)uid);
    else if (rc == -2)
        kprintf("useradd: user '%s' already exists\n", username);
    else if (rc == -4)
        kprintf("useradd: password must be non-empty\n");
    else if (rc == -5)
        kprintf("useradd: failed to create home directory\n");
    else
        kprintf("useradd: failed (%d)\n", (uint64_t)(-rc));
}

/* userdel <username> */
void cmd_userdel(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: userdel <username>\n");
        return;
    }
    if (!session_is_root()) {
        kprintf("userdel: permission denied (root only)\n");
        return;
    }
    int rc = user_delete(args);
    if (rc == 0)
        kprintf("User '%s' deleted\n", args);
    else if (rc == -1)
        kprintf("userdel: cannot delete root\n");
    else
        kprintf("userdel: user '%s' not found\n", args);
}

/* passwd [username] */
void cmd_passwd(const char *args) {
    struct user_session *s = session_get();
    const char *target = (args && *args) ? args : s->username;

    /* Non-root can only change their own password */
    if (!session_is_root() && strcmp(target, s->username) != 0) {
        kprintf("passwd: permission denied\n");
        return;
    }

    kprintf("New password: ");
    char pw[USER_MAX_PASS];
    shell_read_line(pw, USER_MAX_PASS);

    int rc = user_passwd(target, pw);
    if (rc == 0)
        kprintf("Password updated for '%s'\n", target);
    else if (rc == -2)
        kprintf("passwd: password must be non-empty\n");
    else
        kprintf("passwd: user '%s' not found\n", target);
}

/* users - list all users */
void cmd_users(void) {
    kprintf("UID   GID   USERNAME         HOME\n");
    struct user_entry *tbl = users_get_table();
    for (int i = 0; i < USER_MAX_ENTRIES; i++) {
        if (!tbl[i].active) continue;
        kprintf("%-5u %-5u %-16s %s\n",
                (uint64_t)tbl[i].uid,
                (uint64_t)tbl[i].gid,
                tbl[i].username,
                tbl[i].home);
    }
}
