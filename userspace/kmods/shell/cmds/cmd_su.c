/* cmd_su.c — su: substitute user identity
 *
 * Usage:
 *   su           — switch to root (prompts for password)
 *   su root      — same as above
 *   su -         — switch to root with login shell (loads profile)
 *   su - root    — same as above
 *   su username  — switch to specified user (prompts for their password)
 *
 * When invoked by root, no password is required to switch to any user.
 * When a regular user runs su, the target user's password is required.
 *
 * The "-" (login shell) flag changes to the target user's home directory
 * and sets HOME/PWD/SHELL/USER/LOGNAME environment variables appropriately.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "shell.h"       /* for shell_var_get */
#include "string.h"
#include "printf.h"

/* ── Forward declarations for helpers used below ─────────────────── */
extern void cmd_cd(const char *args);

/* ── Read a single line from input (for password prompt) ──────────── */
static void read_password(char *buf, int max)
{
    libc_shell_read_line(buf, max);
}

/* ── Type alias: use the libc user entry typedef from libc.h ─────── */
#define ue_t libc_user_entry

void cmd_su(const char *args)
{
    char username[32];
    struct ue_t ue;
    int is_login_shell = 0;

    username[0] = '\0';

    /* ── Parse arguments ──────────────────────────────────────────── */
    const char *p = args;
    if (p) {
        while (*p == ' ') p++;

        if (*p == '-') {
            is_login_shell = 1;
            p++;
            while (*p == ' ') p++;
        }

        if (*p) {
            int ui = 0;
            while (*p && *p != ' ' && ui < (int)sizeof(username) - 1)
                username[ui++] = *p++;
            username[ui] = '\0';
        }
    }

    /* Default to root if no username specified */
    if (!username[0]) {
        memcpy(username, "root", 5);
    }

    /* ── Target user must exist ──────────────────────────────────── */
    if (libc_user_find(username, &ue) != 0) {
        kprintf("su: unknown user '%s'\n", username);
        return;
    }

    struct libc_user_session *cur = libc_session_get();
    int is_root     = (cur && cur->uid == 0);
    int is_same_user = (cur && strcmp(cur->username, username) == 0);

    /* Already this user without login-shell flag → nothing to do */
    if (is_same_user && !is_login_shell) {
        kprintf("su: already %s\n", username);
        return;
    }

    /* ── Authentication ──────────────────────────────────────────── */
    if (!is_root) {
        /* Non-root must provide the target user's password */
        char password[64];
        kprintf("Password: ");
        read_password(password, (int)sizeof(password));
        kprintf("\n");

        int rc = libc_session_login(username, password);
        if (rc != 0) {
            if (rc == -1)
                kprintf("su: user '%s' not found\n", username);
            else
                kprintf("su: incorrect password\n");
            return;
        }
    } else if (!is_same_user) {
        /* Root switches to any user without password.
         * We directly set the session fields since root has authority. */
        struct libc_user_session *s = libc_session_get();
        if (s) {
            s->logged_in = 1;
            s->uid       = ue.uid;
            s->gid       = ue.gid;
            memcpy(s->username, ue.username, sizeof(s->username));
        }
    }

    /* ── Set environment variables ───────────────────────────────── */
    libc_shell_var_set("USER",   ue.username);
    libc_shell_var_set("LOGNAME", ue.username);
    libc_shell_var_set("SHELL",  "/bin/sh");

    if (is_login_shell) {
        /* Login shell: cd to home, set HOME, PWD */
        const char *home = ue.home[0] ? ue.home : "/";
        libc_shell_var_set("HOME", home);
        libc_shell_var_set("PWD",  home);
        cmd_cd(home);
    } else {
        /* Non-login shell: just set HOME from user entry */
        const char *home = ue.home[0] ? ue.home : "/";
        libc_shell_var_set("HOME", home);
    }

    /* ── Feedback (show new prompt status line) ──────────────────── */
    const char *host = shell_var_get("HOSTNAME");
    const char *cwd  = shell_var_get("PWD");
    if (!host || !host[0]) host = "localhost";
    if (!cwd  || !cwd[0])  cwd  = "/";
    kprintf("[%s@%s %s]\n", ue.username, host, cwd);
}
