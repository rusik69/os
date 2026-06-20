/* cmd_hostname.c — display or set system hostname
 *
 * Usage:
 *   hostname              — print the current hostname
 *   hostname <name>       — set hostname to <name>
 *   hostname -F <file>    — read hostname from file
 *   hostname --file <file>
 *
 * The boot sequence reads /etc/hostname and sets the kernel's
 * hostname before services start.  This command allows runtime
 * query and changes, and can be used by init scripts to restore
 * the hostname from the persistent configuration file.
 */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "sysctl.h"

/*
 * Minimal argument parsing for a single-arg or -F flag.
 * We don't have full argc/argv parsing in the shell command
 * layer, so we parse the raw argument string ourselves.
 */
static void skip_whitespace(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

/* Extract the next whitespace-delimited token into buf.
 * Returns the length, or 0 if no token found. */
static int next_token(const char **p, char *buf, int max) {
    skip_whitespace(p);
    if (**p == '\0') return 0;
    int i = 0;
    while (**p && **p != ' ' && **p != '\t' && i < max - 1) {
        buf[i++] = **p;
        (*p)++;
    }
    buf[i] = '\0';
    return i;
}

void cmd_hostname(const char *args) {
    /* ── No arguments: print current hostname ──────────────────── */
    if (!args || *args == '\0') {
        kprintf("%s\n", sysctl_get_hostname());
        return;
    }

    char tok0[64], tok1[64];
    const char *p = args;

    /* Read first token */
    if (next_token(&p, tok0, sizeof(tok0)) == 0) {
        kprintf("%s\n", sysctl_get_hostname());
        return;
    }

    /* Check for flags */
    if (strcmp(tok0, "-F") == 0 || strcmp(tok0, "--file") == 0) {
        /* Read filename from next token */
        if (next_token(&p, tok1, sizeof(tok1)) == 0) {
            kprintf("hostname: -F requires a file argument\n");
            return;
        }

        char buf[128];
        uint32_t out_size = 0;
        if (libc_fs_read_file(tok1, buf, sizeof(buf) - 1, &out_size) != 0) {
            kprintf("hostname: cannot read '%s'\n", tok1);
            return;
        }
        if (out_size == 0) {
            kprintf("hostname: file '%s' is empty\n", tok1);
            return;
        }
        buf[out_size] = '\0';
        sysctl_set_hostname(buf);
        return;
    }

    if (strcmp(tok0, "--help") == 0 || strcmp(tok0, "-h") == 0) {
        kprintf("Usage: hostname [name|-F file|--file file]\n");
        return;
    }

    /* ── Set hostname from argument ─────────────────────────────── */
    sysctl_set_hostname(tok0);
}
