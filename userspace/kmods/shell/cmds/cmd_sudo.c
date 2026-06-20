/* cmd_sudo.c — sudo: execute a command as superuser (or another user)
 *
 * Usage:
 *   sudo <command> [args...]              — run command as root
 *   sudo -u <user> <command> [args...]    — run command as specified user
 *   sudo -E <command> [args...]           — preserve environment (in this
 *                                           shell, env is always preserved)
 *   sudo -l                               — list allowed commands
 *   sudo -k                               — kill the auth timestamp cache
 *
 * Authentication:
 *   Prompts for the CURRENT user's password (not the target user's).
 *   Once authenticated, the credential is cached for 5 minutes (per
 *   shell session) to avoid repeated prompting.
 *
 * Authorization (/etc/sudoers):
 *   Root (uid 0) can always use sudo without a password.
 *   Other users must be listed in /etc/sudoers.  The file format is:
 *
 *     # comment
 *     username ALL=(ALL) ALL          → full access
 *     username hostname=(user) cmd    → specific command as specific user
 *
 *   If /etc/sudoers cannot be read, a minimal built-in policy is used:
 *   the default shell user "user" is granted sudo access.
 *
 * ── Allowed headers for app sources: ─────────────────────────────
 * libc.h, shell_cmds.h, shell_cmd_table.h, shell.h, printf.h,
 * string.h, stdlib.h, types.h, keyboard.h, blockdev.h, fat32.h,
 * ata.h, ahci.h, service.h, fault.h, heap.h, syscall.h, vfs.h,
 * module.h, module_elf.h, ssh.h, ssh_client.h, sysctl.h, users.h
 */

#include "shell_cmds.h"
#include "libc.h"
#include "shell.h"
#include "string.h"
#include "printf.h"

/* ── Constants ─────────────────────────────────────────────────── */
#define SUDO_CACHE_TIMEOUT_SEC  300   /* 5 minutes */
#define SUDOERS_PATH           "/etc/sudoers"
#define SUDOERS_LINE_MAX       256

/* ── Auth timestamp cache ────────────────────────────────────────
 * Stores the last successful authentication time per (username, hostname).
 * Simple single-entry cache — sufficient for a single-user shell session.
 */
static struct {
    char    username[32];
    uint64_t auth_tick;   /* timer ticks at auth time (via libc_uptime_ticks) */
    int     valid;
} sudo_cache;

/* ── Clear the auth cache (sudo -k) ───────────────────────────── */
static void sudo_cache_clear(void) {
    sudo_cache.username[0] = '\0';
    sudo_cache.auth_tick   = 0;
    sudo_cache.valid       = 0;
}

/* ── Check if auth cache is still fresh for the given user ──────
 * Returns 1 if cache is valid and not expired, 0 otherwise.
 * libc_uptime_ticks() returns the system timer tick count (TIMER_FREQ=100). */
static int sudo_cache_valid(const char *username) {
    if (!sudo_cache.valid)
        return 0;
    if (strcmp(sudo_cache.username, username) != 0)
        return 0;
    uint64_t now = libc_uptime_ticks();
    uint64_t elapsed_ticks = (now >= sudo_cache.auth_tick)
                             ? (now - sudo_cache.auth_tick) : 0;
    /* TIMER_FREQ is 100 Hz, so SUDO_CACHE_TIMEOUT_SEC * 100 ticks */
    if (elapsed_ticks > (uint64_t)SUDO_CACHE_TIMEOUT_SEC * 100)
        return 0;
    return 1;
}

/* ── Update auth cache for the given user ─────────────────────── */
static void sudo_cache_set(const char *username) {
    memcpy(sudo_cache.username, username, sizeof(sudo_cache.username) - 1);
    sudo_cache.username[sizeof(sudo_cache.username) - 1] = '\0';
    sudo_cache.auth_tick = libc_uptime_ticks();
    sudo_cache.valid     = 1;
}

/* ── Simple token-based sudoers parser ──────────────────────────
 * Returns 1 if 'user' is authorized to run 'command' as 'target_user',
 * 0 otherwise.
 *
 * Supports basic sudoers syntax line by line:
 *   user hostname=(target) command
 *   ALL  ALL=(ALL) ALL        → wildcard access
 *   user  ALL=(ALL) ALL       → unconditional access for user
 *
 * Lines starting with '#' are ignored.  Empty lines are ignored.
 * If sudoers file is missing, built-in default: "user" has access.
 */
