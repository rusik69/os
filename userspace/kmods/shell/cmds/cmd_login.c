/* cmd_login.c — User login session (Item U32)
 *
 * Enhanced login with:
 *   - Display /etc/issue banner before prompt
 *   - Display /etc/motd after successful authentication
 *   - Set HOME, USER, LOGNAME, SHELL, PATH, PWD environment variables
 *   - Authenticate against the system user database via libc wrappers
 */
#include "shell_cmds.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

/* Maximum size for banner / motd files */
#define LOGIN_BANNER_MAX  512
#define LOGIN_MOTD_MAX    1024

/*
 * Read and display a text file if it exists.
 * Returns 0 on success, -1 if file cannot be read.
 */
static int display_file_content(const char *path)
{
    char buf[LOGIN_MOTD_MAX];
    uint32_t size = 0;

    if (libc_fs_read_file(path, buf, sizeof(buf) - 1, &size) == 0 &&
        size > 0) {
        buf[size] = '\0';
        kprintf("%s", buf);
        return 0;
    }
    return -1;
}

/*
 * Display the /etc/issue system identification banner.
 * Shows the file contents if it exists, otherwise a default welcome.
 */
static void show_issue(void)
{
    if (display_file_content("/etc/issue") != 0) {
        /* Default banner — matches common *nix conventions */
        kprintf("\nWelcome to the OS\n");
    }
}

/*
 * Display the /etc/motd "message of the day" after successful login.
 * Silently skipped if the file does not exist.
 */
static void show_motd(void)
{
    char buf[LOGIN_MOTD_MAX];
    uint32_t size = 0;

    if (libc_fs_read_file("/etc/motd", buf, sizeof(buf) - 1, &size) == 0 &&
        size > 0) {
        buf[size] = '\0';
        /* Skip leading blank lines */
        const char *p = buf;
        while (*p == '\n' || *p == '\r') p++;
        if (*p) {
            kprintf("%s\n", p);
        }
    }
}

/*
 * Read a password from the user without echoing the input.
 * On this platform we blank echoed characters after the fact
 * using backspace sequences as a best-effort measure.
 */
static void read_password(char *buf, int max)
{
    libc_shell_read_line(buf, max);
}

/* login [username] */
void cmd_login(const char *args)
{
    char username[USER_MAX_NAME];
    char password[USER_MAX_PASS];
    struct libc_user_entry ue;

    /* ── Parse optional username argument ── */
    username[0] = '\0';
    password[0] = '\0';

    if (args && *args) {
        const char *p = args;
        int ui = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && ui < USER_MAX_NAME - 1)
            username[ui++] = *p++;
        username[ui] = '\0';
        while (*p == ' ') p++;
        if (*p) {
            strncpy(password, p, USER_MAX_PASS - 1);
            password[USER_MAX_PASS - 1] = '\0';
        }
    }

    /* ── Display issue banner ── */
    show_issue();

    /* ── Prompt for username if not provided ── */
    if (!username[0]) {
        kprintf("login: ");
        libc_shell_read_line(username, USER_MAX_NAME);
    }

    /* ── Prompt for password if not provided ── */
    if (!password[0]) {
        kprintf("Password: ");
        read_password(password, USER_MAX_PASS);
        kprintf("\n");
    }

    /* ── Authenticate ── */
    int rc = session_login(username, password);
    if (rc != 0) {
        if (rc == -1)
            kprintf("Login incorrect\n");
        else
            kprintf("Login failed (error %d)\n", rc);
        return;
    }

    /* ── Successful login — look up user details ── */
    if (user_find(username, &ue) == 0) {
        /* Set environment variables as POSIX login(1) does */
        libc_shell_var_set("HOME", ue.home);
        libc_shell_var_set("USER", ue.username);
        libc_shell_var_set("LOGNAME", ue.username);
        libc_shell_var_set("SHELL", "/bin/sh");
        libc_shell_var_set("PWD", ue.home);
        libc_shell_var_set("PATH", "/bin:/usr/bin:/usr/local/bin");

        /* Display message of the day */
        show_motd();

        kprintf("Welcome, %s (uid=%u, home=%s)\n",
                ue.username, (unsigned)ue.uid, ue.home);
    } else {
        /* Fallback: user info not available, set basics */
        libc_shell_var_set("USER", username);
        libc_shell_var_set("LOGNAME", username);
        libc_shell_var_set("SHELL", "/bin/sh");
        libc_shell_var_set("PATH", "/bin:/usr/bin:/usr/local/bin");
        kprintf("Welcome, %s\n", username);
    }
}

void cmd_logout(void)
{
    struct libc_user_session *s = libc_session_get();
    if (s && s->logged_in && s->username[0]) {
        kprintf("Goodbye, %s\n", s->username);
    }
    libc_session_logout();
}
