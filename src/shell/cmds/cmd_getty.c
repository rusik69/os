/* cmd_getty.c — getty: serial console login daemon (Item U12)
 *
 * Getty opens the console, displays login/password prompts, authenticates
 * the user, and spawns a shell.  When the shell exits, getty re-prompts
 * (or exits, to let init respawn it via /etc/inittab with 'respawn').
 *
 * Usage:
 *   getty [baud] [line] [term]    — run login loop on console
 *   getty stop                     — stop the daemon
 *   getty status                   — show running state
 *
 * In /etc/inittab (respawn mode):
 *   co:23:respawn:/bin/getty 115200 console vt100
 *
 * Note: In the current kernel, serial I/O is always on COM1 (console).
 * The baud/line/term arguments are accepted for compatibility but the
 * implementation reads/writes stdin/stdout which is the serial console.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "stdlib.h"
#include "syscall.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define GETTY_MAX_ATTEMPTS     3       /* failed logins before giving up */
#define GETTY_MAX_USER         64      /* max username length */
#define GETTY_MAX_PASS         128     /* max password length */
#define GETTY_TIMEOUT_TICKS    500     /* poll timeout (5 sec at 100 Hz) */
#define GETTY_MAX_ARGS         16

/* Standard file descriptor numbers */
#define GETTY_STDIN_FD         0
#define GETTY_STDOUT_FD        1
#define GETTY_STDERR_FD        2

/* ── Global state ───────────────────────────────────────────────────── */

static volatile int getty_running = 0;
static int getty_pid = 0;

/* ── Forward declarations ───────────────────────────────────────────── */

static void getty_login_loop(void);

/* ── Internal helpers ───────────────────────────────────────────────── */

/*
 * Write a string to stdout (fd 1).
 */
static long put_str(const char *s)
{
    if (!s) return 0;
    size_t len = strlen(s);
    if (len == 0) return 0;
    return libc_syscall(SYS_WRITE, GETTY_STDOUT_FD,
                        (uint64_t)(uintptr_t)s, (uint64_t)len, 0, 0);
}

/*
 * Read a single character from stdin (fd 0) with timeout.
 * Returns the character, or -1 on timeout/error.
 * This is a non-blocking poll to allow timeout.
 */
static int get_char_timeout(void)
{
    char ch;
    uint64_t start = libc_syscall(SYS_UPTIME, 0, 0, 0, 0, 0);

    for (;;) {
        long n = libc_syscall(SYS_READ, GETTY_STDIN_FD,
                              (uint64_t)(uintptr_t)&ch, 1, 0, 0);
        if (n > 0)
            return (unsigned char)ch;

        /* Check timeout */
        uint64_t now = libc_syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
        if (now - start > GETTY_TIMEOUT_TICKS)
            return -1;

        /* Brief pause to avoid busy-waiting */
        for (volatile int i = 0; i < 10000; i++)
            __asm__ volatile("pause");
    }
}

/*
 * Read a line from stdin into buf (max-1 bytes max).
 * If echo is non-zero, characters are echoed back.
 * Handles backspace, strips trailing newline.
 * Returns the number of characters read.
 */
static int read_line(char *buf, int max, int echo)
{
    int pos = 0;

    for (;;) {
        int ch = get_char_timeout();
        if (ch < 0)
            break;  /* timeout */

        if (ch == '\r' || ch == '\n') {
            put_str("\r\n");
            break;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (pos > 0) {
                pos--;
                if (echo) put_str("\b \b");
            }
            continue;
        }

        if (ch >= 32 && ch < 127) {
            buf[pos++] = (char)ch;
            if (echo) {
                char obuf[2] = { (char)ch, '\0' };
                put_str(obuf);
            }
        }

        if (pos >= max - 1)
            break;
    }

    buf[pos] = '\0';
    return pos;
}

/*
 * Authenticate a user via the session login syscall.
 * Returns 0 on success, -1 on failure.
 */
static int getty_authenticate(const char *username, const char *password)
{
    if (!username || !*username || !password)
        return -1;

    long ret = libc_syscall(SYS_SESSION_LOGIN,
                            (uint64_t)(uintptr_t)username,
                            (uint64_t)(uintptr_t)password,
                            0, 0, 0);
    return (ret == 0) ? 0 : -1;
}

/*
 * Display the /etc/issue banner if it exists.
 */
static void show_issue(void)
{
    char buf[512];
    uint32_t out_size = 0;
    if (libc_fs_read_file("/etc/issue", buf, sizeof(buf) - 1, &out_size) == 0
        && out_size > 0) {
        buf[out_size] = '\0';
        put_str(buf);
    } else {
        put_str("\nWelcome to the OS\n\n");
    }
}

/*
 * Fork and exec a shell for the authenticated user.
 * In the current kernel, we fall back to a simple message and re-loop
 * since userspace exec is limited.  When full userspace ELF loading
 * is available, this will fork and exec /bin/sh with proper env.
 */