static int sudo_check_sudoers(const char *user, const char *target_user,
                               const char *command)
{
    /* Root is always authorized */
    if (strcmp(user, "root") == 0)
        return 1;

    /* Try to stat /etc/sudoers via libc (no direct vfs.h needed) */
    struct vfs_stat st;
    if (libc_vfs_stat(SUDOERS_PATH, &st) != 0) {
        /* No sudoers file — fall back to built-in policy:
         * the default "user" account has sudo rights. */
        if (strcmp(user, "user") == 0)
            return 1;
        return 0;
    }

    /* Read sudoers file */
    char buf[4096];
    uint32_t size = 0;
    if (libc_vfs_read(SUDOERS_PATH, buf, sizeof(buf) - 1, &size) != 0)
        return 0;
    buf[size] = '\0';

    /* Parse line by line using strtok_r */
    char *saveptr;
    char *line = strtok_r(buf, "\n\r", &saveptr);
    while (line) {
        /* Skip leading whitespace */
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || *line == '\0') {
            line = strtok_r(NULL, "\n\r", &saveptr);
            continue;
        }

        /* Duplicate the line so strtok_r doesn't corrupt the original */
        char line_buf[SUDOERS_LINE_MAX];
        strncpy(line_buf, line, sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';

        /* Parse tokens from the line */
        char *lp_save;
        char *tok_user   = strtok_r(line_buf, " \t", &lp_save);
        char *tok_host   = strtok_r(NULL, " \t", &lp_save);
        char *tok_target_spec = strtok_r(NULL, " \t", &lp_save);
        char *tok_cmd    = strtok_r(NULL, " \t", &lp_save);

        if (!tok_user || !tok_host || !tok_target_spec || !tok_cmd) {
            line = strtok_r(NULL, "\n\r", &saveptr);
            continue;
        }

        /* Parse target user from (target) syntax */
        char tok_target[64] = "";
        {
            const char *s = tok_target_spec;
            if (*s == '(') s++;
            int ti = 0;
            while (*s && *s != ')' && ti < (int)sizeof(tok_target) - 1)
                tok_target[ti++] = *s++;
            tok_target[ti] = '\0';
        }

        /* Check match */
        int user_match   = (strcmp(tok_user, user) == 0 ||
                            strcmp(tok_user, "ALL") == 0);
        int host_match   = (strcmp(tok_host, "ALL") == 0);
        int target_match = (strcmp(tok_target, target_user) == 0 ||
                            strcmp(tok_target, "ALL") == 0);
        int cmd_match    = (strcmp(tok_cmd, command) == 0 ||
                            strcmp(tok_cmd, "ALL") == 0);

        if (user_match && host_match && target_match && cmd_match)
            return 1;

        line = strtok_r(NULL, "\n\r", &saveptr);
    }

    return 0;
}

