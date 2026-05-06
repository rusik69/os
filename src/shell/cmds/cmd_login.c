#include "shell_cmds.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

/* Read a password from the shell without echoing (simple: just read a line) */
static void read_password(char *buf, int max) {
    /* On real hardware the VGA/telnet layer doesn't easily suppress echo;
     * we blank the echoed chars after the fact by printing backspace sequences.
     * For now, read normally — a production implementation would disable echo. */
    libc_shell_read_line(buf, max);
}

/* login [username] */
void cmd_login(const char *args) {
    char username[USER_MAX_NAME];
    char password[USER_MAX_PASS];
    struct user_entry ue;

    username[0] = '\0';
    password[0] = '\0';

    if (args && *args) {
        const char *p = args;
        int ui = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && ui < USER_MAX_NAME - 1) username[ui++] = *p++;
        username[ui] = '\0';
        while (*p == ' ') p++;
        if (*p) {
            strncpy(password, p, USER_MAX_PASS - 1);
            password[USER_MAX_PASS - 1] = '\0';
        }
    }

    if (!username[0]) {
        kprintf("Username: ");
        libc_shell_read_line(username, USER_MAX_NAME);
    }

    if (!password[0]) {
        kprintf("Password: ");
        read_password(password, USER_MAX_PASS);
    }

    int rc = session_login(username, password);
    if (rc == 0) {
        struct user_session *s = session_get();
        if (user_find(s->username, &ue) == 0) {
            libc_shell_var_set("HOME", ue.home);
            libc_shell_var_set("PWD", ue.home);
            kprintf("Welcome, %s (uid=%u, home=%s)\n",
                    s->username, (uint64_t)s->uid, ue.home);
        } else {
            kprintf("Welcome, %s (uid=%u)\n", s->username, (uint64_t)s->uid);
        }
    } else if (rc == -1) {
        kprintf("login: user '%s' not found\n", username);
    } else {
        kprintf("login: incorrect password\n");
    }
}

void cmd_logout(void) {
    struct user_session *s = session_get();
    kprintf("Goodbye, %s\n", s->username);
    session_logout();
}