static void spawn_shell(const char *username)
{
    /* ── Set up environment ─────────────────────────────────────── */
    char home[64];
    strcpy(home, "/home/");
    strncat(home, username, sizeof(home) - strlen(home) - 1);

    char shell_env[12][64];
    int env_idx = 0;

    snprintf(shell_env[env_idx], sizeof(shell_env[env_idx]),
             "HOME=%s", home);  env_idx++;
    snprintf(shell_env[env_idx], sizeof(shell_env[env_idx]),
             "USER=%s", username);  env_idx++;
    snprintf(shell_env[env_idx], sizeof(shell_env[env_idx]),
             "LOGNAME=%s", username);  env_idx++;
    snprintf(shell_env[env_idx], sizeof(shell_env[env_idx]),
             "SHELL=/bin/sh");  env_idx++;
    snprintf(shell_env[env_idx], sizeof(shell_env[env_idx]),
             "TERM=vt100");  env_idx++;
    snprintf(shell_env[env_idx], sizeof(shell_env[env_idx]),
             "PATH=/bin:/usr/bin");  env_idx++;

    (void)shell_env;

    /* In the current architecture, we can't fork+exec a userspace shell
     * from a kernel command.  Log the successful login and loop back
     * to the login prompt.  When userspace ELF loading is extended to
     * support /bin/sh, this will be replaced with fork + execve. */
    put_str("\r\n");
    put_str("Login successful.  (Shell exec pending userspace support.)\r\n");

    /* Clear password from memory */
    /* Not needed here since we don't store passwords */
    (void)home;
}

/* ── Main getty login loop ──────────────────────────────────────────── */

/*
 * Run the login prompt loop on the console.
 * Loops forever, prompting and authenticating.
 */
static void getty_login_loop(void)
{
    char username[GETTY_MAX_USER];
    char password[GETTY_MAX_PASS];

    getty_running = 1;
    put_str("getty: started on console\r\n");

    for (int attempt = 0; attempt < GETTY_MAX_ATTEMPTS; ) {
        /* Show issue banner on first prompt of each session */
        if (attempt == 0)
            show_issue();

        /* ── Read username ──────────────────────────────────────── */
        put_str("login: ");
        int ulen = read_line(username, sizeof(username), 1);

        if (ulen <= 0) {
            /* Timeout or empty — retry prompt */
            continue;
        }

        /* ── Read password (no echo) ────────────────────────────── */
        put_str("Password: ");
        int plen = read_line(password, sizeof(password), 0);

        if (plen <= 0) {
            put_str("\n");
            continue;
        }

        /* ── Authenticate ───────────────────────────────────────── */
        if (getty_authenticate(username, password) == 0) {
            /* Login successful */
            attempt = 0;  /* reset counter */

            /* Wipe password */
            for (int i = 0; i < GETTY_MAX_PASS; i++)
                password[i] = 0;

            /* Spawn shell (loops back on exit) */
            spawn_shell(username);

            /* spawn_shell returns when shell exits — re-prompt */
            put_str("\r\n");
            continue;
        }

        /* ── Failed login ───────────────────────────────────────── */
        put_str("\r\nLogin incorrect\r\n");
        attempt++;
    }

    /* Too many failed attempts */
    put_str("\r\nToo many failed login attempts.  Exiting.\r\n");
    getty_running = 0;
}

/* ── Public command entry point ─────────────────────────────────────── */

void cmd_getty(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: getty [baud] [line] [term]\n");
        kprintf("       getty stop\n");
        kprintf("       getty status\n");
        return;
    }

    char tok0[32];
    const char *p = args;
    while (*p == ' ') p++;

    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(tok0) - 1)
        tok0[i++] = *p++;
    tok0[i] = '\0';

    /* ── stop: terminate the getty daemon ───────────────────────── */
    if (strcmp(tok0, "stop") == 0) {
        if (getty_pid > 0) {
            libc_kill((uint32_t)getty_pid, 15);  /* SIGTERM */
            getty_running = 0;
            getty_pid = 0;
            kprintf("getty: stopped\n");
        } else {
            kprintf("getty: not running\n");
        }
        return;
    }

    /* ── status: show daemon state ──────────────────────────────── */
    if (strcmp(tok0, "status") == 0) {
        kprintf("getty: %s (pid=%d)\n",
                getty_running ? "running" : "stopped",
                getty_pid);
        return;
    }

    /* ── Otherwise, start the getty login loop ──────────────────── */
    kprintf("getty: starting login loop on console...\n");

    /* Record our PID for stop/status */
    getty_pid = (int)libc_syscall(SYS_GETPID, 0, 0, 0, 0, 0);

    /* Run the login loop (blocks until too many failures) */
    getty_login_loop();

    /* If we get here, too many failures — exit */
    getty_pid = 0;
    kprintf("getty: exiting after too many failed login attempts\n");
}