/* ── Main sudo command ────────────────────────────────────────── */
void cmd_sudo(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: sudo [-u <user>] [-E] [-k] [-l] <command> [args...]\n");
        kprintf("       sudo -k              (kill auth timestamp)\n");
        kprintf("       sudo -l              (list allowed commands)\n");
        return;
    }

    /* ── Parse arguments ──────────────────────────────────────── */
    char target_user[32] = "root";
    int  list_mode       = 0;
    int  kill_cache      = 0;
    char cmd_buf[256];
    const char *cmd_args = NULL;

    /* Copy args to a mutable buffer for tokenizing */
    char arg_buf[512];
    strncpy(arg_buf, args, sizeof(arg_buf) - 1);
    arg_buf[sizeof(arg_buf) - 1] = '\0';

    char *saveptr;
    char *tok_state = arg_buf;

    /* Parse flags until we hit the command */
    while (1) {
        char *tok = strtok_r(tok_state, " \t", &saveptr);
        tok_state = NULL;  /* subsequent calls continue from saveptr */
        if (!tok) break;

        if (tok[0] == '-') {
            /* Parse flag(s) */
            for (int fi = 1; tok[fi]; fi++) {
                char flag = tok[fi];
                switch (flag) {
                case 'u': {
                    /* -u <user> — consume next token */
                    char *next = strtok_r(NULL, " \t", &saveptr);
                    if (!next) {
                        kprintf("sudo: -u requires a username\n");
                        return;
                    }
                    strncpy(target_user, next, sizeof(target_user) - 1);
                    target_user[sizeof(target_user) - 1] = '\0';
                    break;
                }
                case 'E':
                    /* Preserve environment — in this shell we always do */
                    break;
                case 'k':
                    kill_cache = 1;
                    break;
                case 'l':
                    list_mode = 1;
                    break;
                default:
                    kprintf("sudo: unknown option '-%c'\n", flag);
                    return;
                }
            }
        } else {
            /* First non-flag token is the command */
            strncpy(cmd_buf, tok, sizeof(cmd_buf) - 1);
            cmd_buf[sizeof(cmd_buf) - 1] = '\0';

            /* Remaining tokens are arguments */
            char *rest = strtok_r(NULL, "", &saveptr);
            cmd_args = rest ? rest : NULL;
            break;
        }
    }

    /* Handle -k */
    if (kill_cache) {
        sudo_cache_clear();
        kprintf("sudo: authentication timestamp cleared\n");
        return;
    }

    /* Handle -l */
    if (list_mode) {
        struct libc_user_session *cur = libc_session_get();
        if (!cur) {
            kprintf("sudo: not logged in\n");
            return;
        }
        kprintf("User %s may run the following commands on this host:\n",
                cur->username);
        kprintf("    (ALL) ALL\n");
        return;
    }

    /* Validate command */
    if (!cmd_buf[0]) {
        kprintf("sudo: no command specified\n");
        return;
    }

    /* ── Get current session info ─────────────────────────────── */
    struct libc_user_session *cur = libc_session_get();
    if (!cur) {
        kprintf("sudo: not logged in\n");
        return;
    }

    const char *username = cur->username;
    int is_root = (cur->uid == 0);

    /* ── Authorization ────────────────────────────────────────── */
    if (!is_root) {
        /* Check if user is authorized for this command */
        if (!sudo_check_sudoers(username, target_user, cmd_buf)) {
            kprintf("sudo: user '%s' is not in the sudoers file for command '%s' as '%s'\n",
                    username, cmd_buf, target_user);
            return;
        }

        /* Check auth cache (re-prompt if expired or missing) */
        if (!sudo_cache_valid(username)) {
            /* Prompt for password */
            char password[64];
            kprintf("[sudo] password for %s: ", username);
            libc_shell_read_line(password, (int)sizeof(password));
            kprintf("\n");

            /* Verify password using libc authentication */
            int rc = libc_session_login(username, password);
            if (rc != 0) {
                kprintf("sudo: incorrect password\n");
                return;
            }

            /* Cache the successful auth */
            sudo_cache_set(username);
        }
    }

    /* ── Look up target user ──────────────────────────────────── */
    struct libc_user_entry target_ue;
    if (libc_user_find(target_user, &target_ue) != 0) {
        kprintf("sudo: unknown target user '%s'\n", target_user);
        return;
    }

    /* ── Save original session state ──────────────────────────── */
    uint16_t orig_uid = cur->uid;
    uint16_t orig_gid = cur->gid;
    char orig_username[32];
    memcpy(orig_username, cur->username, sizeof(orig_username));

    /* ── Execute command as target user ────────────────────────── */
    /* Temporarily switch session to the target user */
    cur->uid = target_ue.uid;
    cur->gid = target_ue.gid;
    memcpy(cur->username, target_ue.username, sizeof(cur->username));

    /* Update environment variables */
    libc_shell_var_set("USER",    target_ue.username);
    libc_shell_var_set("LOGNAME", target_ue.username);
    libc_shell_var_set("SUDO_USER", orig_username);
    libc_shell_var_set("SUDO_COMMAND", cmd_buf);
    libc_shell_var_set("SUDO", "1");

    /* Execute the command */
    libc_shell_exec_cmd(cmd_buf, cmd_args);

    /* ── Restore original session ─────────────────────────────── */
    /* Re-fetch session pointer (may have changed during exec) */
    cur = libc_session_get();
    if (cur) {
        cur->uid = orig_uid;
        cur->gid = orig_gid;
        memcpy(cur->username, orig_username, sizeof(cur->username));

        libc_shell_var_set("USER",    orig_username);
        libc_shell_var_set("LOGNAME", orig_username);
        libc_shell_var_set("SUDO_USER", "");
        libc_shell_var_set("SUDO", "");
    }
}
