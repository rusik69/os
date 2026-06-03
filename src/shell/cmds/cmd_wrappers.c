/* cmd_wrappers.c — Adapter wrappers for commands using argc/argv style
 *
 * The shell dispatch calls all commands as void (*)(const char *args).
 * Some newer commands were written with int (*)(int argc, char **argv).
 * This file provides wrapper functions that convert args to argc/argv
 * and forward to the original implementation.
 */
#include "shell_cmds.h"
#include "string.h"
#include "libc.h"
#include "printf.h"
#include "stdlib.h"

/* ── Helper: tokenize a const char *args string into argc/argv ────── */
#define ARGV_MAX 64
#define ARGV_BUF_SIZE 512

static int args_to_argv(const char *args, int *out_argc, char **out_argv)
{
    static char buf[ARGV_BUF_SIZE];
    static char *argv[ARGV_MAX];
    int argc = 0;

    if (!args) {
        *out_argc = 0;
        *out_argv = NULL;
        return 0;
    }

    /* Copy to mutable buffer */
    size_t len = strlen(args);
    if (len >= ARGV_BUF_SIZE) len = ARGV_BUF_SIZE - 1;
    memcpy(buf, args, len);
    buf[len] = '\0';

    /* Tokenize by spaces/tabs */
    char *p = buf;
    while (*p && argc < ARGV_MAX - 1) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        argv[argc++] = p;

        /* Skip to next whitespace */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;

    *out_argc = argc;
    *out_argv = argv;
    return argc;
}

/* ── Wrappers for int (*)(int, char **) commands ──────────────────── */

void cmd_comm_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_comm(argc, argv);
}

void cmd_expand_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_expand(argc, argv);
}

void cmd_fold_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_fold(argc, argv);
}

void cmd_seq_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_seq(argc, argv);
}

void cmd_tee_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_tee(argc, argv);
}

void cmd_yes_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_yes(argc, argv);
}

void cmd_tsort_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_tsort(argc, argv);
}

void cmd_join_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_join(argc, argv);
}

void cmd_unexpand_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_unexpand(argc, argv);
}

void cmd_fmt_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_fmt(argc, argv);
}

void cmd_pr_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_pr(argc, argv);
}

void cmd_base32_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_base32(argc, argv);
}

/* ── Wrappers for void (*)(void) commands ─────────────────────────── */

void cmd_neofetch_wrapper(const char *args) {
    (void)args;
    cmd_neofetch();
}
