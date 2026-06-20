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

static int args_to_argv(const char *args, int *out_argc, char ***out_argv)
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

/* ── Batch B51-B100 orphan command wrappers ─────────────────────── */
void cmd_ctr_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_ctr(argc, argv);
}
void cmd_crictl_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_crictl(argc, argv);
}
void cmd_orchctl_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_orchctl(argc, argv);
}
void cmd_compose_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_compose(argc, argv);
}
void cmd_depmod_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_depmod(argc, argv);
}
void cmd_adjtimex_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_adjtimex(argc, argv);
}
void cmd_capsh_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_capsh(argc, argv);
}
void cmd_dumpleases_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_dumpleases(argc, argv);
}
void cmd_envdir_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_envdir(argc, argv);
}
void cmd_fatattr_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_fatattr(argc, argv);
}
void cmd_fbset_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_fbset(argc, argv);
}
void cmd_fsfreeze_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_fsfreeze(argc, argv);
}
void cmd_ftpget_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_ftpget(argc, argv);
}
void cmd_ftpput_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_ftpput(argc, argv);
}
void cmd_getopt_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_getopt(argc, argv);
}
void cmd_ifplugd_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_ifplugd(argc, argv);
}
void cmd_ionice_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_ionice(argc, argv);
}
void cmd_mkfs_ext2_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_mkfs_ext2(argc, argv);
}
void cmd_mkswap_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_mkswap(argc, argv);
}

/* ── Additional orphan wrappers for int (int, char**) cmds ──────── */
void cmd_chvt_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_chvt(argc, argv);
}
void cmd_eject_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_eject(argc, argv);
}
void cmd_dc_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_dc(argc, argv);
}
void cmd_dnsdomainname_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_dnsdomainname(argc, argv);
}
void cmd_dos2unix_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_dos2unix(argc, argv);
}
void cmd_fgconsole_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_fgconsole(argc, argv);
}
void cmd_cpio_wrapper(const char *args) {
    int argc; char **argv;
    args_to_argv(args, &argc, &argv);
    cmd_cpio(argc, argv);
}

/* ── Wrappers for void (*)(void) commands ─────────────────────────── */

void cmd_neofetch_wrapper(const char *args) {
    (void)args;
    cmd_neofetch();
}
