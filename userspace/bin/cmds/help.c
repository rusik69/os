/* help.c — show help: list available commands */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("HermesOS Userspace Commands\n");
    printf("Available commands: cat, ls, cp, mv, rm, mkdir, rmdir, touch, chmod,\n");
    printf("  echo, printf, yes, true, false, sleep, clear, ps, kill, uname,\n");
    printf("  uptime, free, df, dmesg, whoami, id, hostname, who, tty, arch,\n");
    printf("  dir, cal, help, exec, dirname, basename, tee, head, tail, wc,\n");
    printf("  sort, uniq, cut, tr, diff, grep, find, env, sh, and more.\n");
    printf("Type 'help <command>' for help on a specific command.\n");
    return 0;
}
