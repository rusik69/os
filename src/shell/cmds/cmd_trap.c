/* cmd_trap.c — trap command: register a shell command to run on a signal */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

/*
 * Usage:
 *   trap 'command'  SIGNUM   — register command for signal number
 *   trap 'command'  SIGNAME  — SIGTERM(15), SIGINT(2), SIGUSR1(10), SIGUSR2(12)
 *   trap -          SIGNUM   — reset to default
 *   trap            — list registered traps
 *
 * The handler executes the stored command string via shell_exec_cmd when the
 * signal is delivered to the current process.
 */

#define TRAP_MAX   8
#define TRAP_CMD_MAX 64

static struct {
    int  signum;
    char cmd[TRAP_CMD_MAX];
    int  active;
} trap_table[TRAP_MAX];

/* The signal handler set via libc_signal: runs the stored command */
static void trap_handler(int sig) {
    for (int i = 0; i < TRAP_MAX; i++) {
        if (trap_table[i].active && trap_table[i].signum == sig) {
            /* Execute stored command via shell */
            libc_shell_exec_cmd(trap_table[i].cmd, NULL);
            return;
        }
    }
}

static int signame_to_num(const char *s) {
    if (s[0] >= '0' && s[0] <= '9') {
        int n = 0;
        while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
        return n;
    }
    if (strcmp(s, "SIGTERM") == 0 || strcmp(s, "TERM") == 0) return 15;
    if (strcmp(s, "SIGINT")  == 0 || strcmp(s, "INT")  == 0) return  2;
    if (strcmp(s, "SIGHUP")  == 0 || strcmp(s, "HUP")  == 0) return  1;
    if (strcmp(s, "SIGUSR1") == 0 || strcmp(s, "USR1") == 0) return 10;
    if (strcmp(s, "SIGUSR2") == 0 || strcmp(s, "USR2") == 0) return 12;
    if (strcmp(s, "SIGKILL") == 0 || strcmp(s, "KILL") == 0) return  9;
    return -1;
}

void cmd_trap(const char *args) {
    if (!args || !*args) {
        /* List traps */
        kprintf("%-6s  %s\n", "SIG", "COMMAND");
        for (int i = 0; i < TRAP_MAX; i++) {
            if (trap_table[i].active)
                kprintf("%-6d  %s\n", (uint64_t)trap_table[i].signum, trap_table[i].cmd);
        }
        return;
    }

    /* Parse: trap 'cmd' SIGNAL  or  trap cmd SIGNAL (after quote removal) */
    char trap_cmd[TRAP_CMD_MAX];
    int ci = 0;
    const char *sig_str;

    if (*args == '\'') {
        const char *p = args + 1;
        while (*p && *p != '\'' && ci < TRAP_CMD_MAX - 1)
            trap_cmd[ci++] = *p++;
        trap_cmd[ci] = '\0';
        if (*p == '\'') p++;
        while (*p == ' ') p++;
        sig_str = p;
    } else {
        int len = (int)strlen(args);
        while (len > 0 && args[len - 1] == ' ') len--;
        int sig_at = len;
        while (sig_at > 0 && args[sig_at - 1] != ' ') sig_at--;
        if (sig_at <= 0 || sig_at >= len) {
            kprintf("Usage: trap 'cmd' SIGNAL\n");
            return;
        }
        int cmd_len = sig_at;
        while (cmd_len > 0 && args[cmd_len - 1] == ' ') cmd_len--;
        if (cmd_len >= TRAP_CMD_MAX) cmd_len = TRAP_CMD_MAX - 1;
        memcpy(trap_cmd, args, cmd_len);
        trap_cmd[cmd_len] = '\0';
        sig_str = args + sig_at;
        while (*sig_str == ' ') sig_str++;
    }

    if (!trap_cmd[0]) { kprintf("Usage: trap 'cmd' SIGNAL\n"); return; }
    if (!*sig_str) { kprintf("Usage: trap 'cmd' SIGNAL\n"); return; }
    int sig = signame_to_num(sig_str);
    if (sig <= 0) { kprintf("trap: unknown signal: %s\n", sig_str); return; }

    int is_reset = (trap_cmd[0] == '-' && trap_cmd[1] == '\0');

    if (is_reset) {
        /* Remove trap */
        for (int i = 0; i < TRAP_MAX; i++) {
            if (trap_table[i].active && trap_table[i].signum == sig) {
                trap_table[i].active = 0;
                break;
            }
        }
        libc_signal(sig, (void (*)(int))0);  /* SIG_DFL */
        return;
    }

    /* Register trap */
    int slot = -1;
    for (int i = 0; i < TRAP_MAX; i++) {
        if (!trap_table[i].active && slot < 0) slot = i;
        if (trap_table[i].active && trap_table[i].signum == sig) { slot = i; break; }
    }
    if (slot < 0) { kprintf("trap: table full\n"); return; }
    trap_table[slot].signum = sig;
    trap_table[slot].active = 1;
    strncpy(trap_table[slot].cmd, trap_cmd, TRAP_CMD_MAX - 1);
    trap_table[slot].cmd[TRAP_CMD_MAX - 1] = '\0';

    libc_signal(sig, trap_handler);
    kprintf("trap: signal %d -> '%s'\n", (uint64_t)sig, trap_table[slot].cmd);
}
